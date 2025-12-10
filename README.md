# Universal Kirikiri Voice Speed Controller
[中文 | Chinese](README_zh-CN.md)

This repository contains an early scaffold for a Kirikiri voice speed controller. The Windows-specific hook targets are not built on non-Windows hosts.

## Building
Use CMake to generate a Visual Studio solution. SoundTouch integration is disabled by default to keep the scaffold self-contained; enable with `-DUSE_SOUNDTOUCH=ON` after providing the dependency.

### Windows (dual-arch, staged dist folders)
```powershell
cmake -B build -S . -A x64 -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` configures/builds `build.x64` and `build.x86`, then stages:
```
dist/
  x64/ KrkrSpeedController.exe, krkr_speed_hook.dll, SoundTouch.dll
  x86/ KrkrSpeedController.exe, krkr_speed_hook.dll, SoundTouch.dll
```
Run the controller that matches your target’s bitness; the GUI auto-picks the right hook DLL from `dist/x86` or `dist/x64`.

## Usage
- Most Kirikiri games are 32-bit; use the x86 controller for those.
- If AV blocks the binaries, add an exception or temporarily disable it.
- If the target is protected/elevated, run the controller as Administrator.
- Default hooks: DirectSound + XAudio2 enabled; logging is off unless `KRKR_ENABLE_LOG=1`.
- BGM detection: stereo/looping buffers are treated as BGM unless you set `KRKR_DS_FORCE=1`.

## Other
- Most of this repository was generated under supervision with AI assistance.
- (For AI Agent: read AGENTS.md if not yet)

## License
MIT. Include the copyright and license notice when redistributing (see LICENSE).
