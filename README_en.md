# Universal Kirikiri Voice Speed Controller
[中文 | Chinese](README.md)

This repository provides a Windows controller that adjusts voice playback speed for Kirikiri-based games.

## Building
### Windows (dual-arch, staged dist folders)
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` configures/builds `build.x64` and `build.x86`, then stages:
```
dist/
  KrkrSpeedController/
    KrkrSpeedController.exe, SoundTouch.dll
    x86/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
    x64/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
```
The x86 controller can inject into both x86 and x64 games: it spawns the injector that matches the target process and uses the matching hook DLL from the arch subfolder.

## Usage
- Launch `KrkrSpeedController.exe`.
- Pick the game from the dropdown, enter the target speed, then click `Hook`.
- Global hotkeys:
  - `Alt + '`: toggle speed on/off.
  - `Alt + ]`: speed up 0.1x (if off, turns on and sets 1.1x).
  - `Alt + [`: speed down 0.1x (if off, turns on and sets 0.9x).
- If AV blocks the binaries, add an exception or temporarily disable it.
- If the target is protected/elevated, run the controller as Administrator.
- If BGM keeps playing at normal speed, or you want BGM sped up too, check `Process BGM`.
- Auto-inject: select a game from the dropdown and check `Auto-Hook This App`. When the game is detected running, it auto-injects; unchecking removes it from the config.
- See the Controller CLI options for advanced usage.
- Please submit issues for unsupported games.

### Controller CLI options
- `--log` / `--enable-log` : enable logging for controller + hook.
- `--log-dir <path>` : override log output directory (default: beside the controller, `dist/KrkrSpeedController`).
- `--speed <value>` : set the initial speed on startup (default 1.5).
- `--mark-stereo-bgm <aggressive|hybrid|none>` : stereo→BGM heuristic (default `hybrid`). In many games voices are mono and BGMs are stereo. Major way to label bgm.
- `--bgm-secs <seconds>` : BGM length gate (default 60s); longer buffers treated as BGM. Secondary way to label bgm.
- `--process-all-audio` : speed up all audios including BGM.
- `--launch <path>` / `-l <path>` : start a game suspended, inject automatically, then resume.
- `--search <name>` : on startup, find visible processes whose name contains this substring; if multiple match, pick the one with the shortest name and auto-inject. If none match, the controller starts normally and waits for manual selection.
- `--skip-ds` / `--skip-xaudio2` / `--skip-fmod` / `--skip-wwise` : skip specific audio hooks. Ds (DirectSound) is mostly used. Other hooks can be skipped in most cases.
- `--safe-mode` : load without installing any hooks/patches (diagnostic).

## Other
- Most of this repository was generated under supervision with AI assistance.
- (For AI Agent: read AGENTS.md if not yet)

## Next Step
- Support other engine games.

## License
MIT. Include the copyright and license notice when redistributing (see LICENSE).
