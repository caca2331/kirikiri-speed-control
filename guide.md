# **Technical Specification: Universal Kirikiri Voice Speed Controller

(Hooks, DSP, GUI)**

## **1. Project Objective**

Create a Windows DLL (`krkr_speed_hook.dll`) that injects into Kirikiri games (Krkr2 / KrkrZ) and intercepts audio API calls to apply **real-time Time Stretching** (change playback speed without altering pitch).

### **Primary Target Range**

Time-stretching must be optimized specifically for **0.75× to 2.0×** playback speed for **speech** (voice lines).

### **Use Case**

When the game is running under speedhacks (e.g., Cheat Engine), speech becomes desynchronized or overlapped.
This tool ensures:

* Speech playback speed = Game speed
* No pitch distortion
* No noticeable latency

---

## **2. Technology Stack & Libraries**

### **Language & Build**

* C++17
* 32-bit (x86) DLL mandatory; x64 optional

### **Hooking**

* **MinHook**
  (`MH_CreateHook`, `MH_EnableHook`)

### **DSP Library**

* **SoundTouch**, configured and tuned specifically for *speech time-stretching in 0.75×–2.0× range*
* Optionally **Rubber Band Library** (if licensing allows) for higher-quality modes

### **Why SoundTouch (for this target range):**

* Low latency, stable real-time behavior
* Uses **WSOLA-family time-domain OLA algorithms**, ideal for voice
* Tunable parameters suitable for 0.75×–2.0× range
* Lightweight enough for in-game processing

---

## **3. Core Architecture Overview**

The DLL acts as an audio middleware layer between the game and the OS:

```
Game Engine → Hook Layer → DSP Processing → Audio Driver → Speakers
```

All PCM audio buffers submitted by the game are intercepted, time-stretched, and then forwarded to the real XAudio2 / DirectSound engine.

---

## **4. DSP Requirements (Optimized for 0.75×–2.0× Speech)**

### **4.1 Algorithm Selection**

Use SoundTouch with WSOLA-family time-stretching, configured with:

* **Adjusted sequence / overlap parameters tuned for voice**
* Reduced artifacting at:

  * Slowdown ~0.75× (avoid reverb/muddy quality)
  * Speedup ~1.5×–2.0× (avoid choppiness/robotic artifacts)

### **4.2 Recommended SoundTouch Settings**

The code agent must explicitly apply:

```
SETTING_SEQUENCE_MS    = 30–45
SETTING_OVERLAP_MS     = 8–12
SETTING_SEEKWINDOW_MS  = 20–30
```

(Values may be auto-adjusted dynamically based on speed ratio.)

### **4.3 Latency**

Total processing latency must remain **< 100 ms**.
A dynamic buffer policy should be implemented for low-latency voice playback.

---

## **5. XAudio2 Hooking Module (KrkrZ)**

### **5.1 Version Detection**

At DLL load:

1. Scan loaded modules for:

   * `XAudio2_7.dll` (Win7, COM)
   * `XAudio2_8.dll` / `XAudio2_9.dll` (Win8+, flat API)
2. Alternatively hook `LoadLibraryA/W` to detect future loads.

### **5.2 Hook Entry Points**

Depending on version:

| Version | Hook Function      |
| ------- | ------------------ |
| 2.7     | `CoCreateInstance` |
| 2.8/2.9 | `XAudio2Create`    |

Wrap the resulting `IXAudio2` instance.

### **5.3 Hook `CreateSourceVoice`**

Purpose:

* Inject our proxy callback object
* Track each voice through `VoiceContext`

### **5.4 Hook `SubmitSourceBuffer` (Core)**

This is where time-stretching occurs:

1. Copy PCM from user buffer
2. Feed into SoundTouch (`putSamples()`)
3. Retrieve processed PCM (`receiveSamples()`)
4. Construct a new `XAUDIO2_BUFFER`
5. Submit using the *original* function pointer
6. Maintain buffer memory until `OnBufferEnd` callback

### **5.5 Dynamic VTable Scanning**

Because XAudio2 VTable layouts vary:

* Create a dummy IXAudio2 instance at startup
* Scan virtual methods to identify:

  * `CreateSourceVoice`
  * `SubmitSourceBuffer`
  * `SetFrequencyRatio` (used only for internal state tracking, not pitch shift)

---

