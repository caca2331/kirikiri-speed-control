# Universal Kirikiri Voice Speed Controller – Implementation Guide (Dec 2025)

This guide is the “how it works” document for re‑implementing the controller and hooks. It reflects the **current, tested logic**—DirectSound is the reference path; other hooks should mirror it (Unity titles included).

## 1. Objective
- Inject a DLL into target games and time‑stretch **speech** to match game speed (0.5×–2.0×) without pitch distortion and with minimal latency (<100 ms).

## 2. Stack & Build
- C++17, MSVC static CRT.
- SoundTouch (WSOLA) for DSP; defaults: sequence 35 ms, overlap 10 ms, seek 25 ms (tune per voice).
- MinHook for IAT/vtable patching.
- Builds: x86 mandatory, x64 optional. `dist_dual_arch` stages both.

## 3. Core Flow
```
Game → Hook (DS/XA/FMOD/Wwise) → DSP (SoundTouch) → Output device
```
Each intercepted buffer is frequency‑scaled (tempo) then pitch‑restored so the user speed is achieved without pitch shift.

## 4. AudioStreamProcessor (reference behavior)
- Cbuffer is **output FIFO** (already DSP’d). It is **not** reprocessed.
- Per Unlock (must fill Abuffer length immediately):
  1) Copy from Cbuffer into output until full or Cbuffer empty.
  2) Process **only the new slice** with SoundTouch (pitch mode, ratio = 1 / appliedSpeed).
  3) If DSP overproduces, stash excess into Cbuffer; if underproduces, **front‑pad zeros** to reach exact Abuffer size.
  4) Cap Cbuffer to ~0.1 s; drop newest overflow, keep oldest for continuity.
- Idle reset: if idle >200 ms beyond predicted play end, clear Cbuffer and `flush()` SoundTouch state.

## 5. DirectSound Hook (tested)
- Hook points: DirectSoundCreate/DirectSoundCreate8 → CreateSoundBuffer → Unlock/Release (secondary PCM16 only).
- Classification: mono → likely voice; long/stereo → likely BGM; length gate default 60 s; optional process‑all‑audio.
- Speed control per Unlock:
  - DesiredFreq = clamp(baseFreq * userSpeed, DSBFREQUENCY_[MIN,MAX]).
  - Always `GetFrequency` and `SetFrequency` if mismatch (overrides games that keep resetting frequency).
  - AppliedSpeed = DesiredFreq / baseFreq; feed this to SoundTouch (pitch mode) in AudioStreamProcessor.
- DSP result is written back to the locked regions; tail is kept in Cbuffer (not re‑DSPed).
- Logs: `--log` enables; debug audio dumps via controller flag write WAVs to `audiolog/{original,changed}`.

## 6. XAudio2 / FMOD / Wwise (expected parity)
- Mirror DS semantics: do not reprocess tail; enforce desired playback rate each submit/unlock; exact buffer length respected; same speed math (clamp, appliedSpeed, pitch‑restore). These paths are less tested—align them to DS when fixing bugs (Unity games especially).

## 7. Controller (KrkrSpeedController.exe)
- Enumerates visible processes, writes shared settings (speed, gates, stereo‑BGM mode, process‑all) to per‑PID shared memory, launches injector, and reports status.
- CLI highlights: `--log`, `--debug-audio-log`, `--process-all-audio`, `--mark-stereo-bgm <aggressive|hybrid|none>`, `--bgm-secs <N>`, `--search <term>`, `--launch <exe>`.

## 8. Injection & Shared Settings
- Injector writes the hook DLL path into remote process via LoadLibraryW.
- Shared settings name: `Local\KrkrSpeedSettings_<pid>`; fields include userSpeed, length gate, processAllAudio, stereoBgmMode, logging flags, skip hooks, safe mode.
- Hook reads settings at attach; DS/XA polls periodically for updates.

## 9. Stability Practices
- Never block audio threads; fail soft: on exceptions, disable processing for that path.
- Validate pointers, buffer sizes; guard SetFrequency/Submit failures with logs.
- Keep buffers alive until engine callbacks signal completion (pendingBuffers in XA; Unlock path in DS).

## 10. Known Gaps / To‑Do
- Bring XAudio2/FM0D/Wwise to full parity with DS (tail handling, per‑submit freq enforce).
- More coverage on Unity titles that use XA/FM0D backends.
- Further tuning of SoundTouch params for very short slices (<60 ms).

Use this guide to reimplement or extend the controller: follow the DS hook and AudioStreamProcessor behaviors as the canonical reference. 
