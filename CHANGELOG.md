## [1.2.0] - 2026-01-03
### Added
- WASAPI hook with tempo-based speedup path (Unity titles supported)
- Process blacklist file (`process_blacklist.txt`) to hide common/system processes
- Injected process tagging in dropdown (`[Injected][pid] ...`)
- Single-instance controller protection
- Rolling status bar with auto-scroll and hooked-process exit notifications
- Elevated injector fallback when non-elevated injection fails
- Japanese README
### Changed
- WASAPI hooking/processing refinements and cleanup
- UI tweaks: status area height increased, tooltip updates for WASAPI mode
### Removed
- Removed unused hooks, including Fmod, XAudio2, Wwise.
- Removed several unused arg flags

## [1.1.0] - 2026-01-02
### Added
- Auto-hook feature with per-game config (auto_hook + process_bgm)
- Optional delayed auto-hook toggle (configurable delay)
- Hotkey to change 
- Speed hotkeys and `--speed` CLI argument
- UI localization with language dropdown, including Chinese, English, and Japanese
- Tooltips for labels and controls (including hotkey help)
### Changed
- UI layout updates and dropdown selection behavior refinements
- Dist staging structure changed for clarity

## [1.0.1] - 2025-12-29
### Fixed
- Passthrough empty audio buffer
- Fixed frequency changed by game: now check (and set) frequency for every sound buffer
- Fixed artifacts in streamed audios
### Changed
- Updated guide
### Added
- `--search` argument
### Other
- Cleanup

## [1.0.0] - 2025-12-14
### Changed
- Restructured
- Minor fixes
- Detailed readme
- Handles more cases

## [0.1.0] - 2025-12-10
### Added
- Initial release
