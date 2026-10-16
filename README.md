# D2SteamFix

D2SteamFix is a launch wrapper for the Steam version of Destiny 2 on 64-bit Windows. It aims to reduce frame pacing and stuttering issues by temporarily blocking Steam's overlay renderer while you play, then restoring its original permissions after the game exits.

Blocking the Steam overlay can also affect Steam Input, notifications, Game Recording, and Remote Play. During normal game launches, D2SteamFix changes only the overlay files' permissions, leaving their contents intact. The separate repair mode described below asks Steam to replace its overlay files. D2SteamFix does not inject code, inspect process memory, modify game files, change Steam configuration, or bypass BattlEye.

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

## Playing Destiny 2

Start Destiny 2 from Steam as usual. D2SteamFix blocks the overlay before starting the game and restores its permissions when the game exits.

D2SteamFix supports normal and administrator launches. If Windows blocks a required permission change or launcher operation, it requests permission through the Windows administrator prompt. Cancelling the prompt stops the launch and restores any permissions already changed.

## Interrupted Sessions

D2SteamFix stores `steamfix.dat`, its backup of the original Steam overlay permissions, and `steamfix.log` in the Destiny 2 directory. Steam launches use the directory of the forwarded `destiny2launcher.exe` command, even if the working directory differs.

If a session is interrupted, the next Steam launch reuses the existing backup and keeps the overlay blocked until Destiny 2 exits. It then restores the original permissions. Keep `steamfix.dat` until recovery succeeds; deleting it can leave the Steam overlay disabled in other games.

If a saved backup no longer matches the renderer files or permissions, D2SteamFix preserves it and reports the mismatch. Open `steamfix.exe` directly to use Steam repair instead.

## Repairing Steam Overlay Permissions

Double-click `steamfix.exe` in the Destiny 2 directory to repair Steam's overlay files, including when a recovery backup is missing or unusable. D2SteamFix also offers this repair during a Steam launch if the overlay files already have blocked execute access and there is no backup.

The dialog explains that Steam and Destiny 2 will close so Steam can repair its files. After you continue and approve any required Windows permission prompt, D2SteamFix closes those applications, removes the affected overlay files, and starts Steam to download fresh copies. If Steam cannot be located, select its `steam.exe` in the file picker.

After Steam exits and the repaired files are verified, D2SteamFix removes `steamfix.dat` and any `.tmp` or `.old` recovery files from the game directory, keeps all logs, and exits. Start Steam again when you are ready to play. Cancelling before repair leaves the files and applications unchanged. Recovery cleanup runs only after the repaired files are verified.

## Removing D2SteamFix

1. Exit Destiny 2 normally so D2SteamFix can restore the overlay permissions. If `steamfix.dat` remains after the session ends, complete recovery or use the repair steps above before continuing.
2. Remove the D2SteamFix command from Destiny 2's **Launch Options** in Steam.
3. Delete `steamfix.exe` from the game directory if you no longer need it.

## Troubleshooting

If a launch fails, check `steamfix.log` in the Destiny 2 directory for details. The [exit code reference](docs/EXIT_CODES.md) explains the reported error codes. When [reporting a problem](https://github.com/d2checkpoint-com/D2SteamFix/issues), include the error message and relevant log entries; review them for personal paths before posting.

### Windows Defender

The published binary may be flagged as a false positive by Windows Defender. This is a known issue and Microsoft has confirmed that it was a false positive and has removed it.

If your Windows Defender still reports the old detection, [please follow these instructions to clear cached detections and update malware definitions.](docs/WINDOWS_DEFENDER.md)

## Development

See [Building from Source](docs/BUILDING.md) for build requirements and commands, and [AGENTS.md](AGENTS.md) for repository guidance.
