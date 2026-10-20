#pragma once

namespace Warp
{
	// Call from SKSEPluginLoad: hooks the input dispatch so door clicks on the map can be swallowed.
	void InstallHooks();

	// Call at kDataLoaded: resolves keywords, registers the input sink, starts the 250 ms tracker.
	void Install();

	// SKSE co-save
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void OnLoad(SKSE::SerializationInterface* a_intfc);
	void OnRevert(SKSE::SerializationInterface* a_intfc);

	// After a save is loaded or a new game starts
	void OnGameLoaded();
}
