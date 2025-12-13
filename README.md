# Universal Kirikiri Voice Speed Controller
[中文 | Chinese](README_zh-CN.md)

This repository contains an early scaffold for a Kirikiri voice speed controller. The Windows-specific hook targets are not built on non-Windows hosts.

## Building
Use CMake to generate a Visual Studio solution. SoundTouch binaries/headers are bundled under `externals/soundtouch` and are always enabled; vcpkg is only a fallback if the bundle is missing.

### Windows (dual-arch, staged dist folders)
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` configures/builds `build.x64` and `build.x86`, then stages:
```
dist/
  x64/ KrkrSpeedController.exe, krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
  x86/ KrkrSpeedController.exe, krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
  KrkrSpeedController_x86.lnk  (Windows shortcut to launch the x86 controller)
```
Either controller can inject into both x86 and x64 games: it spawns the injector that matches the target process and uses the matching hook DLL from the sibling dist folder.

### Controller CLI options (no env vars)
Flags are sent to the injected hook via shared settings:
- `--log` / `--enable-log`
- `--log-dir <path>`
- `--skip-ds`, `--skip-xaudio2`, `--safe-mode`, `--disable-veh`
- `--bgm-secs <seconds>` (length/BGM gate)
- `--force-all` (process all audio), `--disable-bgm` (never treat as BGM)

## Usage
- Most Kirikiri games are 32-bit; use the x86 controller for those.
- If AV blocks the binaries, add an exception or temporarily disable it.
- If the target is protected/elevated, run the controller as Administrator.
- Default hooks: DirectSound + XAudio2 + FMOD enabled; logging is off unless `--log` is passed.
- Supported engines: Kirikiri (DirectSound/XAudio2), FMOD (Unity/Native), Wwise (Experimental).
- BGM detection: stereo/looping buffers are treated as BGM unless you pass `--force-all`.

## Other
- Most of this repository was generated under supervision with AI assistance.
- (For AI Agent: read AGENTS.md if not yet)

## License
MIT. Include the copyright and license notice when redistributing (see LICENSE).
