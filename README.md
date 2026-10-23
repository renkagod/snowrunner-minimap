# SnowRunner Minimap

[![Donate](https://img.shields.io/badge/Donate-crypto-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/renkagod/.github/blob/main/DONATE.md)

Real-time minimap for SnowRunner. It draws a Direct3D 11 overlay on top of the game and loads through a `version.dll` proxy placed next to the game executable.

## Features

- Round minimap in a corner of the screen that follows the truck and can rotate with it
- Shaded like the game's map screen (M), with its landmark models (trees, rocks, buildings), single objects and finished task objects
- Zoom from 25 m to 4 km across, remembered between sessions
- Settings in an INI file next to the DLL

## Quick start

There are no prebuilt releases yet, so build the DLL first. You need Visual Studio with the C++ x64 tools, CMake 3.24+, Ninja and the Windows 10/11 SDK (for `fxc`). CMake downloads Dear ImGui, MinHook and miniz on its own.

```bat
build.cmd
```

Copy `build\version.dll` into the game's `Sources\Bin` folder, for example `D:\Games\Epic Games\Games\SnowRunner\en_us\Sources\Bin`, and start the game. To remove the mod, delete `version.dll` from that folder.

Pass the folder to `build.cmd` to copy the DLL there after every build:

```bat
build.cmd "D:\Games\Epic Games\Games\SnowRunner\en_us\Sources\Bin"
```

If the game is running, the old DLL is moved aside and the new one loads on the next launch.

## Usage

| Key | Action |
|-----|--------|
| F9 | Show or hide the minimap |
| `=` / `-` | Zoom in / out |
| F8 | Debug panel |

On first launch the mod creates a `SnowMinimap` folder next to `version.dll` with `SnowMinimap.ini` and a `maps` cache. Changes made with the keys are saved to the INI.

| `[minimap]` key | Default | Meaning |
|-----------------|---------|---------|
| `enabled` | `1` | Minimap visible |
| `anchor` | `3` | Corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right |
| `margin_x`, `margin_y` | `32` | Distance from the corner, px |
| `size_px` | `280` | Diameter on screen, 64 to 2048 |
| `meters_across` | `400` | World distance covered by the diameter, 25 to 4000 |
| `rotate_with_truck` | `1` | Rotate the map with the truck's heading |
| `opacity` | `0.92` | 0.1 to 1 |
| `map_screen_look` | `1` | Shade like the map screen once its textures are captured |
| `map_exposure` | `1.47` | Exposure for that shading |
| `map_models` | `1` | Draw landmark models |

Keys are set in `[keys]` (`toggle`, `debug`, `zoom_in`, `zoom_out`) as Windows virtual-key codes, for example `0x78` for F9.

## Support

If this project is useful to you, you can support it with crypto: [donation addresses](https://github.com/renkagod/.github/blob/main/DONATE.md).