## **6. DirectSound8 Hooking Module (Krkr2)**

### **6.1 Hook Points**

* `DirectSoundCreate8`
* `IDirectSound8::CreateSoundBuffer`
* Hook buffer-level methods:

  * `IDirectSoundBuffer::Lock`
  * `IDirectSoundBuffer::Unlock`

### **6.2 Recommended Implementation: Shadow Buffer**

Instead of modifying the game's ring buffer directly:

1. Game writes PCM to a **fake ring buffer** (shadow input).
2. A dedicated mixer thread:

   * Reads from shadow buffer
   * Applies time-stretch
   * Writes the processed PCM to the *real* DirectSound buffer
3. Avoids cursor desynchronization and silence padding issues.

This design greatly improves stability and sound quality.

---

## **7. Voice Context & Classification**

### **7.1 VoiceContext Structure**

```cpp
struct VoiceContext {
    SoundTouch* dsp;
    float userSpeed;       // from GUI slider
    float engineRatio;     // internal XAudio2 SetFrequencyRatio state
    float effectiveSpeed;  // userSpeed * engineRatio
    bool isVoice;          // classification result
    std::deque<BufferMeta> pendingBuffers; // lifecycle mgmt
};
```

### **7.2 Improved Voice vs. BGM Classification**

Use multiple signals:

1. **Channels: mono → likely speech**
2. **Duration < 20 s → likely speech**
3. **Spectral features:**

   * Zero-crossing rate
   * RMS variability
4. **User overrides via GUI** (whitelist/blacklist)

Only process audio when `isVoice == true`.

---

## **8. Memory & Buffer Management Improvements**

To ensure stability:

### **8.1 Preallocated Memory Pools**

Avoid per-buffer `malloc/free`.
Use per-voice pools or lock-free ring buffers.

### **8.2 Reference-Counted Buffer Queue**

Buffers must be retained until XAudio2 invokes `OnBufferEnd`.

### **8.3 Thread-Safe DSP Calls**

Use lightweight spinlocks only; avoid blocking.

---

## **9. Global Speed Control Strategy**

Instead of a single global multiplier:

```
effectiveSpeed = userSpeed * engineFrequencyRatio
```

Where:

* `userSpeed` is controlled by GUI (0.75×–2.0×)
* `engineFrequencyRatio` comes from XAudio2 internal calls
* Only speech voices apply time-stretch

This ensures compatibility with in-game voice effects.

---

## **10. Minimal GUI Requirement (New)**

The tool must include a small GUI application (`KrkrSpeedController.exe`).

### **10.1 Purposes**

* Allow user to **select a target process** (list running processes; choose Krkr game)
* Inject DLL automatically
* Display **detected voices**, buffer statistics, and playback latency
* Provide **live speed adjustment slider** (0.75×–2.0×)
* Toggle:

  * “Process speech only”
  * “Process all audio”
  * Verbose logging

### **10.2 Recommended Framework**

* Win32 API or ImGui (static runtime, light footprint)
* Must support:

  * Process enumeration
  * DLL injection
  * IPC with the injected DLL (Named Pipe or Shared Memory)

The GUI must be optional:
If DLL is injected manually, default speed = 1.0×.

---

## **11. Implementation Roadmap (For Code Agent)**

1. **Create DLL project**

   * Integrate MinHook + SoundTouch
   * Implement `DllMain` → start initialization thread

2. **XAudio2 Module**

   * Version detector
   * Hook entry points
   * Dynamic VTable scanner
   * Implement SubmitSourceBuffer wrapper

3. **DSP Integration**

   * Tuned SoundTouch settings for speech
   * Effective speed calculation

4. **Voice Classification**

   * Multi-feature heuristic
   * User overrides

5. **DirectSound Module**

   * Implement Shadow Buffer playback architecture

6. **Memory Safety**

   * Buffer lifetime tracking
   * Reference-counted queues
   * Preallocated pool

7. **GUI Application**

   * Process list + selection
   * DLL injection
   * Real-time controls and status display

8. **Testing**

   * Krkr2 examples: Fate/stay night
   * KrkrZ examples: 9-nine-, Maitetsu

---

## **12. Stability & Crash Prevention**

* All DSP work must be exception-safe
* Never block audio engine threads
* Validate pointers and buffer sizes
* Automatically disable processing on repeated DSP failures
