# Universal Kirikiri Voice Speed Controller – Implementation Guide (Dec 2025)

This guide is the “how it works” document for understanding, implementing, and re‑implementing the controller and hooks. It reflects the **current, tested logic**—DirectSound is the reference path; WASAPI should mirror it where possible (Unity titles included).

## 1. Objective
- Inject a DLL into target games and time‑stretch **speech** to match game speed (0.5×–2.0×) without pitch distortion and with minimal latency (<100 ms).

## 2. Stack & Build
- C++17, MSVC static CRT.
- SoundTouch (WSOLA) for DSP; defaults: sequence 35 ms, overlap 10 ms, seek 25 ms (tune per voice).
- MinHook for IAT/vtable patching.
- Builds: x86 mandatory, x64 optional. `dist_dual_arch` stages both.

## 3. Core Flow
```
Game → Hook (DirectSound/WASAPI) → DSP (SoundTouch) → Output device
```
Each intercepted buffer is frequency‑scaled (tempo) then pitch‑restored so the user speed is achieved without pitch shift.
Primary tracked interface is **DirectSound**. If no DirectSound buffers are observed, the WASAPI hook serves as the
fallback for untracked audio APIs (currently anything not routed through DirectSound).

## 4. AudioStreamProcessor (reference behavior)
- Cbuffer is **output FIFO** (already DSP’d). It is **not** reprocessed.
- Per Unlock (must fill Abuffer length immediately):
  1) Copy from Cbuffer into output until full or Cbuffer empty.
  2) Process **only the new slice** with SoundTouch (pitch mode, ratio = 1 / appliedSpeed).
  3) If DSP overproduces, stash excess into Cbuffer; if underproduces, **front‑pad zeros** to reach exact Abuffer size.
  4) Cbuffer has no hard cap; continuity is preserved and it is only cleared on idle reset.
- Tempo path (WASAPI): accumulate input in Abuffer until ~30 ms, then run SoundTouch in **tempo** mode. Output is filled from Cbuffer first, then new DSP output, and zero‑padded if needed.
- Idle reset: if idle >200 ms beyond predicted play end, clear Cbuffer/Abuffer and `flush()` SoundTouch state.

## 5. Two Processing Routes
### Route A — Voice/BGM distinguishable (DirectSound‑style)
- Identify likely voice vs BGM:
  - Mono buffers → voice‑leaning.
  - Long or stereo buffers → BGM‑leaning (length gate default 60 s).
  - Optional “process all audio” override.
- For **voice**:
  1) Compute `DesiredFreq = clamp(baseFreq * userSpeed, DSBFREQUENCY_[MIN,MAX])`.
  2) `AppliedSpeed = DesiredFreq / baseFreq`.
  3) Run **pitch mode** in AudioStreamProcessor with `ratio = 1 / AppliedSpeed` to restore pitch.
  4) Write back to the locked regions; overflow goes into Cbuffer (not re‑DSP’d).
  5) Enforce `SetFrequency(DesiredFreq)` each unlock to override game resets.
- For **BGM**:
  - Default: bypass DSP and keep base frequency.
  - If `processAllAudio`, use the same pipeline as voice.

### Route B — Voice/BGM indistinguishable (WASAPI‑style)
- Used when no tracked interface is active (currently only DirectSound).
- Always process PCM16 audio (no BGM gate).
- Hook points: `IAudioClient::Initialize` → `IAudioRenderClient::GetBuffer/ReleaseBuffer`.
- Per `ReleaseBuffer`:
  1) Compute `effectiveFrames` via the accumulator (`targetFrames/outFrames`) so long‑term output tracks `sum(inputFrames / speed)`.
  2) Run **tempo mode** (`SoundTouch::setTempo`) through `processTempoToSize`:
     - Abuffer accumulates until ~30 ms before DSP to stabilize SoundTouch.
     - Output size is exactly `effectiveFrames` (Cbuffer + new DSP output + zero padding if needed).
  3) Release only `effectiveFrames` (drop mode) to speed up playback without pitch shift.
- Speed‑down is not supported in this route without a proxy render client (would require buffering and backpressure).

## 6. DirectSound Hook (tested)
- Hook points: DirectSoundCreate/DirectSoundCreate8 → CreateSoundBuffer → Unlock/Release (secondary PCM16 only).
- Classification: mono → likely voice; long/stereo → likely BGM; length gate default 60 s; optional process‑all‑audio.
- Speed control per Unlock:
  - DesiredFreq = clamp(baseFreq * userSpeed, DSBFREQUENCY_[MIN,MAX]).
  - Always `GetFrequency` and `SetFrequency` if mismatch (overrides games that keep resetting frequency).
  - AppliedSpeed = DesiredFreq / baseFreq; feed this to SoundTouch (pitch mode) in AudioStreamProcessor.
- DSP result is written back to the locked regions; tail is kept in Cbuffer (not re‑DSPed).
- Logs: `--log` enables; debug audio dumps via controller flag write WAVs to `audiolog/{original,changed}`.

## 7. Controller (KrkrSpeedController.exe)
- Enumerates visible processes, writes shared settings (speed, gates, stereo‑BGM mode, process‑all) to per‑PID shared memory, launches injector, and reports status.
- CLI highlights: `--log`, `--debug-audio-log`, `--process-all-audio`, `--mark-stereo-bgm <aggressive|hybrid|none>`, `--bgm-secs <N>`, `--search <term>`, `--launch <exe>`.

## 8. Injection & Shared Settings
- Injector writes the hook DLL path into remote process via LoadLibraryW.
- Shared settings name: `Local\KrkrSpeedSettings_<pid>`; fields include userSpeed, length gate, processAllAudio, stereoBgmMode, logging flags, skipDirectSound, safe mode.
- Hook reads settings at attach; DS/WASAPI polls periodically for updates.

## 9. Stability Practices
- Never block audio threads; fail soft: on exceptions, disable processing for that path.
- Validate pointers, buffer sizes; guard SetFrequency/Submit failures with logs.
- Keep buffers alive until engine callbacks signal completion (Unlock path in DS; render client lifetimes in WASAPI).

## 10. Known Gaps / To‑Do
- More coverage on Unity titles that rely on WASAPI timing quirks.
- Further tuning of SoundTouch params for very short slices (<60 ms).

Use this guide to reimplement or extend the controller: follow the DS hook and AudioStreamProcessor behaviors as the canonical reference. 
