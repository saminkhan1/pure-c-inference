#!/usr/bin/env python3
"""
Downloads and prepares industry-standard ASR datasets for autoresearch.
Uses HuggingFace Datasets streaming to fetch small, diverse, representative slices
without downloading 100s of GBs. Resamples all audio to 16kHz WAV.

Datasets included:
1. LibriSpeech (clean, read English)
2. VoxPopuli (English subset - European Parliament, oratorical)
3. Common Voice (English - diverse accents and recording qualities)
"""

import os
import json
import sys
import warnings
import librosa
import soundfile as sf
from datasets import load_dataset

warnings.filterwarnings("ignore")

def export_split(dataset_name, config_name, split, n_samples, output_dir, text_column, audio_column):
    os.makedirs(output_dir, exist_ok=True)
    manifest = []
    
    print(f"Fetching {n_samples} samples from {dataset_name} ({config_name}) - {split}...")
    try:
        # Use streaming to avoid massive downloads
        ds = load_dataset(dataset_name, config_name, split=split, streaming=True)
    except Exception as e:
        print(f"Error loading {dataset_name}: {e}")
        return []
    
    count = 0
    for item in ds:
        if count >= n_samples:
            break
            
        audio_data = item[audio_column]
        ref_text = item[text_column]
        
        # Skip if reference text is empty or missing
        if not ref_text or not isinstance(ref_text, str):
            continue
            
        sample_rate = audio_data['sampling_rate']
        audio_array = audio_data['array']
        
        # Resample to 16kHz if needed, since voxtral uses 16000
        if sample_rate != 16000:
            audio_array = librosa.resample(y=audio_array, orig_sr=sample_rate, target_sr=16000)
            sample_rate = 16000
            
        file_name = f"{dataset_name.split('/')[-1]}_{config_name}_{count:04d}.wav"
        file_path = os.path.join(output_dir, file_name)
        
        sf.write(file_path, audio_array, sample_rate)
        
        # Standardize text for scoring (uppercase, no punct for basic WER, though we might want punct later)
        # For now, just save raw and uppercase
        manifest.append({
            "audio_filepath": file_path,
            "reference_text": ref_text.strip(),
            "normalized_text": ref_text.strip().upper(),
            "duration": len(audio_array) / sample_rate,
            "dataset": dataset_name,
            "split": split
        })
        count += 1
        
    print(f"  -> Saved {count} samples to {output_dir}")
    return manifest

def main():
    root_dir = "samples/benchmark/autoresearch"
    os.makedirs(root_dir, exist_ok=True)
    
    all_manifests = []
    
    # 1. LibriSpeech (clean read English)
    ls_manifest = export_split(
        dataset_name="librispeech_asr", 
        config_name="clean", 
        split="test", 
        n_samples=50, 
        output_dir=os.path.join(root_dir, "librispeech"),
        text_column="text",
        audio_column="audio"
    )
    all_manifests.extend(ls_manifest)
    
    # 2. LibriSpeech (other, more challenging English)
    vp_manifest = export_split(
        dataset_name="librispeech_asr", 
        config_name="other", 
        split="test", 
        n_samples=50, 
        output_dir=os.path.join(root_dir, "librispeech_other"),
        text_column="text", 
        audio_column="audio"
    )
    all_manifests.extend(vp_manifest)
    
    # Write global manifest
    manifest_path = os.path.join(root_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(all_manifests, f, indent=2)
        
    print(f"\nSuccessfully created autoresearch dataset!")
    print(f"Total files: {len(all_manifests)}")
    print(f"Manifest saved to: {manifest_path}")

if __name__ == "__main__":
    main()
