# Voxtral Realtime 4B in Pure C

System-wide on-device dictation for Apple Silicon, implemented in pure C.

This repo takes [Mistral AI's Voxtral Realtime 4B model](https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602) and turns it into a real Mac product surface: Metal-accelerated inference, streaming decode, microphone capture, global hotkeys, a menu bar app, and text injection into the focused application. The goal is not just to run the model. The goal is to make realtime dictation feel native on Apple hardware.

The main guided beta path is:

```bash
make wexproflow
./voxtral -d voxtral-model --dictate
```

## Why This Is Technically Interesting

This is not just an offline transcription demo:

- Apple Silicon-first inference path with Metal/MPS and a real dictation loop
- same core engine powers offline files, stdin streaming, microphone input, and dictation
- incremental streaming encoder/decoder state with bounded-memory behavior on long audio
- no Python runtime, CUDA toolkit, model server, or vLLM stack required at inference time
- `Voxtral.app` is built from the same source tree as the CLI, not a separate wrapper

High-level pipeline:

```text
mic / wav / stdin
  -> mel spectrogram
  -> streaming encoder
  -> adapter
  -> decoder
  -> tokens
  -> paste into focused macOS app
```

Verified on an Apple M4 Max for this guided beta setup:

- `make wexproflow`
- `./voxtral --help`
- `./voxtral -d voxtral-model -i test_speech.wav --silent`
- `cat test_speech.wav | ./voxtral -d voxtral-model --stdin --silent`
- `make test`
- `python3 benchmark.py`
- `plutil -lint Info.plist com.voxtral.agent.plist Voxtral.app/Contents/Info.plist`
- `codesign --verify --deep --strict --verbose=2 Voxtral.app`

![demo](samples/demo.gif)

## Apple Silicon Quickstart

Prerequisite: Xcode Command Line Tools.

```bash
xcode-select --install
./download_model.sh
make wexproflow
./voxtral -d voxtral-model --dictate
```

`make wexproflow` builds the Apple Silicon dictation target, creates `Voxtral.app`, and ad-hoc signs it locally. If you start dictation from `./voxtral`, the binary relaunches itself through `Voxtral.app` automatically so the menu bar app and global hotkey work correctly.

For beta sharing via DMG, the artifact now includes a self-contained `Install Voxtral.command` helper that copies the app, strips quarantine, writes the launch agent, and starts dictation mode without requiring a source checkout.

## Dictation Flow

Dictation mode is the main show:

1. Run `./voxtral -d voxtral-model --dictate`
2. Grant the requested macOS permissions
3. Press `Command+R` anywhere to start or stop recording
4. Pause speaking and the app auto-stops on silence
5. The transcribed text is pasted into the focused app

`Escape` cancels mid-recording. Dictation history is appended to `~/.config/voxtral/history.log`. Single-instance locking uses `~/.config/voxtral/voxtral.pid`.

Required macOS permissions:

| Permission | Why |
| --- | --- |
| Accessibility | text injection into the focused app |
| Input Monitoring | global `Command+R` and `Escape` hotkeys |

If either permission is missing, the app stays alive in the menu bar and shows the exact System Settings path to fix it.

## Performance Snapshot

Fresh `python3 benchmark.py` run on an Apple M4 Max. These numbers matter because the product target is realtime dictation, not batch-only ASR:

| Metric | Value |
| --- | --- |
| Runs | 18 |
| Audio processed | 658.2 s |
| Avg TTFT | 239.64 ms |
| Weighted decode step | 21.27 ms |
| Avg time to final | 17479.04 ms |
| Overall RTF | 0.6135 |

This is the benchmark summary from the repo in its current guided-beta state, not a hand-picked one-off trace. See [SPEED.md](SPEED.md) for deeper performance history and notes.

## Guided Beta Smoke-Tested

These commands were rerun on an Apple M4 Max before publishing this guided beta configuration:

```bash
make wexproflow
./voxtral --help
./voxtral -d voxtral-model -i test_speech.wav --silent
cat test_speech.wav | ./voxtral -d voxtral-model --stdin --silent
make test
python3 benchmark.py
plutil -lint Info.plist com.voxtral.agent.plist Voxtral.app/Contents/Info.plist
codesign --verify --deep --strict --verbose=2 Voxtral.app
```

## More Ways To Run It

Offline file transcription:

```bash
./voxtral -d voxtral-model -i test_speech.wav --silent
```

Stdin input (WAV auto-detected, or raw `s16le` 16 kHz mono):

```bash
cat test_speech.wav | ./voxtral -d voxtral-model --stdin --silent
```

Live microphone input:

```bash
./voxtral -d voxtral-model --from-mic
```

Useful flags:

- `-I <secs>` controls encoder batching. The CLI default is `0.5`. Lower is more responsive, higher is more efficient. For offline `-i` runs it matters much less because all audio is already available.
- `--json-metrics` prints structured timing data
- `--monitor` prints inline pipeline status symbols to stderr
- `--alt <cutoff>` shows alternative tokens when the model is uncertain

## Build Targets

The repo is Apple-first, but there are still secondary build paths:

```bash
make wexproflow    # Apple Silicon dictation build, creates ad-hoc Voxtral.app
make mps           # Apple Silicon Metal transcription build
make blas          # CPU fallback via Accelerate/OpenBLAS
make test          # regression suite
make inspect       # safetensors inspector
make clean
```

The current guided beta is centered on `make wexproflow` and `--dictate`. `make blas`, `benchmark.py`, `eval_harness.py`, `autoresearch.sh`, and the Python reference implementation are kept in the repo, but they are not the headline product story.

## Architecture Notes

- Chunked encoder with overlap handling and incremental KV cache
- Rolling compaction keeps encoder and decoder memory bounded on long audio
- Streaming C API in [voxtral.h](voxtral.h) built around `vox_stream_t`
- BF16 weights are memory-mapped from safetensors for fast startup
- Dictation mode layers a menu bar app, global hotkey capture, microphone input, and text injection on top of the same inference core

For deeper technical details:

- [MODEL.md](MODEL.md): model structure, tensor layout, decode schedule
- [SPEED.md](SPEED.md): benchmark history and optimization notes
- [AUTORESEARCH.md](AUTORESEARCH.md): research/eval framing

## Guided Beta Packaging

`Voxtral.app` is ad-hoc signed and not notarized. This repo ships a guided beta install flow, not a Gatekeeper-clean consumer app.

If you run `spctl -a -vv Voxtral.app` on a freshly built or downloaded bundle, rejection is expected. The supported path is the bundled installer, which copies the app into place and removes quarantine.

For beta users receiving the DMG, the supported path is:

1. Open the DMG
2. Run `Install Voxtral.command`
3. Enter the model directory when prompted

Source-based installs can still use `make install-beta MODEL_DIR=/path/to/voxtral-model`.

## Model And License

- Code in this repo: MIT
- Model weights: Apache-2.0 via Hugging Face
- Model download: `./download_model.sh`
