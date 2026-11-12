# Modern TTS engines — research notes (2026-11-08)

Source: Telegram group «Распознавание и синтез речи» (t.me/speech_recognition_ru,
N. Shmyrev / alphacep), messages 2024–2026-11, plus the local system state.
Focus: engines usable as a **system** voice (screen reader, notifications, "read selection"):
local, CPU, low latency, stream/interrupt, Russian + English.

## Local system (Xubuntu 26.04)
- speech-dispatcher 0.12.1; modules: espeak, espeak-ng(+mbrola), festival, flite, pico,
  rhvoice, baratinoo/cicero/ivona/kali/voxin (mostly dead or proprietary).
- RHVoice: universe has 1.8.0+dfsg without voices, but **multiverse has 1.14.0-3 with voices**
  (checked 2026-11-10): rhvoice-russian (aleksandr, anna, artemiy, elena, irina, arina,
  evgeniy-rus, mikhail, pavel, tatiana, timofey, umka, victoria, vitaliy(-ng), yuriy, vsevolod,
  aleksandr-hq), rhvoice-english (alan, bdl, clb, slt, evgeniy-eng, lyubov), + uk, pl, cs, …
  and speech-dispatcher-rhvoice 1.14. Voices are non-free (trained nets, no training scripts).
  Nothing to package; the app only needs multiverse enabled.
- Piper wired by hand: `~/.config/speech-dispatcher/modules/piper.conf` → `sd_generic`
  shell pipeline (piper → sox → paplay), binary/models inside the Pied flatpak dir.
  `DefaultModule piper`. Voices: ru_RU-denis-medium, en_US-ryan-high.
- speech-dispatcher ships no Piper config of its own in 0.12.1 (only user-made ones).

## Engines by class

### Classic (formant / unit selection) — fast, tiny, accelerate well
- **espeak-ng** — every language, robotic; also the phonemizer behind Piper.
- **RHVoice** (C++, LGPL-2.1, speechd/SAPI5/NVDA/Android) — the screen-reader choice for
  Russian; blind users run it very fast. Shmyrev (2026-06): "on weak phones even piper
  struggles, only rhvoice". Voices from rhvoice.su/voices; Ubuntu ships them in multiverse
  (rhvoice-russian, rhvoice-english, …), not in universe.

### Light neural, real-time on CPU — the target for a system voice
- **Piper** (VITS, ONNX, rhasspy/piper-voices on HF; now OHF-Voice/piper1-gpl) — the
  de-facto standard; "fine, but sometimes messes up" stress/intonation in Russian.
  Fixes: RuAccent, silero-stress, mitrokun/espeak-ng-data (Russian-improved espeak data).
