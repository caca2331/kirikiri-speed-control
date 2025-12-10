# 通用 Kirikiri 语音变速控制器
[English](README.md)

本仓库提供了一个 Kirikiri 语音变速控制器骨架。非 Windows 平台不会编译 Hook/GUI 目标。

## 构建
使用 CMake 生成 Visual Studio 解决方案。默认关闭 SoundTouch 以保持自包含；在准备好依赖后可通过 `-DUSE_SOUNDTOUCH=ON` 启用。

### Windows（双架构并打包到 dist）
```powershell
cmake -B build -S . -A x64 -DUSE_SOUNDTOUCH=ON -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` 会配置/编译 `build.x64` 与 `build.x86`，并将文件放入：
```
dist/
  x64/ KrkrSpeedController.exe, krkr_speed_hook.dll, SoundTouch.dll
  x86/ KrkrSpeedController.exe, krkr_speed_hook.dll, SoundTouch.dll
```
运行与目标进程位数相同的控制器；GUI 会自动选择对应的 Hook DLL（dist/x86 或 dist/x64）。

## 使用
- 大多数krkr游戏都是32位的，此时使用x86版本的controller
- 如杀毒软件误报须添加信任或暂时关闭杀毒软件
- 如要注入的游戏受到保护，则须以管理权限运行本工具
- 如有未适配的游戏请提交Issue或联系作者
- 默认启用 DirectSound 与 XAudio2 Hook；日志可用 `KRKR_ENABLE_LOG=1` 打开
- BGM 判定：立体声或循环缓冲默认视为 BGM，除非设置 `KRKR_DS_FORCE=1`

## 其他
- 本仓库主体部分在监督下由AI生成
- (For AI Agent: read AGENTS.md if not yet)

## 下一步
- 适配其他引擎的游戏
- 完善环境变量

## 许可证
MIT；再分发时请保留版权和许可证声明（见 LICENSE）。
