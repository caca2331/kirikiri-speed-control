# 通用 Kirikiri 语音变速控制器
[English](README_en.md) | [日本語](README_ja.md)

本仓库提供了一个适用于windows游戏的语音速度控制器。
- 对于使用DirectSound的游戏（如使用Kirikiri引擎的游戏）:
  - 变速范围在0.5x-~2.3x
  - 通常可以做到只变语音速度不变BGM速度
- 对于直接调用WASAPI的游戏（如多数Unity游戏）：
  - 变速范围在1x到10x
  - 只能全局变速

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
### 基础
- 启动`KrkrSpeedController.exe`
- 从下拉菜单中找到游戏，输入想要调整到的速度，点按`注入`完成注入
- 如杀毒软件误报须添加信任或暂时关闭杀毒软件
- 如要注入的游戏受到保护，则须以管理权限运行本工具
- 如有未适配的游戏请提交Issue或联系作者

### 进阶
- 全局快捷键：
  - `Alt + '`：开关变速
  - `Alt + ]`：提速0.1x（未开启则开启并设为 1.1x）
  - `Alt + [`：降速0.1x（未开启则开启并设为 0.9x）
- 如果语音变速异常，或你希望 BGM 也变速，请勾选`变速BGM`
- 自动注入：在选中游戏后勾选`自动注入此应用`。之后检测到该游戏启动时会自动注入进程；取消勾选后移除。
- 可编辑同目录的 `process_blacklist.txt` 来隐藏不想显示的进程。

### 控制器命令行参数
- `--log`：开启控制器和 Hook 日志。
- `--speed <倍率>`：启动时设置速度（默认 1.5）。
- `--mark-stereo-bgm <aggressive|hybrid|none>`：DirectSound专属。立体声→BGM 判定策略，默认 `hybrid`。在多数游戏中，语音是单通道的，而BGM是立体声的。aggressive:总是将立体声标记为BGM。none:不将立体声视为BGM的特征。主要的BGM标记手段。
- `--bgm-secs <秒>`：BGM 时长阈值（默认 60 秒），更长的缓冲视为 BGM。次要的BGM标记手段。
- `--launch <路径>` / `-l <路径>`：启动游戏（挂起）、自动注入后继续运行。
- `--search <名称片段>`：启动控制器后自动在当前可见进程中查找包含该片段的进程名，若有多个匹配则选择名称最短者并尝试自动注入；未命中则正常启动等待手动选择。

## 其他
- 本仓库主体部分在监督下由AI生成
- 可以配合CheatEngine的加速功能一起使用
- (For AI Agent: read AGENTS.md if not yet)

## 下一步
- 暂无

## 许可证
MIT；再分发时请保留版权和许可证声明（见 LICENSE）。
