# Universal Kirikiri Voice Speed Controller (Skeleton)

This repository contains an early scaffold for a Kirikiri voice speed controller based on the provided technical specification. The Windows-specific hook targets are not built on non-Windows hosts, but the DSP core and tests can still be compiled for smoke testing.

## File-by-file overview
- `CMakeLists.txt`: Configures the shared DSP library, optional Windows-only hook/GUI targets, and the DSP smoke test;
  toggles SoundTouch support via `USE_SOUNDTOUCH`, logging via `ENABLE_LOGGING`, and guards GUI/DLL builds behind
  platform checks.
- `src/common/Logging.h` / `src/common/Logging.cpp`: Lightweight temp-file logger with `KRKR_LOG_*` helpers; enabled by
  default and compiled out with `-DENABLE_LOGGING=OFF`.
- `src/common/DspPipeline.h` / `src/common/DspPipeline.cpp`: Declares and implements the tempo-adjustment pipeline,
  including pluggable DSP backends, buffer management helpers, and WSOLA-aligned defaults for speech.
- `src/common/VoiceContext.h`: Defines the `VoiceContext` structure that tracks user speed, engine frequency ratio, speech
  classification flags, and queued buffer metadata for safe lifetime handling.
- `src/hook/XAudio2Hook.h` / `src/hook/XAudio2Hook.cpp`: Placeholder hook surface for XAudio2 with version detection stubs,
  vtable scanning helpers, and submit-voice interception points awaiting full MinHook wiring.
- `src/hook/DirectSoundHook.h` / `src/hook/DirectSoundHook.cpp`: Skeleton for DirectSound8 interception, outlining shadow-
  buffer handling and hook registration points for future implementation.
- `src/hook/dllmain.cpp`: DLL entry that initializes logging and kicks off hook setup on load; currently stubbed to avoid
  running on unsupported hosts during development.
- `src/gui/main.cpp`: Win32 controller UI with a refreshable process dropdown, speed entry box (0.75–2.0×), and “Hook +
  Apply” button that attempts to inject `krkr_speed_hook.dll` into the selected PID; closing the window exits the app.
- `tests/dsp_smoke.cpp`: Generates synthetic sine data to validate tempo changes across multiple ratios and channel counts,
  emitting log lines alongside console output so runs are traceable even if the console closes quickly.

## Building
Use CMake to generate a Visual Studio solution. SoundTouch integration is disabled by default to keep the scaffold self-contained; enable with `-DUSE_SOUNDTOUCH=ON` after providing the dependency. Logging is enabled by default and writes to per-process temp files; disable at configure time with `-DENABLE_LOGGING=OFF` to compile out the log helpers.

### Windows (full targets)
```powershell
cmake -B build -S . -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release
```

### Non-Windows smoke tests only
```bash
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_GUI=OFF
cmake --build build
ctest --test-dir build -V
```

## Runtime Notes
- When `USE_SOUNDTOUCH=ON`, the build now stages `SoundTouch.dll` beside `KrkrSpeedController.exe`, `krkr_speed_hook.dll`, and `dsp_smoke.exe` so the binaries run without manual dependency copying.
- With `ENABLE_LOGGING=ON` (default), each binary writes to `%TEMP%/krkr_speed_<pid>.log`, covering the controller UI,
  hook DLL, and smoke tests for easier troubleshooting. Disable logging at configure time if you want silent binaries.
- `KrkrSpeedController.exe` opens a Win32 UI: refresh the running process list, select a target, enter a speed between 0.75
  and 2.0×, and press “Hook + Apply” to attempt `krkr_speed_hook.dll` injection from the same folder. Closing the window
  cleanly exits the controller.
- `dsp_smoke.exe` prints progress to stdout and logs so the run is traceable even when launched by double-clicking.

## Next Steps
- Wire MinHook to the XAudio2 and DirectSound entry points.
- Replace the DSP fallback with the tuned SoundTouch configuration (sequence/overlap/seek window).
- Implement voice classification heuristics and IPC so the controller UI can drive live speed changes inside injected games.

---

# 通用Kirikiri语音变速控制器（骨架版）

