#include "Settings.h"

namespace
{
	constexpr auto kIniPath = "Data/SKSE/Plugins/DungeonDoorWarp.ini";

	std::string Trim(std::string a_s)
	{
		auto notSpace = [](unsigned char c) { return !std::isspace(c); };
		a_s.erase(a_s.begin(), std::find_if(a_s.begin(), a_s.end(), notSpace));
		a_s.erase(std::find_if(a_s.rbegin(), a_s.rend(), notSpace).base(), a_s.end());
		return a_s;
	}

	std::string Lower(std::string a_s)
	{
		for (auto& c : a_s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return a_s;
	}

	// "section.key" (lower case) -> value
	using IniMap = std::unordered_map<std::string, std::string>;

	IniMap ReadIni()
	{
		IniMap        map;
		std::ifstream file(kIniPath);
		if (!file) {
			logger::warn("{} not found, using defaults", kIniPath);
			return map;
		}

		std::string section;
		std::string line;
		while (std::getline(file, line)) {
			// strip comments
			for (const char marker : { ';', '#' }) {
				if (auto pos = line.find(marker); pos != std::string::npos) {
					line.erase(pos);
				}
			}
			line = Trim(line);
			if (line.empty()) {
				continue;
			}
			if (line.front() == '[' && line.back() == ']') {
				section = Lower(Trim(line.substr(1, line.size() - 2)));
				continue;
			}
			auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			map[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
		}
		return map;
	}

	const std::string* Find(const IniMap& a_map, const char* a_key)
	{
		auto it = a_map.find(a_key);
		return it != a_map.end() ? &it->second : nullptr;
	}

	void Get(const IniMap& a_map, const char* a_key, bool& a_out)
	{
		if (auto* v = Find(a_map, a_key)) {
			auto s = Lower(*v);
			a_out = !(s == "0" || s == "false" || s == "off" || s == "no");
		}
	}

	void Get(const IniMap& a_map, const char* a_key, int& a_out)
	{
		if (auto* v = Find(a_map, a_key)) {
			try {
				a_out = std::stoi(*v, nullptr, 0);
			} catch (...) {
				logger::warn("Bad number for {}: '{}'", a_key, *v);
			}
		}
	}

	void Get(const IniMap& a_map, const char* a_key, std::uint32_t& a_out)
	{
		int value = static_cast<int>(a_out);
		Get(a_map, a_key, value);
		a_out = value < 0 ? 0u : static_cast<std::uint32_t>(value);
	}

	void Get(const IniMap& a_map, const char* a_key, float& a_out)
	{
		if (auto* v = Find(a_map, a_key)) {
			try {
				a_out = std::stof(*v);
			} catch (...) {
				logger::warn("Bad number for {}: '{}'", a_key, *v);
			}
		}
	}
}

namespace Settings
{
	void Load()
	{
		dungeonKeywords = {
			"LocTypeDungeon",
			"LocTypeClearable",
			"LocTypeDraugrCrypt",
			"LocTypeDwarvenAutomatons",
			"LocTypeFalmerHive",
			"LocTypeVampireLair",
			"LocTypeWarlockLair",
			"LocTypeAnimalDen",
			"LocTypeBanditCamp",
			"LocTypeForswornCamp",
			"LocTypeHagravenNest",
			"LocTypeDragonPriestLair",
			"LocTypeMine",
		};

		const auto ini = ReadIni();

		Get(ini, "general.dungeonsonly", dungeonsOnly);
		Get(ini, "general.blockincombat", blockInCombat);
		Get(ini, "general.combatradius", combatRadius);
		Get(ini, "general.notifications", notifications);
		Get(ini, "general.debuglog", debugLog);

		if (auto* list = Find(ini, "general.dungeonkeywords"); list && !list->empty()) {
			dungeonKeywords.clear();
			std::size_t start = 0;
			while (start <= list->size()) {
				auto end = list->find(',', start);
				if (end == std::string::npos) {
					end = list->size();
				}
				auto word = Trim(list->substr(start, end - start));
				if (!word.empty()) {
					dungeonKeywords.push_back(word);
				}
				start = end + 1;
			}
		}

		Get(ini, "mapwarp.enabled", mapWarpEnabled);
		Get(ini, "mapwarp.button", mapWarpButton);
		Get(ini, "mapwarp.modifier", mapWarpModifier);
		Get(ini, "mapwarp.visitrule", visitRule);
		Get(ini, "mapwarp.doorreachradius", doorReachRadius);
		Get(ini, "mapwarp.localmapcheck", localMapCheck);

		Get(ini, "entrancehotkey.enabled", entranceEnabled);
		Get(ini, "entrancehotkey.hotkey", entranceHotkey);
		Get(ini, "entrancehotkey.modifier", entranceModifier);
		Get(ini, "entrancehotkey.confirm", entranceConfirm);
		Get(ini, "entrancehotkey.confirmseconds", entranceConfirmSeconds);

		Get(ini, "followers.movefollowers", moveFollowers);
		Get(ini, "followers.movesummons", moveSummons);
		Get(ini, "followers.skipwaitingfollowers", skipWaitingFollowers);
		Get(ini, "followers.gatherradius", followerGatherRadius);
		Get(ini, "followers.offset", followerOffset);

		visitRule = std::clamp(visitRule, 1, 3);
		localMapCheck = std::clamp(localMapCheck, 0, 2);
		blockInCombat = std::clamp(blockInCombat, 0, 2);

		logger::info("Settings: DungeonsOnly={} ({} keywords) BlockInCombat={} CombatRadius={} Notifications={} DebugLog={}",
			dungeonsOnly, dungeonKeywords.size(), blockInCombat, combatRadius, notifications, debugLog);
		logger::info("Settings: MapWarp Enabled={} Button={} Modifier={} VisitRule={} DoorReachRadius={} LocalMapCheck={}",
			mapWarpEnabled, mapWarpButton, mapWarpModifier, visitRule, doorReachRadius, localMapCheck);
		logger::info("Settings: Entrance Enabled={} Hotkey={} Modifier={} Confirm={} ({}s)",
			entranceEnabled, entranceHotkey, entranceModifier, entranceConfirm, entranceConfirmSeconds);
		logger::info("Settings: Followers Move={} Summons={} SkipWaiting={} GatherRadius={} Offset={}",
			moveFollowers, moveSummons, skipWaitingFollowers, followerGatherRadius, followerOffset);
	}
}