- **Vosk TTS** — ports checked 2026-11-10: only Python upstream (pip vosk-tts: onnxruntime,
  tokenizers; BERT for prosody in the multi models, 0.9/0.10-multi load ~70 s, issue #46);
  andreytkachenko/vosk-tts-rs (Rust, ort, Apache-2.0, 0 stars, written in 3 days 2026-04,
  then abandoned); browser ports vlapky/vosk-tts-web, FedorMilovanov/gb-vosk-tts (JS,
  onnxruntime-web, so the pipeline is portable). No C/C++ port, no sherpa-onnx support.
  Shmyrev on speechd (vosk-tts #31, 2025-01): pipeline via echo is too slow, needs a native
  module like speechd pico.c, "when we stabilise the algorithm".
  (alphacep, Apache-2.0; 0.8 based on Matcha, 0.10-multi) — Shmyrev's answer
  to "best Russian TTS with low latency and streaming" (2026-05); good stress and
  intonation. Demos: mitrokun.github.io/voskvoice. Some report 0.8–0.9 s per sentence on
  CPU for the multi model.
- **TeraTTS v2** — Russian, "good for CPU" (Den4ik, 2026-08); Android TTS ports exist
  (davnozdu/supertonic-android, teratts-android).
- **Supertonic 3** (~385 MB) — fast multilingual; voices mixable; some artifacts.
- **Pocket TTS** (Kyutai, ~100M) — faster than real time on CPU, voice cloning from a short
  sample; **xVibePocketTTS** (GenVoice, 2026-09) = Russian fine-tune with RuAccent, streaming.
  Shmyrev: of the small ones only Pocket is really real-time.
- **Kokoro 82M** — great English quality; Russian only experimental (kokoro-ruslan).
- **Kokoro, Russian** (checked 2026-11-11): huggingface.co/zaakirio/kokoro-ru — Kokoro-82M
  trained for Russian (StyleTTS2 / kikiri-tts recipe), voices sveta + masha (one checkpoint)
  and dima (separate checkpoint), G2P with RUAccent (Python), text input; weights OpenRAIL,
  code Apache-2.0. ONNX exports: huggingface.co/da1353/kokoro-ru (Node G2P on espeak stress,
  worse). Not checked: whether its weights fit the stock Kokoro graph (then kokoro.cpp could
  run them), quality by ear.
  ggml runtimes: **kokoro.cpp** (simonfxr; GGUF huggingface.co/simonfxr/kokoro.cpp-GGUF,
  F16/Q8_0, 54 stock voices, CPU/Vulkan/CUDA/Metal, `kokoro-cli`) — the Vulkan candidate;
  TTS.cpp (mmwillet and forks) — proof of concept, patched ggml, macOS only tested.
  Same name, ONNX Runtime inside: koth/kokoro.cpp, rzafiamy (kokoro-server).
- **KittenTTS** — tiny, English.
- **Silero v5.5** (ru) — good, but restrictive licence (avoided in the group); RuVoice TTS
  (Android) builds on it.
- Benchmark: heyneo.com/blog/kokoro-supertonic-inflect-nano-pocket-tts-cpu-benchmark.

- **ZONOS3-Mini** (Zyphra, added 2026-11-10; github.com/Zyphra/ZONOS3-Mini, code MIT, model
  Apache-2.0 on HF Zyphra/ZONOS3-Mini, ~43 languages incl. ru/uk/en; repo from 2026-10-29).
  11-layer LM (gated DeltaNet + global attention, 64-expert MoE), 10 codebooks at 15 Hz,
  iSTFT decoder → 44.1 kHz. **Native C++/ggml port `servers/ggml` (zonos3.cpp)**: GGUF,
  CPU/CUDA/Vulkan/Metal, no Python at runtime; CLI + HTTP server (own v2 API with streaming
  float32 PCM, OpenAI-compatible /v1/audio/speech). Default backbone zonos3-ptq.gguf 0.66 GB
  (whole gguf/ folder 9.3 GB). Voice cloning from ≤ 60 s of audio → 192-dim speaker
  embedding (reusable without the encoder). To check: CPU real-time factor on the Ryzen 5
  3500U, Russian quality, whether a resident server + thin speechd module gives low latency.
  Packaging: own ggml submodule (not sherpa-onnx); a .deb of the CLI/server + a model .deb.
  **Measured 2026-11-10** (Ryzen 5 3500U, static bundled ggml, build in
  /home/grigory/Storage/Claude/zonos3-build, models 1.16 GB: ptq + zac-keep2 + spk-encoder):
  LM RTF 0.22–0.29, but the zac decoder is the bottleneck: RTF 0.99 with its default 4 threads,
  0.70 with `ZAC_THREADS=6` (8 no better), 0.62 with OpenBLAS (`-DGGML_BLAS=ON`, small gain).
  Total ≈ RTF 0.85 + ~2–3 s model load per process → only usable as a resident server.
  Without a speaker the voice is random per seed and often very quiet (en rms 0.006):
  pass `--loudness -20` (−16 clips) or a speaker embedding (`assets/default_voices/*.embedding.npy`;
  no Russian default voice). One ru run with the AmericanMale embedding came out half as long
  (text dropped?). Language is taken from the text, no flag. Samples: _tmp/zonos3/out/.
  User's listening (2026-11-10): only the runs without a speaker + `--loudness` sound right
  (blas-ru, blas-en); English default voices on Russian text glitch or have a heavy accent.
  → For Russian a Russian speaker embedding is needed: e.g. encode a good random-seed output
  with `spk-encoder-cli` to pin that voice, or a freely licensed Russian recording.
  **Done:** seeds 1, 7, 11, 23 (`--loudness -20`) → `spk-encoder-cli` embeddings
  (_tmp/zonos3/out/voices/seedN.embedding.npy); the voice holds on a new text (seed 99).
  User: all four good, seed 1 the best, 7 a bit plainer. Seed 4 broke (text cut, then 12 s).
  Needs ffmpeg at runtime for audio input (`--clone`, spk-encoder-cli).
  **speechd module `sd_zonos3` (2026-11-11, C + GLib, MIT; source _tmp/sd-zonos3, installed in
  the user's ~/.local/libexec/speech-dispatcher-modules + modules/zonos3.conf):** starts
  zonos3-server on a free localhost port on the first message (POST /v2/generate,
  `speaker_mode: embedding`), splits the text into sentences, and sends each one to speechd as soon as
  it is ready (speechd queues module audio without waiting for playback, so the next
  sentence is synthesized while the previous one plays). The first piece is cut at a comma after 40 chars
  (`Zonos3FirstChunk`). Voices are `zonos3-<lang>-<name>` from `*.embedding.npy`. Rate maps to
  `speaking_rate` in UTF-8 bytes/s (`14 · bytes/char · 2^(rate/100)`).
  The server survives module restarts: speechd restarts all modules on every SIGHUP, which the settings
  app sends on each change, and the model reloaded each time (26 s → first sound). Now
  `$XDG_RUNTIME_DIR/sd_zonos3.state` (pid port threads owner released + model) lets the new
  module adopt it; a quitting module leaves a reaper that stops the server after
  `Zonos3IdleTimeout` (600 s) unless adopted. After HUP: 7.5 s instead of 26 s for a short
  phrase incl. playback. Slowness on the loaded laptop: synthesis RTF ≈ 1.2–2 (see the table
  below), so long first sentences wait; streaming within a sentence is the next step.

### Heavy (GPU, audiobooks / dubbing) — not for a system voice, maybe "read a file" later
F5-TTS (Russian fine-tune "Misha"; short context), OmniVoice (k2-fsa; good cloning,
stress issues), Qwen3-TTS, CosyVoice3, Higgs v3, FireRedTTS, MOSS, VibeVoice.

- **ebooker** (romadinov.github.io/ebooker, github.com/romadinov/ebooker; added 2026-11-10)
  — EPUB → M4B audiobook pipeline (Python/uv, Apple Silicon or NVIDIA), not an engine but
  a good field report on Russian TTS. Every chunk is re-transcribed with Whisper and checked
  (numbers, content words, word count, silence/clipping), bad ones re-generated. Measured
  backends: Silero v5 (ru, RTF 0.008), Kokoro-82M (en), ESpeech-TTS-1 (ru, F5, cloning),
  Chatterbox, and **Shtorm-PocketTTS-RU** (CC-BY-4.0, 100M Pocket TTS fine-tune, RTF 0.19 on
  CPU, cloning, obeys acute stress marks; their pick for Russian narration) — a candidate for
  the light class above too. Front-end ideas worth borrowing: explicit stress with RUAccent
  (or silero-stress, which also restores ё), user-editable stress_overrides.txt and
  pronunciation.txt, Latin words in Russian text transliterated via CMUdict, Roman numerals
  → ordinals in the right case, all-caps abbreviations spelt out; measured pause lengths per
  punctuation mark (comma ≈ 0, "…" ≈ 0.45 s, ";" ≈ 0.55 s, "." ≈ 0.7 s).

## Runtimes worth building on
- **sherpa-onnx** (k2-fsa, Apache-2.0, C/C++ API, CPU) — one runtime for many TTS model
  families (VITS/Piper, Matcha, Kokoro, Kitten, …) and for STT (Vosk, GigaAM v3, Whisper,
  Parakeet). To check: exact list of TTS families in the current release, whether Vosk TTS
  and Supertonic/Pocket load there.
- **sherpa-onnx ≥ 2.0.0 (#4055) dropped espeak-ng / piper-phonemize** (GPL-free now).
  TTS front ends: built-in lexicon, or external phonemes passed through
  `SherpaOnnxGenerationConfig` (`phoneme_codepoints` / `tokens`; `data_dir` deprecated).
  → `sd_sherpa` phonemizes Piper/VITS/Kokoro input itself with the system libespeak-ng
  (GPL-3 module, which is fine for speech-dispatcher).
- Packaging: nobody ships sherpa-onnx for Debian/Ubuntu; resolute has
  libonnxruntime-dev 1.23.2, so only sherpa-onnx itself is missing (own PPA, 2.0.1+ds with
  vendored small deps: kaldi-native-fbank(+kissfft), kaldi-decoder, simple-sentencepiece,
  openfst fork, hclust-cpp, eigen 5.0.1, json, utf8proc).
- **Speed check 2026-11-10** (Ryzen 5 3500U, ru_RU-dmitri-medium, 6 s of speech, whole process
  incl. model load): piper 1.2 (Pied, bundled onnxruntime 1.14) 1.15 s; piper 1.8 + Ubuntu
  onnxruntime 1.23.2 2.35 s; piper 1.8 + upstream onnxruntime 1.23.2 2.1 s (Ubuntu's build only
  ~12 % slower). piper 1.8 hard-codes `SetIntraOpNumThreads(1)`; local patch (branch `threads`
  in _tmp/piper1/src, env PIPER_NUM_THREADS, default 0 = per core) → 1.85 s. The rest of the gap
  to 1.2 is still open (to check: ORT_ENABLE_BASIC vs ALL, disabled mem arena, load vs
  synthesis time, ort 1.14 vs 1.23). sherpa-onnx CLI also defaults to 1 thread: MMS ru RTF
  1.16 → 0.49 with 4 threads; Supertonic 3 ru RTF 0.70 (8 threads), Pocket en 0.97 (1 thread).
  **Decision (user, 2026-11-10): build against Ubuntu's libonnxruntime1.23, no upstream
  onnxruntime copy.** (Upstream could only live in a private dir with RPATH: same file name
  libonnxruntime.so.1.23.2, but SONAME .so.1 vs .so.1.23 and no VERS_1.23 symbol versions.)
- (2026-11-11) Interim speechd modules through sd_generic + sherpa-say (load per phrase,
  ~6–7 s per short phrase): `supertonic-generic` (31 languages × 10 speakers,
  supertonic-LANG-N) and `pocket-generic` (pocket-bria, pocket-loona) in the user's
  ~/.config/speech-dispatcher/modules (sources _tmp/sherpa/speechd). **User's verdict:
  Supertonic Russian is decent but ZONOS3 Russian is better; Supertonic English is good.** supertonic-ru-7 (sid 6): "very decent, on the ZONOS level" (preliminary).
- Russian STT for the later "Speech input" tab: GigaAM v3 (sherpa-onnx, RTF ~0.05–0.1 on
  Ryzen, INT8), Vosk, Whisper, Parakeet.

## Consequences for the settings app
1. speech-dispatcher stays the bus (Orca, Firefox, Chromium, xdg speech portal speak to it);
   the app configures it and does not replace it.
2. Its module list is outdated: modern engines come only through `sd_generic` shell
   pipelines (fragile: no streaming, no index marks, process per message, quoting bugs).
   Better: **one native module `sd_sherpa`** (C, libspeechd-module + sherpa-onnx C API) that
   loads ONNX voices of several families → Piper, Matcha/Vosk, Kokoro, Kitten from one
   place, low latency, interruptible. Possibly upstreamable to speech-dispatcher.
3. Voice catalog in the app: per engine an index (piper-voices voices.json; sherpa-onnx
   tts-models release; rhvoice.su voices), filter by language, sample playback, install to
   `~/.local/share/<app>/voices/`, then (re)generate module config + restart the daemon.
4. Russian stress: optional preprocessing (RuAccent / silero-stress) is a later option.

## STT / dictation landscape (for the later "Speech input" part; from the user, 2026-11-08)
- **IBus-Speech-To-Text** (github.com/Manish7093/IBus-Speech-To-Text) — dictation as an IBus
  input method, offline Vosk, GTK4/libadwaita settings. The closest thing to native
  dictation; works in any toolkit that speaks IBus → the natural integration point for us too.
- **voxtype** (github.com/peteonrails/voxtype) — push-to-talk dictation daemon for Linux.
- **Ubuntu: "Myna"** — Canonical's own effort (per the user; details still to look up).
- GNOME Shell extensions: Speech to Text (OpenRouter, cloud), gnome-speech2text and
  speech2text-extension (Whisper / whisper.cpp, local), OpenWispr (local companion daemon).
  Speed of Sound — standalone GTK app, Whisper.
- WebKitGTK: Web Speech API recognition locally through whisper.cpp + GStreamer.
- GNOME Discourse 2026 proposal "Desktop-wide offline speech-to-text / dictation support":
  D-Bus interfaces + an XDG portal, because Wayland forbids injecting keystrokes into
  other windows. Our STT design should follow that (input method / portal), not xdotool.

## Speed and resources on the user's laptop (2026-11-11)
Ryzen 5 3500U (4 cores / 8 threads), 5.7 GB RAM, load average 2–6 from other apps (Firefox,
Telegram) during the run. One phrase per language (ru ≈ 140 chars, en ≈ 160 chars),
best of 2 runs. Script _tmp/bench/bench.py, RHVoice through _tmp/bench/rhv/rhv-say (C API).
"Call" = one process as speechd's generic module runs it (start + model load + synthesis);
ZONOS3 = warm resident server (the load is paid once). RTF = time / audio length (< 1 is
faster than real time). CPU = CPU-seconds per second of speech.

| Engine | Lang | Audio, s | Call, s | RTF call | RTF synth only | CPU s / s speech | Peak RAM |
|---|---|---|---|---|---|---|---|
| RHVoice (Aleksandr / SLT) | ru | 11.2 | 0.75 | **0.07** | — | 0.06 | 27 MB |
| RHVoice | en | 9.1 | 0.56 | **0.06** | — | 0.06 | 19 MB |
| Piper ru_RU-dmitri-medium | ru | 7.7 | 4.9 | 0.64 | — | 1.9 | 231 MB |
| Piper en_US-ljspeech-high | en | 10.1 | 15.2 | 1.50 | — | 7.7 | 288 MB |
| MMS-TTS (sherpa) | ru | 9.1 | 9.5 | 1.05 | 0.82 | 5.9 | 242 MB |
| Supertonic 3 (sherpa, 8 steps) | ru | 10.8 | 15.0 | 1.40 | 1.25 | 8.3 | 307 MB |
| Supertonic 3 | en | 11.1 | 13.2 | 1.18 | 1.03 | 7.4 | 309 MB |
| Pocket TTS (sherpa) | en | 7.9 | 16.2 | 2.05 | 1.83 | 12.1 | 581 MB |
| ZONOS3-Mini, server, 4 decoder threads | ru | 8.7 | — | — | 1.50 | 5.3 | 1.93 GB |
| ZONOS3-Mini, 4 threads | en | 10.4 | — | — | 1.45 | 5.1 | 1.93 GB |
| ZONOS3-Mini, 8 threads | ru | 8.7 | — | — | 1.57 | 5.6 | 1.94 GB |
| ZONOS3-Mini, 8 threads | en | 10.4 | — | — | 1.51 | 5.4 | 1.94 GB |

ZONOS3 server load: 6 s (10 CPU-s) on a calmer system, 15 s under load.
Conclusions: on this CPU every neural engine is around real time or slower, so the first
sentence always waits; only RHVoice is instant. 4 decoder threads beat 8 (less CPU, a bit
faster) → the user's zonos3.conf now has `Zonos3Threads 4`; the module default should change to
4. sherpa-say uses nproc threads; Supertonic/Pocket probably also oversubscribe (to try: -t 4,
fewer Supertonic steps). The numbers depend on the background load.

### ZONOS3 on the Vega 8 through ggml Vulkan (2026-11-11, night)
Build: `-DGGML_VULKAN=ON` (docker image zonos-vk = zonos-build + libvulkan-dev glslc
spirv-headers; script _tmp/zonos3/vk/build-vk.sh; binaries
/home/grigory/Storage/Claude/zonos3-build/build-vk/). **Needed a fix:** ggml's Vulkan backend
reports the Vega 8 (uma) as `GGML_BACKEND_DEVICE_TYPE_IGPU`, and zonos3 picks only `..._GPU`,
so `--gpu` silently ran on the CPU. Patch _tmp/zonos3/vk/igpu.patch (zonos3.cpp, zac.cpp:
fall back to IGPU, treat IGPU as GPU) — worth sending upstream to Zyphra.
Same phrases and script as the table above, at a quieter moment (load 2–4):

| ZONOS3 variant | ru RTF | en RTF | CPU s / s speech | RAM (RSS) | VRAM |
|---|---|---|---|---|---|
| CPU, 4 decoder threads | 0.96 | 0.93 | 3.3 | 1.85 GB | — |
| Vulkan, LM on GPU, decoder on CPU (`--zac-cpu`) | 0.92 | 0.89 | 2.2 | 1.29 GB | |
| **Vulkan, all on GPU (`--gpu`)** | **0.55** | **0.52** | **0.27** | **0.32 GB** | ≈1.3 GB |

Server load 3.8 s. VRAM is the 2 GB carve-out reserved by the BIOS, which the CPU can't use anyway,
so the GPU variant also frees ~1.5 GB of system RAM. The first measurement under load
(previous table, RTF 1.5) shows how much the CPU variant suffers from other apps; the GPU one
barely uses the CPU. Output differs slightly (f16 on GPU: ru 8.8 s vs 8.67 s); the user
compared _tmp/bench/out/zonos-{cpu,vulkan}-ru.wav by ear: no audible difference.

**Done (2026-11-11):** zonos3-tts 2~local is built with Vulkan + OpenBLAS + igpu.patch
(build dir build-vkblas, Depends libvulkan1, Recommends mesa-vulkan-drivers). In sd_zonos3,
`Zonos3Gpu 1` is the default: the server starts with `--gpu`, and if it dies while
loading (no memory etc.), it is started again with `--cpu`. The mode is kept in the state
file, so only a server with the same setting is adopted. The default number of threads is now
min(CPUs, 4). zonos3-say tries `--gpu` and retries with `--cpu` (`ZONOS3_BACKEND=cpu` skips
the GPU). Protocol test on the GPU: cold first audio 8.7 s, adopted 2.1 s (CPU: 35.6 / 8.2 s).
A reaper bug was fixed along the way: the forked reaper inherited `server_child`, so
`waitpid` failed for it, it took the server for dead, removed the state and left the
server orphaned. The first live test then showed two more problems:
- `spd-say` without `-w` autospawns speechd, disconnects at once, and speechd quits 5 s later
  (`Timeout 5`) without waiting for a module that is still loading. The message is lost.
- The module swallowed QUIT while it waited for the server and was killed without releasing it.

Now the release-time reaper is replaced by a **watcher forked with the server**: every 5 s
it checks the state file, and when the owner is 0 or no longer a living sd_zonos3
process for Zonos3IdleTimeout seconds, it stops the server. QUIT during a wait ends
the message and the module. Tested: normal and adopted runs, QUIT and SIGKILL while
loading. In every case the server is gone after the timeout. Note for a PPA build: `GGML_NATIVE=ON` ties the binaries to this CPU.

### sherpa-onnx threads and Supertonic steps (2026-11-11, Ryzen 5 3500U, 4C/8T)

Same 12 s phrase, best of 2 (`_tmp/bench/sherpa/run.sh`):

| engine | steps | threads | RTF | wall | CPU s |
|---|---|---|---|---|---|
| Supertonic ru | 8 | 8 | 1.06 | 13.1 s | 77 |
| Supertonic ru | 8 | 4 | 0.92 | 11.3 s | 38 |
| Supertonic ru | 8 | 2 | 0.91 | 11.3 s | 21 |
| Supertonic ru | 5 | 4 | 0.59 | 7.8 s | 25 |
| Supertonic ru | 3 | 4 | 0.42 | 5.9 s | 19 |
| Pocket en | – | 8 | 1.63 | 14.8 s | 85 |
| Pocket en | – | 4 | 1.12 | 10.5 s | 44 |
| Pocket en | – | 2 | 1.15 | 11.3 s | 23 |

8 threads (SMT) is the worst; 4 is optimal, 2 nearly as fast at half the CPU.
User's verdict by ear: 5 steps good, 3 bad. sherpa-say (sherpa-onnx 2.0.1-3~local) now
defaults to 5 steps and min(nproc, 4) threads, `-n STEPS` overrides. Pocket stays slower
than real time even at 4 threads. Supertonic misplaces Russian stress ("к+афе"), see PLAN.

### Vosk TTS through plain ONNX Runtime (checked 2026-11-11)

vosk_tts (Python) is a thin layer over onnxruntime: the model directory has `model.onnx`
(VITS-based acoustic model + vocoder, 22050 Hz), `dictionary` (word, probability, phonemes
with stress; best entry per word kept), `config.json`, optional `bert/model.onnx` +
`bert/vocab.txt` (WordPiece). Pipeline: split text on punctuation/spaces → dictionary lookup,
`g2p.convert(word)` for unknown words → phoneme ids (`^`/`$`, blanks or the 5-field
"multistream" tuples via `phoneme_id_map`) → BERT per-word embeddings (1×768×N, zeros
without BERT) → one `run()` with inputs `input`, `input_lengths`, `scales`
[noise, 1/rate, duration noise], `sid`, `bert`, (`phone_duration_extra` for multistream_v3).
No numpy-only magic, so a C port (sd_vosk or inside sd_sherpa) needs: the dictionary
lookup, a port of g2p.py (Russian rules), a WordPiece tokenizer, the multistream mapping.
Not drop-in for sherpa-onnx (its VITS path has no bert/multistream inputs). The only
existing port is vosk-tts-rs (Rust, `ort`, downloads ONNX Runtime; abandoned).
Bonus: the dictionary carries stress, so known words ("кафе") are stressed right without
RUAccent.

### sd_vosk (2026-11-11)

Native speechd module, C + GLib + json-glib + ONNX Runtime C API (Ubuntu libonnxruntime
1.23), MIT; source `_tmp/sd-vosk` (vosk.c = port of vosk_tts, sd_vosk.c, vosk.conf,
build-deb.sh, proto.py, bench.c; Python counterpart `_tmp/vosk-ref/bench.py`). The port
matches vosk_tts on 5 reference texts (tokens, BERT rows, phoneme ids). Packages:
speech-dispatcher-vosk 0.1-1~local + vosk-tts-model-ru-0.9-multi 0.9-1~local (780 MB,
ships `dictionary.sorted` = best entry per word, mmapped, binary search). Voices
`vosk-ru-female-1..3`, `vosk-ru-male-1..2`; options VoskModelsDir, VoskVoice, VoskBert,
VoskPreload (default on, loads in a thread; STOP works while waiting), VoskThreads,
VoskFirstChunk. A warm-up sentence runs after the load. No AddModule needed: speechd
autodetects modules while speechd.conf has no AddModule line.

**Slow load and the fix.** In the sequential executor ORT's memory planner
(allocation_planner.cc `FindReusableTensor`, walks the whole free list for every output)
is quadratic on the 17 000-node acoustic graph: 37 s at optimization level 0, ~70 s at
ALL; onnxsim (12 900 nodes) 55 s, `.ort` format and pip ORT 1.31 no better (found with
gdb + Ubuntu debuginfod symbols). `ORT_PARALLEL` plans no buffer reuse → 4 s. It needs
inter-op threads > 1, otherwise ORT silently falls back to sequential
(inference_session.cc: no inter-op pool → SEQUENTIAL). BERT stays sequential + ALL.

**C vs Python (vosk_tts, pip ORT 1.31.1, docker), same 5 texts, 4 threads, Ryzen
4-core/8-thread, 5.8 GB RAM with 4 GB in swap and Firefox busy, so absolute RTF is noisy:**

| | load | memory (peak RSS) | synth RTF |
|---|---|---|---|
| sd_vosk / bench.c, BERT | 6–8 s | 1.28 GB | 1.13 / 1.42 (interleaved runs) |
| Python vosk_tts, BERT | 106–148 s | 1.4–1.8 GB | 1.12 / 1.44 (interleaved runs) |
| sd_vosk, no BERT | 3.8 s | 0.33–0.40 GB | ~0.7 (quieter moment) |
| C, all sequential (old) | 70–94 s | 1.27 GB | 0.77 |

Synthesis speed is the same (same ONNX graphs, the time is in the model); the gain is the
load: ~20× faster, plus no Python. Python with ORT's default thread count (all 8) was
slower than with 4. Without BERT the speech is flatter; switch: `VoskBert 0`.

### iGPU fix sent upstream (2026-11-11)

Zyphra/ZONOS3-Mini PR #2 (github.com/Zyphra/ZONOS3-Mini/pull/2, branch gtubolcev:igpu,
clone /home/grigory/Storage/Claude/zonos3-pr): shared `servers/ggml/src/backend-device.h`
(GPU → IGPU → CPU; is_gpu counts both) used by zonos3, zac, zac-encoder, ssl-encoder,
spk-encoder and the ssl-reference-state test. Built on main f9863b3 (needs cargo now:
docker image zonos-vk-blas-rust = zonos-vk-blas + Rust from rust:1-slim), `--gpu` →
Vulkan0 on the Vega 8. Once merged, drop igpu.patch from our zonos3-tts package.

### sd_sherpa (2026-11-11)

Native speechd module, C + GLib, MIT; source `_tmp/sd-sherpa` (sd_sherpa.c, sherpa.conf,
build.sh, build-deb.sh, proto.py = protocol test that writes WAVs to out/). The sherpa-onnx C
API runs inside the module, no server: Supertonic 3 loads in 0.8–1.3 s, so the restart on every
speechd SIGHUP costs about a second, unlike ZONOS (6–15 s). One model is loaded at a time.
Models are found in `/usr/share/sherpa-onnx/tts` and `~/.local/share/sherpa-onnx/tts`:
Supertonic (`supertonic-<lang>-<female|male>-<n>`; voice.bin = sorted style files F1–F5, M1–M5,
so sid 0–4 female, 5–9 male), Pocket (`pocket-en-<wav name>` from test_wavs/ and voices/), MMS
VITS on characters (`mms-ru`, text lowercased). Kitten/Kokoro/Piper are skipped (they need a
phonemizer: next step, libespeak-ng). 314 voices. `SherpaVoice "<lang>" "<voice>"` per language
(the settings app writes it), voice types MALEn/FEMALEn → n-th voice of that gender of the
first model of the language (Supertonic before MMS and Pocket). Rate → speed 2^(rate/100)
(Supertonic ×1.05, its normal speed). STOP is read in the sherpa progress callback and between
sentences. Ubuntu's onnxruntime + libonnx print hundreds of "Schema error: … already registered"
lines on every model load; stderr goes to /dev/null during the load unless Debug ≥ 2.
Measured (protocol test, busy laptop): "Кафе на углу открылось в девять утра." 2.8 s of speech in
1.3 s, first audio 2.2 s after SPEAK including the model load; "Привет!" 0.9 s.