本仓库提供一个按照技术规范搭建的Kirikiri语音变速控制器脚手架。在非Windows环境下不会编译Windows特定的Hook和GUI目标，但仍可编译DSP核心与冒烟测试。

## 文件逐一说明
- `CMakeLists.txt`：配置共享DSP库、可选的仅限Windows的Hook/GUI目标以及DSP冒烟测试；通过`USE_SOUNDTOUCH`切换
  SoundTouch支持，通过`ENABLE_LOGGING`开关日志，并用平台判断保护GUI/DLL构建。
- `src/common/Logging.h` / `src/common/Logging.cpp`：轻量级临时文件日志与`KRKR_LOG_*`宏，默认开启，可用
  `-DENABLE_LOGGING=OFF`在编译阶段裁掉。
- `src/common/DspPipeline.h` / `src/common/DspPipeline.cpp`：声明并实现变速处理管线，包括可插拔的DSP后端、缓冲区管理
  工具，以及针对语音的WSOLA默认参数。
- `src/common/VoiceContext.h`：定义`VoiceContext`结构，用于跟踪用户速度、引擎频率比、语音分类标志以及缓冲区元数据
  队列以保证生命周期安全。
- `src/hook/XAudio2Hook.h` / `src/hook/XAudio2Hook.cpp`：XAudio2拦截的占位实现，包含版本检测骨架、虚表扫描辅助方法，
  以及等待与MinHook对接的提交拦截点。
- `src/hook/DirectSoundHook.h` / `src/hook/DirectSoundHook.cpp`：DirectSound8拦截的空架构，描述影子缓冲处理和未来的Hook注
  册入口。
- `src/hook/dllmain.cpp`：DLL入口，在加载时初始化日志并启动Hook设置；为避免在不支持的主机上运行，目前保持空操作。
- `src/gui/main.cpp`：Win32控制器界面，带可刷新进程下拉框、0.75–2.0倍速输入框和“Hook + Apply”按钮，尝试将
  `krkr_speed_hook.dll`注入选定PID，关闭窗口即退出。
- `tests/dsp_smoke.cpp`：生成合成正弦波数据，在多种倍速和声道数量下验证变速效果，并同步写入日志，便于在控制台关闭
  时仍能追踪运行情况。

## 构建方式
使用CMake生成Visual Studio解决方案。默认关闭SoundTouch以保持脚手架自包含；在准备好依赖后可通过`-DUSE_SOUNDTOUCH=ON`开启。日志默认开启并写入临时目录的进程日志，若需要静默构建可在配置时加`-DENABLE_LOGGING=OFF`裁剪日志代码。

### Windows（完整目标）
```powershell
cmake -B build -S . -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release
```

### 非Windows（仅冒烟测试）
```bash
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_GUI=OFF
cmake --build build
ctest --test-dir build -V
```

## 运行说明
- 当启用`USE_SOUNDTOUCH=ON`时，构建会自动把`SoundTouch.dll`放到`KrkrSpeedController.exe`、`krkr_speed_hook.dll`和`dsp_smoke.exe`同目录下，无需手动拷贝依赖即可运行。
- `ENABLE_LOGGING=ON`（默认）时，每个进程都会写日志到`%TEMP%/krkr_speed_<pid>.log`，覆盖控制器、Hook DLL和冒烟
  测试，便于排查问题；如需静默运行可在配置阶段关闭日志。
- `KrkrSpeedController.exe` 现为Win32界面：刷新进程列表、选择目标、输入0.75–2.0倍速，点击“Hook + Apply”尝试从
  同目录注入`krkr_speed_hook.dll`；关闭窗口即退出控制器。
- `dsp_smoke.exe` 会同时输出到控制台和日志，即使双击运行控制台快速关闭，也能在日志中查看结果。

## 下一步计划
- 将MinHook接入XAudio2和DirectSound入口。
- 用针对语音的SoundTouch参数（序列/重叠/搜索窗口）替换当前的后备方案。
- 实现语音分类启发式，并加入IPC以便控制器界面能在被注入的进程中实时调整速度。
