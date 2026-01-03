# 汎用 Kirikiri 音声変速コントローラー
[English](README_en.md) | [中文](README.md)

本リポジトリは Windows ゲーム向けの音声再生速度コントローラーを提供します。
- DirectSound を使うゲーム（例: Kirikiri エンジンの多くの作品）:
  - 変速範囲は 0.5x〜約 2.3x
  - 通常は BGM を変えずに音声のみ変速できます
- WASAPI を直接呼ぶゲーム（例: 多くの Unity タイトル）:
  - 変速範囲は 1x〜10x
  - 全体の音声のみ変速できます

## ビルド
### Windows（両アーキテクチャをビルドし dist に配置）
```powershell
cmake -B build -S . -A x64 -DBUILD_GUI=ON
cmake --build build --config Release --target dist_dual_arch
```
`dist_dual_arch` は `build.x64` と `build.x86` を設定/ビルドし、以下に配置します:
```
dist/
  KrkrSpeedController/
    KrkrSpeedController.exe, SoundTouch.dll
    x86/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
    x64/ krkr_injector.exe, krkr_speed_hook.dll, SoundTouch.dll
```
x86 コントローラーは x86 と x64 のゲームの両方に注入できます。対象プロセスに合わせて injector を選び、対応する Hook DLL（x86/x64 のサブフォルダ）を使用します。

## 使い方
### 基本
- `KrkrSpeedController.exe` を起動します。
- ドロップダウンからゲームを選び、目標速度を入力して `Hook` を押します。
- AV がブロックする場合は例外登録または一時的に無効化してください。
- 保護/管理者権限が必要なゲームは管理者として起動してください。
- 未対応のゲームは Issue で報告してください。

### 進階
- グローバルホットキー:
  - `Alt + '`：変速の ON/OFF
  - `Alt + ]`：0.1x ずつ加速（OFF の場合は 1.1x で ON）
  - `Alt + [`：0.1x ずつ減速（OFF の場合は 0.9x で ON）
- 音声変速がうまくいかない場合、または BGM も変速したい場合は `Process BGM` をチェックしてください。
- 自動注入：ゲームを選択して `Auto-Hook This App` にチェックを入れると、起動検知時に自動注入します。解除で設定を削除します。
- コントローラーと同じフォルダの `process_blacklist.txt` を編集すると、ドロップダウンから隠したいプロセスを指定できます。

### コントローラーの CLI オプション
- `--log`：コントローラーと Hook のログを有効化。
- `--speed <倍率>`：起動時の速度を指定（既定 1.5）。
- `--mark-stereo-bgm <aggressive|hybrid|none>`：DirectSound 専用。ステレオ→BGM 判定の設定（既定 `hybrid`）。多くのゲームでは音声がモノラルで BGM がステレオのため、主な判定に使います。`aggressive`：常にステレオを BGM とみなします。`none`：ステレオを BGM 判定の根拠にしません。主な BGM 判定手段です。
- `--bgm-secs <秒>`：BGM 判定の長さしきい値（既定 60 秒）。長いバッファを BGM とみなす補助判定です。
- `--launch <パス>` / `-l <パス>`：ゲームを一時停止で起動し、自動注入後に再開。
- `--search <名前>`：起動時に可視プロセス名の部分一致で検索し、複数ある場合は最短名を選択して自動注入。見つからない場合は通常起動で待機します。

## その他
- 本リポジトリの大部分は AI 支援のもとで生成されています。
- Cheat Engine の速度変更と併用できます。
- (AI エージェント向け: 未読なら AGENTS.md を読むこと)

## 次の予定
- なし

## ライセンス
MIT。再配布時は著作権とライセンス表記を保持してください（LICENSE 参照）。
