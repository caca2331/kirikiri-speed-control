# 通用 Kirikiri 语音变速控制器
[English](README_en.md)

本仓库提供了一个适用于windows的，控制基于 Kirikiri 游戏的语音速度的控制器。

## 构建
### Windows（双架构并打包到 dist）
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` 会配置/编译 `build.x64` 与 `build.x86`，并将文件放入：
```
dist/
  KrkrSpeedController/
    KrkrSpeedController.exe, SoundTouch.dll
    x86/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
    x64/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
```
x86 控制器可注入 x86 和 x64 游戏：控制器会根据目标进程位数选择匹配的 injector 和 Hook DLL（x86/x64 子目录）。

## 使用
- 启动`KrkrSpeedController.exe`
- 点按`Refresh`，从下拉菜单中找到游戏，输入想要调整到的速度，点按`Hook + Apply`
- 如杀毒软件误报须添加信任或暂时关闭杀毒软件
- 如要注入的游戏受到保护，则须以管理权限运行本工具
- 如果变速异常，或你希望 BGM 也变速，请勾选`Process BGM`后点击`Hook + Apply`
- 进阶用法请参照控制器命令行参数部分
- 如有未适配的游戏请提交Issue或联系作者

### 控制器命令行参数
- `--log` / `--enable-log`：开启控制器和 Hook 日志。
- `--log-dir <路径>`：指定日志输出目录（默认与控制器同目录，即 `dist/KrkrSpeedController`）。
- `--mark-stereo-bgm <aggressive|hybrid|none>`：立体声→BGM 判定策略，默认 `hybrid`。在多数游戏中，语音是单通道的，而BGM是立体声的。主要的BGM标记手段。
- `--bgm-secs <秒>`：BGM 时长阈值（默认 60 秒），更长的缓冲视为 BGM。次要的BGM标记手段。
- `--process-all-audio`：变速所有音频，包括BGM等。
- `--launch <路径>` / `-l <路径>`：启动游戏（挂起）、自动注入后继续运行。
- `--search <名称片段>`：启动控制器后自动在当前可见进程中查找包含该片段的进程名，若有多个匹配则选择名称最短者并尝试自动注入；未命中则正常启动等待手动选择。
- `--skip-ds` / `--skip-xaudio2` / `--skip-fmod` / `--skip-wwise`：跳过对应音频 Hook。Ds (DirectSound)为最常用的hook。多数情况下其他hook可以不开启。
- `--safe-mode`：安全模式，仅载入不打补丁（用于排障）。

## 其他
- 本仓库主体部分在监督下由AI生成
- (For AI Agent: read AGENTS.md if not yet)

## 下一步
- 适配其他引擎的游戏

## 许可证
MIT；再分发时请保留版权和许可证声明（见 LICENSE）。
