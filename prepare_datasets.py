#!/usr/bin/env python3
"""
Prepares autoresearch evaluation datasets.
Uses HuggingFace Datasets streaming (no full downloads).
Saves all audio as 16kHz mono WAV.

Datasets:
1. LibriSpeech clean  — 50 clips  (read English, clean studio)
2. LibriSpeech other  — 50 clips  (read English, noisier speakers)
3. dictation_micro    — handcrafted (case+punctuation sensitive, realistic dictation)

Design rules:
- Fixed seeds for reproducibility across runs.
- Save audio duration to manifest so harness can bucket short vs long clips.
- dictation_micro is scored with case+punctuation intact (is_dictation=True in harness).
"""

import json
import os
import shutil
import subprocess
import sys
import warnings
import random
import librosa
import soundfile as sf
from datasets import load_dataset

warnings.filterwarnings("ignore")

SEED = 42


# ── dictation_micro ──────────────────────────────────────────────────────────
# Realistic Mac dictation sentences with natural casing and punctuation.
# These are scored case/punctuation-sensitive to detect regressions in
# Voxtral's capitalization and punctuation output.
DICTATION_MICRO = [
    "Dear John, I hope this email finds you well.",
    "Please schedule a meeting for Thursday at 3 PM.",
    "The quarterly revenue was up 12% compared to last year.",
    "Could you send me the report by end of day?",
    "I'll be working from home on Friday.",
    "The new iPhone comes out in September.",
    "Let's grab coffee at Blue Bottle on Market Street.",
    "My flight lands at SFO at 6:45 AM.",
    "The temperature outside is around 68 degrees Fahrenheit.",
    "She asked, \"Are you coming to the party tonight?\"",
    "The project deadline has been moved to April 15th.",
    "Please add milk, eggs, and bread to the shopping list.",
    "I need to call Dr. Smith's office before noon.",
    "The API rate limit is 1,000 requests per minute.",
    "Set a reminder for 9 AM tomorrow morning.",
    "The package should arrive within 3-5 business days.",
    "Open a new tab and search for the best Italian restaurants nearby.",
    "Turn off the lights in the living room.",
    "Reply to Sarah's message and say I'll be 10 minutes late.",
    "Create a new document called Q1 Sales Review.",
]


def write_silence_wav(path: str, duration_sec: float, sample_rate: int = 16000) -> None:
    """Write a short silent WAV for dictation_micro (TTS not available, use silence as placeholder)."""
    import numpy as np
    samples = int(duration_sec * sample_rate)
    sf.write(path, [0.0] * samples, sample_rate)


def make_dictation_micro(root_dir: str) -> list[dict]:
    """
    Create the dictation_micro dataset.
    We synthesize audio using macOS `say` command if available, otherwise write silence.
    The harness uses this to test case+punctuation-sensitive WER.
    """
    out_dir = os.path.join(root_dir, "dictation_micro")
    os.makedirs(out_dir, exist_ok=True)

    manifest = []
    has_say = _check_say_available()

    for i, text in enumerate(DICTATION_MICRO):
        wav_path = os.path.join(out_dir, f"dictation_{i:04d}.wav")

        if not os.path.exists(wav_path):
            if has_say:
                # macOS text-to-speech → 16kHz mono WAV
                tmp = wav_path.replace(".wav", "_raw.aiff")
                subprocess.run(["say", "-o", tmp, text],
                               capture_output=True, check=False)
                if os.path.exists(tmp):
                    audio, sr = librosa.load(tmp, sr=16000, mono=True)
                    sf.write(wav_path, audio, 16000)
                    os.remove(tmp)
                else:
                    write_silence_wav(wav_path, 3.0)
            else:
                write_silence_wav(wav_path, 3.0)

        # Measure actual duration
        try:
            dur = librosa.get_duration(path=wav_path)
        except Exception:
            dur = 0.0

        manifest.append({
            "audio_filepath": wav_path,
            "reference_text": text,          # case+punct preserved
            "normalized_text": text,          # same — harness decides normalization by dataset name
            "duration": dur,
            "dataset": "dictation_micro",
            "split": "test",
        })

    print(f"  -> dictation_micro: {len(manifest)} sentences "
          f"({'macOS say' if has_say else 'silence placeholder — run on Mac for real audio'})")
    return manifest


