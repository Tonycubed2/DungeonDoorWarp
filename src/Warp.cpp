// Dungeon Door Warp
//
// Two ways to skip backtracking inside dungeons (interiors only):
//
//   1. Map warp: open the local map, hover a load door of the level you are on, click.
//      You are placed right in front of that door (inside this level, facing away from it),
//      exactly where the game puts you when you come through it. You then walk through yourself.
//      Allowed only for doors you have reached before, or doors leading to a level you have visited.
//
//   2. Entrance hotkey: teleports you back to the spot just inside the dungeon's first door,
//      from any level of the same dungeon.
//
// Followers (teammates) and your summons come along. Plain teleport, no fast travel, no time passes.

#include "Warp.h"
#include "Settings.h"

#include <deque>

namespace Warp
{
	namespace
	{
		// ------------------------------------------------------------------
		// State. Everything here is touched only on the main thread
		// (SKSE tasks, the input sink and the co-save callbacks all run there).
		// ------------------------------------------------------------------

		struct Entrance
		{
			bool         valid = false;
			RE::FormID   cell = 0;  // interior cell just inside the dungeon
			RE::FormID   door = 0;  // the entrance door in that cell (also the MoveTo anchor)
			RE::NiPoint3 pos{};     // landing spot in front of that door
			RE::NiPoint3 rot{};
		};

		std::unordered_set<RE::FormID> g_reachedDoors;  // load doors the player has been near
		std::unordered_set<RE::FormID> g_visitedCells;  // dungeon interior cells the player has been in
		Entrance                       g_entrance;

		bool       g_haveLastCell = false;
		RE::FormID g_lastCell = 0;
		bool       g_lastInterior = false;

		std::vector<RE::FormID> g_cellDoors;  // load doors in the current cell
		std::uint32_t           g_doorScanCountdown = 0;

		std::vector<RE::BGSKeyword*> g_dungeonKeywords;

		std::atomic<bool> g_tickQueued{ false };
		std::atomic<bool> g_warpInProgress{ false };

		std::array<bool, 512> g_down{};  // held keys / mouse buttons, for modifiers

		std::chrono::steady_clock::time_point g_confirmUntil{};

		constexpr auto kTickInterval = std::chrono::milliseconds(250);

		// ------------------------------------------------------------------
		// Small helpers
		// ------------------------------------------------------------------

		void Notify(const char* a_text)
		{
			logger::info("Notice: {}", a_text);
			if (Settings::notifications) {
				RE::SendHUDMessage::ShowHUDMessage(a_text);
			}
		}

		RE::DoorTeleportData* GetTeleportData(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return nullptr;
			}
			auto* extra = a_ref->extraList.GetByType<RE::ExtraTeleport>();
			return extra ? extra->teleportData : nullptr;
		}

