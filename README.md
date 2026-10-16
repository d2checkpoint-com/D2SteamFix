# D2SteamFix

D2SteamFix is a launch wrapper for Destiny 2 intended to help alleviate frame pacing and stuttering issues for Steam users. Before starting Destiny 2, it temporarily denies execute access to Steam's overlay renderer DLLs, then restores the original permissions after exiting the game.

Please note that blocking the Steam Overlay DLLs can also affect overlay-dependent features such as Steam Input, notifications, Game Recording, and Remote Play. This program temporarily changes only the ACL metadata of the overlay DLLs; it does not alter their contents. It does not inject code, inspect process memory, modify game files, change Steam configuration, or bypass BattlEye.

## Installation

Download `steamfix.exe` from the [latest GitHub release](https://github.com/d2checkpoint-com/D2SteamFix/releases/latest) and place it directly in your game's directory.

Then, open Destiny 2's launch options in Steam:

1. In Steam right-click **Destiny 2** in your library and select **Properties**.
2. Find the **Launch Options** field under the **General** tab and paste the command below.

```text
"C:\path\to\Destiny 2\steamfix.exe" %command%
```

Replace `C:\path\to\Destiny 2` with the actual location of your Destiny 2 installation. The example path below is for a default Steam installation; if your game is on another drive, use that drive letter and the complete path instead:

```text
"C:\Program Files (x86)\Steam\steamapps\common\Destiny 2\steamfix.exe" %command%
```

![Steam Launch Options](https://static.d2checkpoint.com/steamfix_launch_options.png)

## Building from Source

### Prerequisites

Building D2SteamFix requires Windows and the Microsoft Visual C++ toolchain.

Install either Visual Studio or Visual Studio Build Tools with the Desktop development with C++ workload selected. Make sure to include the MSVC compiler and a Windows SDK. Run the following commands from a Visual Studio Developer command prompt:

```batch
make.cmd configure
```

```batch
make.cmd build
```

GitHub Actions builds both Debug and Release configurations on Windows x64 for pushes and pull requests. The executables are available as workflow artifacts.

## Publishing a Release

Update the version in `CMakeLists.txt` through a pull request, then tag the commit to release with the matching version and push the tag:

```bash
git tag -a v1.0.0 -m "D2SteamFix 1.0"
git push origin v1.0.0
```

Use the new version for each subsequent release. Pushing a `vX.Y.Z` tag runs the same build checks, verifies that the tag matches the CMake project version, and publishes a GitHub release with `steamfix.exe` attached after both builds succeed. The tagged commit must contain the release workflow.

## Windows Defender

The published binary may be flagged as a false positive by Windows Defender. This is a known issue and Microsoft has confirmed that it was a false positive and has removed it.

If your Windows Defender still reports the old detection, [please follow these instructions to clear cached detections and update malware definitions.](docs/WINDOWS_DEFENDER.md)