def _check_say_available() -> bool:
    try:
        return shutil.which("say") is not None
    except Exception:
        return False


def export_split(dataset_name, config_name, split, n_samples, output_dir,
                 text_column, audio_column, seed=SEED) -> list[dict]:
    os.makedirs(output_dir, exist_ok=True)
    manifest = []

    print(f"Fetching {n_samples} samples from {dataset_name}/{config_name} ({split})...")
    try:
        ds = load_dataset(dataset_name, config_name, split=split, streaming=True)
    except Exception as e:
        print(f"  Error loading {dataset_name}: {e}")
        return []

    # Deterministic shuffle: collect a larger pool, shuffle with fixed seed, take n_samples.
    # Streaming doesn't support random access, so we buffer 3× and sample.
    pool = []
    for item in ds:
        ref_text = item.get(text_column, "")
        if not ref_text or not isinstance(ref_text, str):
            continue
        pool.append(item)
        if len(pool) >= n_samples * 3:
            break

    rng = random.Random(seed)
    rng.shuffle(pool)
    selected = pool[:n_samples]

    for count, item in enumerate(selected):
        audio_data = item[audio_column]
        ref_text   = item[text_column].strip()
        sample_rate  = audio_data["sampling_rate"]
        audio_array  = audio_data["array"]

        if sample_rate != 16000:
            audio_array = librosa.resample(y=audio_array, orig_sr=sample_rate, target_sr=16000)
            sample_rate = 16000

        # Use config_name as part of filename key to avoid collisions
        safe_config = config_name.replace("/", "_")
        file_name = f"{dataset_name.split('/')[-1]}_{safe_config}_{count:04d}.wav"
        file_path = os.path.join(output_dir, file_name)
        sf.write(file_path, audio_array, sample_rate)

        duration = len(audio_array) / sample_rate

        manifest.append({
            "audio_filepath": file_path,
            "reference_text": ref_text,
            "normalized_text": ref_text.upper(),
            "duration": round(duration, 3),
            "dataset": dataset_name,
            "split": split,
        })

    print(f"  -> Saved {len(manifest)} samples to {output_dir}")
    return manifest


def main():
    root_dir = "samples/benchmark/autoresearch"
    os.makedirs(root_dir, exist_ok=True)

    all_manifests = []

    # 1. LibriSpeech clean (read English, studio quality)
    all_manifests.extend(export_split(
        dataset_name="librispeech_asr",
        config_name="clean",
        split="test",
        n_samples=50,
        output_dir=os.path.join(root_dir, "librispeech"),
        text_column="text",
        audio_column="audio",
    ))

    # 2. LibriSpeech other (more challenging speakers)
    all_manifests.extend(export_split(
        dataset_name="librispeech_asr",
        config_name="other",
        split="test",
        n_samples=50,
        output_dir=os.path.join(root_dir, "librispeech_other"),
        text_column="text",
        audio_column="audio",
    ))

    # 3. dictation_micro (case+punct sensitive, Mac TTS or silence)
    all_manifests.extend(make_dictation_micro(root_dir))

    # Write global manifest
    manifest_path = os.path.join(root_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(all_manifests, f, indent=2)

    total_duration = sum(m["duration"] for m in all_manifests)
    by_dataset: dict[str, int] = {}
    for m in all_manifests:
        by_dataset[m["dataset"]] = by_dataset.get(m["dataset"], 0) + 1

    print(f"\nAutoresearch dataset ready:")
    print(f"  Total files:    {len(all_manifests)}")
    print(f"  Total duration: {total_duration:.0f}s ({total_duration/60:.1f} min)")
    for ds, cnt in by_dataset.items():
        print(f"  {ds}: {cnt} files")
    print(f"  Manifest:       {manifest_path}")


if __name__ == "__main__":
    main()