		bool IsLoadDoor(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled()) {
				return false;
			}
			auto* base = a_ref->GetBaseObject();
			return base && base->Is(RE::FormType::Door) && GetTeleportData(a_ref) != nullptr;
		}

		RE::NiPointer<RE::TESObjectREFR> GetLinkedDoor(RE::TESObjectREFR* a_door)
		{
			auto* data = GetTeleportData(a_door);
			return data ? data->linkedDoor.get() : RE::NiPointer<RE::TESObjectREFR>{};
		}

		// The spot in front of a_door, inside a_door's cell: it is stored on the partner door,
		// because that is where the game drops you when you come through the partner.
		bool GetLandingAtDoor(RE::TESObjectREFR* a_door, RE::NiPoint3& a_pos, RE::NiPoint3& a_rot)
		{
			auto partner = GetLinkedDoor(a_door);
			auto* data = GetTeleportData(partner.get());
			if (!data) {
				return false;
			}
			// Sanity check: the landing must be close to the door we clicked
			if (data->position.GetDistance(a_door->GetPosition()) > 1500.0f) {
				logger::warn("Door {:08X}: partner's landing spot is {:.0f} units away, ignoring it",
					a_door->GetFormID(), data->position.GetDistance(a_door->GetPosition()));
				return false;
			}
			a_pos = data->position;
			a_rot = data->rotation;
			return true;
		}

		bool CellQualifies(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell || !a_cell->IsInteriorCell()) {
				return false;
			}
			if (!Settings::dungeonsOnly || g_dungeonKeywords.empty()) {
				return true;
			}
			auto* location = a_cell->GetLocation();
			for (int depth = 0; location && depth < 8; ++depth, location = location->parentLoc) {
				for (auto* keyword : g_dungeonKeywords) {
					if (location->HasKeyword(keyword)) {
						return true;
					}
				}
			}
			return false;
		}

		std::vector<RE::TESObjectREFR*> CollectLoadDoors(RE::TESObjectCELL* a_cell)
		{
			std::vector<RE::TESObjectREFR*> doors;
			if (!a_cell) {
				return doors;
			}
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (IsLoadDoor(a_ref)) {
					doors.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return doors;
		}

		std::string Describe(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return "<none>";
			}
			const char* name = a_ref->GetDisplayFullName();
			return std::format("{:08X} '{}'", a_ref->GetFormID(), name ? name : "");
		}

		// The engine's Actor::IsDead() gave "dead" for living followers in testing; read the life state instead
		bool IsActorDead(RE::Actor* a_actor)
		{
			auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!state) {
				return true;
			}
			const auto life = state->GetLifeState();
			return life == RE::ACTOR_LIFE_STATE::kDead || life == RE::ACTOR_LIFE_STATE::kDying;
		}

		// The game's own MoveTo (the one behind Papyrus MoveTo / MoveToEditorLocation),
		// with an explicit position and rotation. a_anchor must be a reference in a_cell.
		void MoveRefTo(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_anchor, RE::TESObjectCELL* a_cell,
			const RE::NiPoint3& a_pos, const RE::NiPoint3& a_rot)
		{
			using func_t = void(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*,
				RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
			static REL::Relocation<func_t> func{ RELOCATION_ID(56227, 56626) };

			const RE::ObjectRefHandle handle = a_anchor->GetHandle();
			RE::TESWorldSpace*        worldSpace = (a_cell && a_cell->IsInteriorCell()) ? nullptr : a_anchor->GetWorldspace();
			func(a_ref, handle, a_cell, worldSpace, a_pos, a_rot);
		}

		// Moving to another level: Papyrus ObjectReference.MoveTo, the path scripted teleports use.
		// It lets the game run its own fade / loading screen sequence. Offsets are from the anchor door.
		bool PapyrusMoveTo(RE::PlayerCharacter* a_player, RE::TESObjectREFR* a_anchor, const RE::NiPoint3& a_pos)
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			if (!policy) {
				return false;
			}
			const auto handle = policy->GetHandleForObject(a_player->GetFormType(), a_player);
			if (handle == policy->EmptyHandle()) {
				return false;
			}

			const auto anchorPos = a_anchor->GetPosition();
			auto*      args = RE::MakeFunctionArguments(
				static_cast<RE::TESObjectREFR*>(a_anchor),
				static_cast<float>(a_pos.x - anchorPos.x),
				static_cast<float>(a_pos.y - anchorPos.y),
				static_cast<float>(a_pos.z - anchorPos.z),
				false);

			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
			const RE::BSFixedString                                  className("ObjectReference");
			const RE::BSFixedString                                  functionName("MoveTo");
			const bool                                               queued = vm->DispatchMethodCall(handle, className, functionName, args, callback);
			logger::info("Papyrus MoveTo to {} {}", Describe(a_anchor), queued ? "queued" : "FAILED");
			return queued;
		}

		// Polls a_condition on the main thread every a_interval until it is true (then runs a_action
		// there) or a_timeout passes (then runs a_onTimeout there, if given).
		void RunWhen(std::function<bool()> a_condition, std::function<void()> a_action,
			std::function<void()> a_onTimeout, std::chrono::milliseconds a_timeout,
			std::chrono::milliseconds a_interval = std::chrono::milliseconds(50))
		{
			std::thread([=]() {
				// 0 = idle, 1 = check queued, 2 = done
				auto       state = std::make_shared<std::atomic<int>>(0);
				const auto deadline = std::chrono::steady_clock::now() + a_timeout;

				while (std::chrono::steady_clock::now() < deadline) {
					std::this_thread::sleep_for(a_interval);
					if (state->load() == 2) {
						return;
					}
					int expected = 0;
					if (!state->compare_exchange_strong(expected, 1)) {
						continue;  // previous check still queued
					}
					SKSE::GetTaskInterface()->AddTask([state, a_condition, a_action]() {
						if (a_condition()) {
							a_action();
							state->store(2);
						} else {
							state->store(0);
						}
					});
				}

				// Let a queued check finish before deciding it timed out (bounded wait)
				for (int i = 0; i < 300 && state->load() == 1; ++i) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				if (state->load() != 2 && a_onTimeout) {
					SKSE::GetTaskInterface()->AddTask(a_onTimeout);
				}
			}).detach();
		}

		bool IsLoading()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		// ------------------------------------------------------------------
		// Followers and summons
		// ------------------------------------------------------------------

		// Followers: player teammates, or members of CurrentFollowerFaction (rank 0 or higher) - follower
		// frameworks and custom followers use one or the other. Summons: actors commanded by the player.
		// All of them on the player's level come along (GatherRadius > 0 limits the distance).
		std::vector<RE::ObjectRefHandle> GatherCompanions(RE::PlayerCharacter* a_player)
		{
			std::vector<RE::ObjectRefHandle> result;
			if (!Settings::moveFollowers && !Settings::moveSummons) {
				return result;
			}
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return result;
			}

			static auto* followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction

			const auto  playerPos = a_player->GetPosition();
			const auto* playerCell = a_player->GetParentCell();
			const float maxDist2 = Settings::followerGatherRadius * Settings::followerGatherRadius;
			std::unordered_set<RE::FormID> seen;

			auto consider = [&](RE::Actor* a_actor) {
				if (!a_actor || a_actor == a_player || !seen.insert(a_actor->GetFormID()).second) {
					return;
				}

				const bool teammate = a_actor->IsPlayerTeammate();
				const bool inFaction = followerFaction && a_actor->GetFactionRank(followerFaction, false) >= 0;
				bool       summon = false;
				if (a_actor->IsCommandedActor()) {
					auto commander = a_actor->GetCommandingActor();
					summon = commander && commander.get() == a_player;
				}

				const bool follower = Settings::moveFollowers && (teammate || inFaction);
				if (!follower && !(Settings::moveSummons && summon)) {
					return;
				}

				const char* skip = nullptr;
				if (IsActorDead(a_actor)) {
					skip = "dead";
				} else if (a_actor->IsDisabled()) {
					skip = "disabled";
				} else if (a_actor->IsOnMount()) {
					skip = "mounted";
				} else if (a_actor->GetParentCell() != playerCell) {
					skip = "on another level";
				} else if (Settings::followerGatherRadius > 0.0f && a_actor->GetPosition().GetSquaredDistance(playerPos) > maxDist2) {
					skip = "farther than GatherRadius";
				} else if (!summon && Settings::skipWaitingFollowers) {
					auto* avOwner = a_actor->AsActorValueOwner();
					if (avOwner && avOwner->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) {
						skip = "told to wait";
					}
				}

				const auto how = std::format("{}{}{}", teammate ? " teammate" : "", inFaction ? " follower-faction" : "", summon ? " summon" : "");
				if (skip) {
					logger::info("  not bringing {} ({}):{}", Describe(a_actor), skip, how);
					return;
				}
				result.push_back(static_cast<RE::TESObjectREFR*>(a_actor)->GetHandle());
				logger::info("  bringing {} ({:.0f} units away):{}", Describe(a_actor),
					std::sqrt(a_actor->GetPosition().GetSquaredDistance(playerPos)), how);
			};

			for (auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (auto& handle : *handles) {
					auto actor = handle.get();
					consider(actor.get());
				}
			}

			logger::info("Companions found: {}", result.size());
			return result;
		}

		void MoveCompanions(const std::vector<RE::ObjectRefHandle>& a_companions)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return;
			}

			const auto  playerPos = player->GetPosition();
			const auto  playerRot = player->GetAngle();
			const float heading = player->GetAngleZ();
			const auto  count = a_companions.size();

			std::size_t index = 0;
			std::size_t moved = 0;
			for (const auto& handle : a_companions) {
				auto  ref = handle.get();
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor || IsActorDead(actor)) {
					continue;
				}

				// Spread in a shallow arc in front of the player: the door is right behind the landing spot
				float angle = heading;
				if (count > 1) {
					angle += -0.7f + 1.4f * static_cast<float>(index) / static_cast<float>(count - 1);
				}
				RE::NiPoint3 pos = playerPos;
				pos.x += std::sin(angle) * Settings::followerOffset;
				pos.y += std::cos(angle) * Settings::followerOffset;

				MoveRefTo(actor, player, cell, pos, playerRot);
				++index;
				++moved;
			}
			logger::info("Moved {} companion(s)", moved);
		}

		// ------------------------------------------------------------------
		// Teleport
		// ------------------------------------------------------------------

		struct WarpTarget
		{
			RE::FormID   cell = 0;
			RE::FormID   anchor = 0;  // a load door in the target cell
			RE::NiPoint3 pos{};
			RE::NiPoint3 rot{};
		};

		void StartWarp(const WarpTarget& a_target, bool a_closeMap, const char* a_what)
		{
			if (g_warpInProgress.exchange(true)) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			logger::info("Warp ({}) to cell {:08X} at ({:.0f}, {:.0f}, {:.0f})",
				a_what, a_target.cell, a_target.pos.x, a_target.pos.y, a_target.pos.z);

			// Collect companions now, while they are still next to the player
			auto companions = GatherCompanions(player);

			if (a_closeMap) {
				if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
					queue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
				}
			}

			// Teleport once the map is closed (moving the player from inside the open map is asking for trouble)
			RunWhen(
				[]() {
					auto* ui = RE::UI::GetSingleton();
					return ui && !ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
				},
				[a_target, companions]() {
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_target.cell);
					auto* anchor = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_target.anchor);
					if (!player || !cell || !anchor) {
						logger::error("Warp target vanished (cell {:08X}, anchor {:08X})", a_target.cell, a_target.anchor);
						g_warpInProgress = false;
						return;
					}

					const bool sameCell = player->GetParentCell() == cell;
					if (sameCell) {
						MoveRefTo(player, anchor, cell, a_target.pos, a_target.rot);
					} else if (!PapyrusMoveTo(player, anchor, a_target.pos)) {
						// Papyrus not available: the direct call (same as same-level warps)
						logger::warn("Papyrus MoveTo could not be started; using the direct call");
						MoveRefTo(player, anchor, cell, a_target.pos, a_target.rot);
					}

					// Once the player has arrived: exact spot and facing, then companions
					const auto target = a_target;
					RunWhen(
						[target]() {
							auto* player = RE::PlayerCharacter::GetSingleton();
							auto* cell = player ? player->GetParentCell() : nullptr;
							return cell && cell->GetFormID() == target.cell && cell->IsAttached() && !IsLoading() &&
							       player->GetPosition().GetDistance(target.pos) < 600.0f;
						},
						[target, companions, sameCell]() {
							auto* player = RE::PlayerCharacter::GetSingleton();
							if (!sameCell) {
								logger::info("Arrived in cell {:08X}", target.cell);
								auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(target.cell);
								auto* anchor = RE::TESForm::LookupByID<RE::TESObjectREFR>(target.anchor);
								if (player && cell && anchor) {
									MoveRefTo(player, anchor, cell, target.pos, target.rot);  // same level now: exact spot and facing
								}
							}
							if (!companions.empty()) {
								MoveCompanions(companions);
							}
							g_warpInProgress = false;
						},
						[]() {
							logger::warn("Player did not arrive in time; companions not moved");
							g_warpInProgress = false;
						},
						std::chrono::milliseconds(30000), std::chrono::milliseconds(100));
				},
				[]() {
					logger::warn("Map did not close; warp cancelled");
					g_warpInProgress = false;
				},
				std::chrono::milliseconds(3000));
		}

		// BlockInCombat=1: only a live enemy in this cell, close by, that is targeting the player counts.
		// The game's own combat flag (BlockInCombat=2) stays on while anything anywhere is still
		// searching for you, even after the fight is over.
		bool InCombat(RE::PlayerCharacter* a_player)
		{
			if (Settings::blockInCombat == 0) {
				return false;
			}

			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return false;
			}

			// 1 = a living enemy on this level within CombatRadius has the player as its combat target
			// 2 = any loaded living actor has the player as its combat target
			// (The engine's own "in combat" call is not used: it gave wrong answers in testing.)
			const bool  anywhere = Settings::blockInCombat == 2;
			const auto  playerPos = a_player->GetPosition();
			const auto* playerCell = a_player->GetParentCell();
			const float maxDist2 = Settings::combatRadius * Settings::combatRadius;

			for (auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (auto& handle : *handles) {
					auto  actorPtr = handle.get();
					auto* actor = actorPtr.get();
					if (!actor || actor == a_player || IsActorDead(actor) || actor->IsDisabled()) {
						continue;
					}
					auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
					if (!target || target.get() != a_player) {
						continue;
					}
					const float dist2 = actor->GetPosition().GetSquaredDistance(playerPos);
					if (!anywhere && (actor->GetParentCell() != playerCell || dist2 > maxDist2)) {
						if (Settings::debugLog) {
							logger::info("  {} targets the player but is {:.0f} units away / elsewhere: ignored",
								Describe(actor), std::sqrt(dist2));
						}
						continue;
					}
					logger::info("Blocked by combat: {} is fighting the player ({:.0f} units away)", Describe(actor), std::sqrt(dist2));
					return true;
				}
			}
			return false;
		}

		bool CanWarpHere(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_cell)
		{
			if (g_warpInProgress) {
				return false;
			}
			if (!a_cell || !a_cell->IsInteriorCell()) {
				Notify("Dungeon Door Warp only works indoors.");
				return false;
			}
			if (!CellQualifies(a_cell)) {
				Notify("Dungeon Door Warp only works in dungeons.");
				return false;
			}
			if (InCombat(a_player)) {
				Notify("You can't warp while an enemy is fighting you.");
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------------
		// 1. Map warp
		// ------------------------------------------------------------------

		bool LocalMapShown(RE::LocalMapMenu& a_local)
		{
			auto& data = a_local.GetRuntimeData();
			switch (Settings::localMapCheck) {
			case 0:
				return true;
			case 2:
				{
					RE::GFxValue visible;
					if (data.localMapMovie.IsObject() && data.localMapMovie.GetMember("_visible", &visible) && visible.IsBool()) {
						return visible.GetBool();
					}
					return data.showingMap;
				}
			default:
				return data.showingMap;
			}
		}

		void LogMapState(RE::LocalMapMenu& a_local)
		{
			auto&        data = a_local.GetRuntimeData();
			RE::GFxValue visible;
			std::string  visibleText = "n/a";
			if (data.localMapMovie.IsObject() && data.localMapMovie.GetMember("_visible", &visible) && visible.IsBool()) {
				visibleText = visible.GetBool() ? "true" : "false";
			}
			logger::info("Map click: showingMap={} movie._visible={} selectedMarker={} markers={}",
				data.showingMap, visibleText, data.selectedMarker, a_local.mapMarkers.size());

			for (std::uint32_t i = 0; i < a_local.mapMarkers.size(); ++i) {
				const auto& marker = a_local.mapMarkers[i];
				auto        ref = RE::TESObjectREFR::LookupByHandle(marker.ref);
				logger::info("  [{}] type={} door={} ref={}{}", i, marker.type, marker.door, Describe(ref.get()),
					ref && IsLoadDoor(ref.get()) ? " (load door)" : "");
			}
		}

		RE::NiPointer<RE::TESObjectREFR> FindHoveredDoor()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
				return {};
			}
			auto menu = ui->GetMenu<RE::MapMenu>();
			if (!menu) {
				return {};
			}
			auto* runtime = menu->GetRuntimeData();
			if (!runtime) {
				return {};
			}
			auto& local = runtime->localMapMenu;

			if (Settings::debugLog) {
				LogMapState(local);
			}
			if (!LocalMapShown(local)) {
				return {};
			}

			const auto index = local.GetRuntimeData().selectedMarker;
			if (index < 0 || static_cast<std::uint32_t>(index) >= local.mapMarkers.size()) {
				return {};
			}

			auto ref = RE::TESObjectREFR::LookupByHandle(local.mapMarkers[static_cast<std::uint32_t>(index)].ref);
			if (!ref || !IsLoadDoor(ref.get())) {
				return {};
			}
			return ref;
		}

		bool DoorAllowed(RE::TESObjectREFR* a_door)
		{
			if ((Settings::visitRule & 1) && g_reachedDoors.contains(a_door->GetFormID())) {
				return true;
			}
			if (Settings::visitRule & 2) {
				auto  partner = GetLinkedDoor(a_door);
				auto* otherSide = partner ? partner->GetParentCell() : nullptr;
				if (otherSide && otherSide->IsInteriorCell() && g_visitedCells.contains(otherSide->GetFormID())) {
					return true;
				}
			}
			return false;
		}

		// A click while the local map is open. Returns true when the click is ours (a door marker of the
		// level you are on, in a dungeon): then it must not reach the map, so neither the game nor
		// LocalMapUpgrade start placing a custom marker (LocalMapUpgrade's "place marker" box crashes
		// if the map closes under it).
		bool HandleMapClick()
		{
			auto door = FindHoveredDoor();
			if (!door) {
				return false;  // an ordinary map click
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !cell) {
				return false;
			}
			if (door->GetParentCell() != cell) {
				if (Settings::debugLog) {
					logger::info("Hovered door {} is not in the player's cell", Describe(door.get()));
				}
				return false;
			}
			if (!cell->IsInteriorCell() || !CellQualifies(cell)) {
				return false;  // not a dungeon: leave the map alone
			}
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
				logger::info("A message box is open over the map; click left alone");
				return false;
			}

			// From here on the click is ours, whether or not the warp is allowed
			if (!CanWarpHere(player, cell)) {
				return true;
			}
			if (!DoorAllowed(door.get())) {
				Notify("You haven't been to that door yet.");
				return true;
			}

			WarpTarget target;
			if (!GetLandingAtDoor(door.get(), target.pos, target.rot)) {
				Notify("Can't find a safe spot at that door.");
				return true;
			}
			target.cell = cell->GetFormID();
			target.anchor = door->GetFormID();

			logger::info("Map warp to door {}", Describe(door.get()));
			StartWarp(target, true, "map door");
			return true;
		}

		// ------------------------------------------------------------------
		// 2. Entrance hotkey
		// ------------------------------------------------------------------

		void TryEntranceWarp()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME) || IsLoading()) {
				return;  // in a menu: leave the key alone
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!player || !CanWarpHere(player, cell)) {
				return;
			}
			if (!g_entrance.valid) {
				Notify("No entrance recorded for this dungeon.");
				return;
			}

			auto* entranceCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_entrance.cell);
			auto* anchor = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_entrance.door);
			if (!entranceCell || !anchor) {
				Notify("The dungeon entrance can't be found.");
				return;
			}
			if (entranceCell == cell && player->GetPosition().GetDistance(g_entrance.pos) < 300.0f) {
				Notify("You are already at the entrance.");
				return;
			}

			if (Settings::entranceConfirm) {
				const auto now = std::chrono::steady_clock::now();
				if (now > g_confirmUntil) {
					g_confirmUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
						std::chrono::duration<float>(Settings::entranceConfirmSeconds));
					Notify("Press again to return to the dungeon entrance.");
					return;
				}
				g_confirmUntil = {};
			}

			WarpTarget target;
			target.cell = g_entrance.cell;
			target.anchor = g_entrance.door;
			target.pos = g_entrance.pos;
			target.rot = g_entrance.rot;
			StartWarp(target, false, "dungeon entrance");
		}

		// ------------------------------------------------------------------
		// Input
		// ------------------------------------------------------------------

		bool ModifierHeld(std::uint32_t a_code)
		{
			return a_code == 0 || (a_code < g_down.size() && g_down[a_code]);
		}

		// Returns true when the press must be swallowed
		bool OnKeyDown(std::uint32_t a_code)
		{
			bool swallow = false;
			if (Settings::mapWarpEnabled && Settings::mapWarpButton != 0 &&
				a_code == Settings::mapWarpButton && ModifierHeld(Settings::mapWarpModifier)) {
				auto* ui = RE::UI::GetSingleton();
				if (ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
					swallow = HandleMapClick();
				}
			}
			if (Settings::entranceEnabled && Settings::entranceHotkey != 0 &&
				a_code == Settings::entranceHotkey && ModifierHeld(Settings::entranceModifier)) {
				TryEntranceWarp();
			}
			return swallow;
		}

		std::array<bool, 512> g_swallowing{};  // buttons whose events are dropped until released
		bool                  g_hookInstalled = false;

		// Returns true when this event must not reach the game
		bool ProcessInputEvent(RE::InputEvent* a_event)
		{
			auto* button = a_event ? a_event->AsButtonEvent() : nullptr;
			if (!button) {
				return false;
			}

			std::uint32_t code = 0;
			switch (a_event->GetDevice()) {
			case RE::INPUT_DEVICE::kKeyboard:
				code = button->GetIDCode();
				break;
			case RE::INPUT_DEVICE::kMouse:
				code = 256 + button->GetIDCode();
				break;
			default:
				return false;
			}
			if (code >= g_down.size()) {
				return false;
			}

			if (button->IsDown()) {
				g_down[code] = true;
				g_swallowing[code] = false;
				if (OnKeyDown(code) && g_hookInstalled) {
					g_swallowing[code] = true;  // also drop the held/release events of this click
					if (Settings::debugLog) {
						logger::info("Click on a door marker kept away from the map");
					}
					return true;
				}
				return false;
			}

			const bool released = !button->IsPressed();
			if (released) {
				g_down[code] = false;
			}
			if (g_swallowing[code]) {
				if (released) {
					g_swallowing[code] = false;
				}
				return true;
			}
			return false;
		}

		// Sits in front of the game's input dispatch (same spot Wheeler and IED use), so a click
		// can be removed from the event list before any menu sees it.
		struct InputDispatchHook
		{
			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events)
			{
				if (a_events) {
					RE::InputEvent* previous = nullptr;
					for (auto* event = *a_events; event;) {
						auto* next = event->next;
						if (ProcessInputEvent(event)) {
							if (previous) {
								previous->next = next;
							} else {
								*a_events = next;
							}
						} else {
							previous = event;
						}
						event = next;
					}
				}
				func(a_source, a_events);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Fallback when the hook could not be installed: sees input but cannot swallow it
		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				for (auto* event = a_event ? *a_event : nullptr; event; event = event->next) {
					ProcessInputEvent(event);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// ------------------------------------------------------------------
		// Tracking: doors reached, levels visited, dungeon entrance
		// ------------------------------------------------------------------

		void SetEntrance(const Entrance& a_entrance, const char* a_how)
		{
			g_entrance = a_entrance;
			logger::info("Dungeon entrance set ({}): cell {:08X}, door {:08X}, at ({:.0f}, {:.0f}, {:.0f})",
				a_how, a_entrance.cell, a_entrance.door, a_entrance.pos.x, a_entrance.pos.y, a_entrance.pos.z);
		}

		// Just arrived in a_cell from a_oldCell (nullptr = came from outside):
		// the entrance is the door here that leads back where we came from.
		void RecordEntranceFromArrival(RE::TESObjectCELL* a_cell, RE::TESObjectCELL* a_oldCell, bool a_cameFromOutside)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const auto playerPos = player->GetPosition();
			const auto doors = CollectLoadDoors(a_cell);

			RE::TESObjectREFR* best = nullptr;
			float              bestDist2 = std::numeric_limits<float>::max();
			for (auto* door : doors) {
				auto  partner = GetLinkedDoor(door);
				auto* otherSide = partner ? partner->GetParentCell() : nullptr;

				const bool leadsBack = a_cameFromOutside ? (!otherSide || !otherSide->IsInteriorCell()) : (otherSide == a_oldCell);
				if (!leadsBack) {
					continue;
				}
				const float dist2 = door->GetPosition().GetSquaredDistance(playerPos);
				if (dist2 < bestDist2) {
					bestDist2 = dist2;
					best = door;
				}
			}

			Entrance entrance;
			entrance.valid = true;
			entrance.cell = a_cell->GetFormID();
			if (best) {
				entrance.door = best->GetFormID();
				if (!GetLandingAtDoor(best, entrance.pos, entrance.rot)) {
					entrance.pos = playerPos;
					entrance.rot = player->GetAngle();
				}
			} else if (!doors.empty()) {
				// Arrived some other way: use where the player stands now, any door here as the anchor
				entrance.door = doors.front()->GetFormID();
				entrance.pos = playerPos;
				entrance.rot = player->GetAngle();
			} else {
				logger::info("No load doors in cell {:08X} yet; entrance not recorded", a_cell->GetFormID());
				return;
			}
			SetEntrance(entrance, a_cameFromOutside ? "entered from outside" : "entered dungeon");
		}

		// Loaded a save inside a dungeon with no entrance on record: walk the door links
		// (load doors are persistent) to find the level whose door leads outside.
		void FindEntranceBySearch(RE::TESObjectCELL* a_start)
		{
			std::deque<std::pair<RE::TESObjectCELL*, int>> queue;
			std::unordered_set<RE::FormID>                   seen;
			queue.emplace_back(a_start, 0);
			seen.insert(a_start->GetFormID());

			while (!queue.empty()) {
				auto [cell, depth] = queue.front();
				queue.pop_front();

				for (auto* door : CollectLoadDoors(cell)) {
					auto  partner = GetLinkedDoor(door);
					auto* otherSide = partner ? partner->GetParentCell() : nullptr;
					if (!otherSide) {
						continue;
					}
					if (!otherSide->IsInteriorCell()) {
						Entrance entrance;
						entrance.valid = true;
						entrance.cell = cell->GetFormID();
						entrance.door = door->GetFormID();
						if (GetLandingAtDoor(door, entrance.pos, entrance.rot)) {
							SetEntrance(entrance, "found by searching door links");
							return;
						}
						continue;
					}
					if (depth < 12 && !seen.contains(otherSide->GetFormID()) && CellQualifies(otherSide)) {
						seen.insert(otherSide->GetFormID());
						queue.emplace_back(otherSide, depth + 1);
					}
				}
			}
			logger::info("No dungeon entrance found by searching from cell {:08X} ({} cells checked)",
				a_start->GetFormID(), seen.size());
		}

		void OnCellChanged(RE::TESObjectCELL* a_cell)
		{
			const bool firstAfterLoad = !g_haveLastCell;
			logger::info("Player cell: {:08X} {} ({}){}", a_cell->GetFormID(),
				a_cell->GetFullName() ? a_cell->GetFullName() : "",
				a_cell->IsInteriorCell() ? (CellQualifies(a_cell) ? "dungeon" : "interior") : "exterior",
				firstAfterLoad ? " [after load]" : "");

			if (!a_cell->IsInteriorCell()) {
				if (g_entrance.valid) {
					logger::info("Left the dungeon; entrance cleared");
				}
				g_entrance = {};
				return;
			}

			const bool qualifies = CellQualifies(a_cell);

			if (firstAfterLoad) {
				if (!g_entrance.valid && qualifies) {
					FindEntranceBySearch(a_cell);
				}
				return;
			}

			const bool cameFromOutside = !g_lastInterior;
			bool       needEntrance = !g_entrance.valid;
			if (!needEntrance && qualifies) {
				// The chain started in a house or keep: the dungeon proper starts here
				auto* entranceCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_entrance.cell);
				needEntrance = !entranceCell || !CellQualifies(entranceCell);
			}

			if (cameFromOutside || (needEntrance && qualifies)) {
				auto* oldCell = cameFromOutside ? nullptr : RE::TESForm::LookupByID<RE::TESObjectCELL>(g_lastCell);
				RecordEntranceFromArrival(a_cell, oldCell, cameFromOutside);
			}
		}

		void RescanDoors(RE::TESObjectCELL* a_cell)
		{
			g_cellDoors.clear();
			for (auto* door : CollectLoadDoors(a_cell)) {
				g_cellDoors.push_back(door->GetFormID());
			}
		}

		void Tick()
		{
			if (IsLoading()) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			if (!cell || !cell->IsAttached()) {
				return;
			}

			if (!g_haveLastCell || cell->GetFormID() != g_lastCell) {
				OnCellChanged(cell);
				g_haveLastCell = true;
				g_lastCell = cell->GetFormID();
				g_lastInterior = cell->IsInteriorCell();
				g_doorScanCountdown = 0;
			}

			if (!CellQualifies(cell)) {
				return;
			}

			g_visitedCells.insert(cell->GetFormID());

			// Rescan the door list once a second (doors can be enabled/disabled by scripts)
			if (g_doorScanCountdown == 0) {
				RescanDoors(cell);
				g_doorScanCountdown = 4;
			} else {
				--g_doorScanCountdown;
			}

			const auto  playerPos = player->GetPosition();
			const float radius2 = Settings::doorReachRadius * Settings::doorReachRadius;
			for (const auto id : g_cellDoors) {
				if (g_reachedDoors.contains(id)) {
					continue;
				}
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (door && door->GetPosition().GetSquaredDistance(playerPos) <= radius2) {
					g_reachedDoors.insert(id);
					if (Settings::debugLog) {
						logger::info("Reached door {}", Describe(door));
					}
				}
			}
		}

		void StartTicker()
		{
			std::thread([]() {
				for (;;) {
					std::this_thread::sleep_for(kTickInterval);
					if (g_tickQueued.exchange(true)) {
						continue;
					}
					SKSE::GetTaskInterface()->AddTask([]() {
						Tick();
						g_tickQueued = false;
					});
				}
			}).detach();
		}

		// ------------------------------------------------------------------
		// Co-save
		// ------------------------------------------------------------------

		constexpr std::uint32_t kRecordDoors = 'DOOR';
		constexpr std::uint32_t kRecordCells = 'CELL';
		constexpr std::uint32_t kRecordEntrance = 'ENTR';
		constexpr std::uint32_t kRecordVersion = 1;

		void WriteSet(SKSE::SerializationInterface* a_intfc, std::uint32_t a_type, const std::unordered_set<RE::FormID>& a_set)
		{
			if (!a_intfc->OpenRecord(a_type, kRecordVersion)) {
				logger::error("Could not open co-save record");
				return;
			}
			const auto count = static_cast<std::uint32_t>(a_set.size());
			a_intfc->WriteRecordData(count);
			for (const auto id : a_set) {
				a_intfc->WriteRecordData(id);
			}
		}

		void ReadSet(SKSE::SerializationInterface* a_intfc, std::unordered_set<RE::FormID>& a_set)
		{
			a_set.clear();
			std::uint32_t count = 0;
			a_intfc->ReadRecordData(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID oldID = 0;
				RE::FormID newID = 0;
				if (a_intfc->ReadRecordData(oldID) != sizeof(oldID)) {
					break;
				}
				if (a_intfc->ResolveFormID(oldID, newID)) {
					a_set.insert(newID);
				}
			}
		}
	}

	void InstallHooks()
	{
		// BSInputDeviceManager::PollInputDevices -> call that hands the event list to the menus
		REL::Relocation<std::uintptr_t> function{ RELOCATION_ID(67315, 68617) };
		const auto                      callSite = function.address() + 0x7B;

		const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
		if (bytes[0] != 0xE8) {
			logger::error("Input dispatch call not found (byte {:02X} at {:X}); falling back to an input sink",
				bytes[0], callSite - REL::Module::get().base());
			return;
		}

		SKSE::AllocTrampoline(14);
		InputDispatchHook::func = SKSE::GetTrampoline().write_call<5>(callSite, InputDispatchHook::thunk);
		g_hookInstalled = true;
		logger::info("Input dispatch hook installed at SkyrimSE.exe+{:X}", callSite - REL::Module::get().base());
	}

	void Install()
	{
		g_dungeonKeywords.clear();
		for (const auto& name : Settings::dungeonKeywords) {
			if (auto* keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(name)) {
				g_dungeonKeywords.push_back(keyword);
			} else {
				logger::warn("Dungeon keyword '{}' not found", name);
			}
		}
		logger::info("{} dungeon keyword(s) active{}", g_dungeonKeywords.size(),
			(Settings::dungeonsOnly && g_dungeonKeywords.empty()) ? " - none found, so every interior counts" : "");

		if (g_hookInstalled) {
			logger::info("Input handled by the dispatch hook");
		} else if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(InputSink::GetSingleton());
			logger::warn("Input sink registered (fallback): map clicks on doors will also reach the map");
		} else {
			logger::error("BSInputDeviceManager not available: map warp and hotkey will not work");
		}

		StartTicker();
		logger::info("Tracker started ({} ms)", kTickInterval.count());
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		WriteSet(a_intfc, kRecordDoors, g_reachedDoors);
		WriteSet(a_intfc, kRecordCells, g_visitedCells);

		if (a_intfc->OpenRecord(kRecordEntrance, kRecordVersion)) {
			const std::uint8_t valid = g_entrance.valid ? 1 : 0;
			a_intfc->WriteRecordData(valid);
			a_intfc->WriteRecordData(g_entrance.cell);
			a_intfc->WriteRecordData(g_entrance.door);
			a_intfc->WriteRecordData(g_entrance.pos);
			a_intfc->WriteRecordData(g_entrance.rot);
		}
		logger::info("Saved: {} reached door(s), {} visited level(s), entrance {}",
			g_reachedDoors.size(), g_visitedCells.size(), g_entrance.valid ? "recorded" : "none");
	}

	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (version != kRecordVersion) {
				logger::warn("Skipping co-save record with version {}", version);
				continue;
			}
			switch (type) {
			case kRecordDoors:
				ReadSet(a_intfc, g_reachedDoors);
				break;
			case kRecordCells:
				ReadSet(a_intfc, g_visitedCells);
				break;
			case kRecordEntrance:
				{
					std::uint8_t valid = 0;
					Entrance     entrance;
					a_intfc->ReadRecordData(valid);
					a_intfc->ReadRecordData(entrance.cell);
					a_intfc->ReadRecordData(entrance.door);
					a_intfc->ReadRecordData(entrance.pos);
					a_intfc->ReadRecordData(entrance.rot);
					entrance.valid = valid != 0 &&
					                 a_intfc->ResolveFormID(entrance.cell, entrance.cell) &&
					                 a_intfc->ResolveFormID(entrance.door, entrance.door);
					g_entrance = entrance.valid ? entrance : Entrance{};
				}
				break;
			default:
				break;
			}
		}
		logger::info("Loaded: {} reached door(s), {} visited level(s), entrance {}",
			g_reachedDoors.size(), g_visitedCells.size(), g_entrance.valid ? "recorded" : "none");
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		g_reachedDoors.clear();
		g_visitedCells.clear();
		g_entrance = {};
		g_cellDoors.clear();
		g_haveLastCell = false;
		g_lastCell = 0;
		g_lastInterior = false;
	}

	void OnGameLoaded()
	{
		g_haveLastCell = false;
		g_confirmUntil = {};
		g_warpInProgress = false;
	}
}
