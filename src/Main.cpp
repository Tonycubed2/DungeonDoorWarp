// Dungeon Door Warp 1.0.4
// Built against CommonLibSSE-NG 6.7.0 for Skyrim AE 1.6.1170 and 1.7.99.
//
// Log: Documents\My Games\Skyrim Special Edition\SKSE\DungeonDoorWarp.log
// INI: Data\SKSE\Plugins\DungeonDoorWarp.ini

#include "Settings.h"
#include "Warp.h"

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 4, 0 },
	.Name = "DungeonDoorWarp"sv,
	.Author = "tonycubed2"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Warp::Install();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Warp::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	logger::info("DungeonDoorWarp 1.0.4 loading; game runtime {}", REL::Module::get().version().string());

	Settings::Load();
	Warp::InstallHooks();

	auto* serialization = SKSE::GetSerializationInterface();
	if (!serialization) {
		logger::critical("Serialization interface not available");
		return false;
	}
	serialization->SetUniqueID('DDWP');
	serialization->SetSaveCallback(Warp::OnSave);
	serialization->SetLoadCallback(Warp::OnLoad);
	serialization->SetRevertCallback(Warp::OnRevert);

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logger::critical("Could not register SKSE messaging listener");
		return false;
	}

	logger::info("Loaded");
	return true;
}
