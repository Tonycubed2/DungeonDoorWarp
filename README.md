# Dungeon Door Warp

SKSE plugin for Skyrim that removes boring backtracking inside dungeons.

- **Map warp:** in a dungeon, open the local map, hover a door of the level you are on and click it. You are placed right in front of that door, on your side of it. Only doors you have reached, or doors leading to a level you have visited.
- **Entrance hotkey:** press the hotkey (default Numpad 0, twice to confirm) to return to just inside the dungeon's first door, from any level.
- **Followers come along:** all followers on your level (teammates and CurrentFollowerFaction members, so multiple-follower setups work) and your summons.

Plain teleport: no fast travel, no time passes. No ESP. Settings in `DungeonDoorWarp.ini`.

## Requirements

- SKSE64
- Address Library for SKSE Plugins

Tested on Skyrim AE 1.6.1170. Built for SE 1.5.97 and AE 1.6.x / 1.7.99 (untested there). Not for VR.

## Building

Visual Studio 2022, CMake, vcpkg (`VCPKG_ROOT` set), and a CommonLibSSE-NG source tree at the path in the `CommonLibSSEPath_NG` environment variable (default `C:/dllmaker/CommonLibSSE-NG`).

From a Developer Command Prompt for VS 2022:

```
cmake --preset vs2022-windows
```
```
cmake --build build --config Release
```

Output: `build\Release\DungeonDoorWarp.dll` and `.pdb`.

## License

Dungeon Door Warp is licensed under the GNU General Public License v3.0 or later (see [LICENSE](LICENSE)), with the same additional permissions as CommonLibSSE NG: the Modding Exception and the GPL-3.0 Linking Exception (with Corresponding Source), see [EXCEPTIONS.md](EXCEPTIONS.md).

It is built with and statically links [CommonLibSSE NG](https://github.com/alandtse/CommonLibSSE-NG), licensed GPL-3.0-or-later with those exceptions. CommonLibSSE NG was originally based on code under the MIT license, copyright (c) 2018 Ryan-rsm-McKenzie (see [licenses/LICENSE-MIT-CommonLibSSE](licenses/LICENSE-MIT-CommonLibSSE)).

The input dispatch hook location (ID 67315 / 68617, +0x7B) is the one used by [IED](https://github.com/SlavicPotato/ied-dev) and [Wheeler](https://github.com/D7ry/wheeler).

Copyright (C) 2026 tonycubed2
