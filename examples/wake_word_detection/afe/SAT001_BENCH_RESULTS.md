# SAT-001 bench validation results

Hardware-measured results for the N.E.R.V.E. SAT-001 room-satellite prototype.
Governance: task **NV-SAT-001**, readiness **BLOCKED** by **NV-HW-001**. This is
bench validation and specification only — it adds no N.E.R.V.E. product code, no
Brain-side surface, and no security mechanism.

**Hardware.** ReSpeaker Lite Voice Assistant Kit — XIAO ESP32-S3 + XMOS XU316 +
4 Ω 5 W speaker. ESP-IDF v5.5.5. The XU316 firmware was **not** flashed or
altered at any point.

**Date.** 2026-08-29. Firmware `67aef4b`.

---

## Endpoint facts (for NV-HW-001)

| Fact | Value | How established |
|---|---|---|
| Input format | `MM` — 2 microphone, **0 playback reference** | `bsp_get_input_format()`, and AFE runtime log `Input PCM Config: total 2 channels(2 microphone, 0 playback)` |
| Acoustic echo cancellation | **None on the ESP side** | Follows from the absence of a reference channel. Whether the XU316 performs its own AEC is **unverified** and cannot be checked without inspecting XU316 firmware (out of scope) |
| Consequence | Endpoint **must** run half-duplex | Wake detection is hard-gated while the speaker is active |
| Mute control | **Observe-only.** The S3 has no mute line to the XU316 | Mute is detected as sustained digitally-exact silence on the mic stream; confirmed working, so the XU316 does emit hard zeros when muted |
| RGB indicator | XU316-driven, **not addressable** from the S3 | No GPIO path in the board definition |
| AFE pipeline | `[input] -> SE(BSS) -> VAD(vadnet1_medium) -> WakeNet(wn9_jarvis_tts) -> [output]` | Runtime log |
| Frame size | 512 samples @ 16 kHz = **32 ms** | `get_fetch_chunksize()` |
| Wake word | `wn9_jarvis_tts` — **temporary** | Final phrase is "Okay Nerve"; Espressif model request pending |

### Measured timings

| Metric | Observed |
|---|---|
| Wake → listening | **217–223 ms** (n=7, very stable) |
| Connect (LAN, host listening) | 4–37 ms |
| Uplink, ~2.6 s utterance (83 KB) | 51–70 ms |
| Response first byte (echo server) | 783–973 ms |
| Playback vs audio duration | 2517/2592, 2932/3008, 7933/8000 ms |
| Full round trip | **3971–4189 ms** |
| Speech peak level | −9.6 to −33.3 dBFS |
| VAD lead-in recovered via `vad_cache` | 288–384 ms |

### Memory (5+ cycles, no leak)

```
internal_free   73751 -> 73923 -> 73931 -> stable
internal_min    26848 -> 26636 -> 26636  (stabilised)
spiram_free     7008684 constant throughout
boot            internal_free=105623  spiram_free=7026608
```

---

## Validation status

| Path | Status | Evidence |
|---|---|---|
| Wake → capture → uplink → response → playback | **PASS** | 191488 B and 256000 B echoed and played |
| **Accept-then-stall (the original hang)** | **PASS** | Server accepted 268288 B then sent nothing; SAT gave up on its 6 s first-byte deadline. Server logged `client closed after 5.16s`. Old firmware had **no recv timeout** and would have blocked forever, stranding the satellite |
| Connect **refused** (fast RST) | **PASS** | `so_error=104`, recovered in 12 ms |
| Connect **timeout** (blackhole `192.168.0.222`) | **PASS** | `connect timed out after 3000ms`, `total_ms=3006` |
| Recovery leaves satellite usable | **PASS** | Second wake always heard after a failure; `STREAMING/THINKING -> ERROR -> IDLE` |
| Half-duplex gate | **PASS** | `wake detection GATED (speaker active, no AEC)` → `RE-ARMED` on every speaking cycle; no self-trigger observed |
| Busy rejection | **PASS** | `wake ignored: busy (THINKING)` |
| VAD-driven utterance end | **PASS** | `reason=silence`, `speech_ms == vad_speech_ms` |
| First-word protection (`vad_cache`) | **PASS** | `captured_ms` exceeds `listen_ms` by the lead-in |
| Mute engaged | **PASS** | `hardware mute ENGAGED (observed)` → `MUTED`; wake word spoken during 162 s muted window produced **no** detection |
| Mute released | **PASS** | `hardware mute RELEASED (observed)` → `IDLE`; wake worked immediately |
| No leak | **PASS** | See memory table |
| Endurance beyond ~7 cycles | **NOT TESTED** | Each cycle needs a spoken wake word |

---

## Defect found and fixed during bench validation

**Energy fallback overrode VAD and held the microphone open.** The energy
detector was OR'd with VAD when classifying each frame. A normal room noise
floor sits above the −45 dBFS threshold, so energy reported speech on ~98 % of
frames, trailing silence never accumulated, and **every** utterance ran to the
8000 ms cap:

```
cycle=1 reason=max_utterance listen_ms=8000 vad_speech_ms=2560 energy_speech_ms=7872
cycle=3 reason=max_utterance listen_ms=8000 vad_speech_ms=2144 energy_speech_ms=7840
```

VAD was correct throughout and had found ~2.2 s of real speech. Fixed in
`67aef4b` by making VAD the sole authority for timing and demoting energy to a
rescue that can only override an *abort*, and only when VAD produced nothing at
all **and** the audio was unmistakably loud.

After the fix: uplink **268 KB → 83 KB**, round trip **9440 ms → 3971 ms**.

A later quiet utterance (`peak=−33.3 dBFS`) showed energy *under*-reporting
(960 ms vs VAD's 1504 ms) — independent confirmation that energy was the wrong
signal to drive timing in either direction.

---

## Scope guard

The transport used here is **raw, unauthenticated TCP on port 5005** to a fixed
IPv4 address, plus temporary UFW allowances on Arvis. **This is bench
scaffolding and must not become the production protocol.** The production
transport is a dedicated authenticated N.E.R.V.E. satellite transport carrying
binary PCM, with the satellite paired as a device through the **existing**
N.E.R.V.E. pairing/token flow — no parallel identity system. None of that is
authorised while NV-SAT-001 is BLOCKED by NV-HW-001.
