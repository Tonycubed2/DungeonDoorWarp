#pragma once

// Settings from Data\SKSE\Plugins\DungeonDoorWarp.ini
//
// Key codes use the SKSE / SkyUI convention:
//   keyboard = DirectX scan code (1-255), e.g. 199 = Home, 42 = Left Shift
//   mouse    = 256 + button: 256 = left, 257 = right, 258 = middle
//   0        = off / none
namespace Settings
{
	// [General]
	inline bool                     dungeonsOnly = true;
	inline std::vector<std::string> dungeonKeywords;
	inline int                      blockInCombat = 1;  // 0 = never, 1 = a nearby enemy is fighting you, 2 = any loaded enemy fighting you
	inline float                    combatRadius = 3000.0f;
	inline bool                     notifications = true;
	inline bool                     debugLog = false;

	// [MapWarp]
	inline bool          mapWarpEnabled = true;
	inline std::uint32_t mapWarpButton = 256;   // left mouse
	inline std::uint32_t mapWarpModifier = 0;   // none
	inline int           visitRule = 3;         // 1 = door reached, 2 = level behind door visited, 3 = either
	inline float         doorReachRadius = 600.0f;
	inline int           localMapCheck = 1;     // 0 = none, 1 = engine "showing local map" flag, 2 = movie _visible

	// [EntranceHotkey]
	inline bool          entranceEnabled = true;
	inline std::uint32_t entranceHotkey = 199;  // Home
	inline std::uint32_t entranceModifier = 0;  // none
	inline bool          entranceConfirm = true;
	inline float         entranceConfirmSeconds = 3.0f;

	// [Followers]
	inline bool  moveFollowers = true;
	inline bool  moveSummons = true;
	inline bool  skipWaitingFollowers = true;
	inline float followerGatherRadius = 0.0f;  // 0 = everyone on the player's level
	inline float followerOffset = 96.0f;

	void Load();
}
