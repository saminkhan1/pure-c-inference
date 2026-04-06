#!/usr/bin/env python3
"""
Autoresearch Evaluation Harness
Evaluates transcript quality (WER/CER) and latency (TTFT, RTF, finalization latency)
across multiple delays to create a composite scoreboard.
"""

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
from dataclasses import dataclass, asdict
import jiwer
import string

JSON_RE = re.compile(r"JSON_METRICS:\s+(.+)")
AUDIO_RE = re.compile(r"Audio:\s+\d+\s+samples\s+\(([0-9.]+)\s+seconds\)")
DECODER_RE = re.compile(
    r"Decoder:\s+\d+\s+text tokens\s+\((\d+)\s+steps\)\s+in\s+(\d+)\s+ms\s+"
    r"\(prefill\s+(\d+)\s+ms\s+\+\s+([0-9.]+)\s+ms/step\)"
)
GPU_RE = re.compile(r"Metal GPU:\s+([0-9.]+)\s+MB")

@dataclass
class EvalResult:
    audio_filepath: str
    dataset: str
    split: str
    delay_ms: int
    reference_text: str
    hypothesis_text: str
    wer: float
    cer: float
    ttft_ms: float
    ttf_ms: float
    endpoint_latency_ms: float
    audio_sec: float
    step_ms: float
    rtf: float
    gpu_mb: float
    wall_sec: float
    success: bool
    error: str = ""

def clean_text(text: str) -> str:
    """Standardizes text for scoring (uppercase, no punct)."""
    if not text:
        return ""
    text = text.upper()
    # Remove punctuation
    text = text.translate(str.maketrans('', '', string.punctuation))
    # Replace multiple spaces with a single space
    return " ".join(text.split())

def run_voxtral(binary: str, model_dir: str, wav_path: str, delay_ms: int) -> tuple[bool, str, dict, float, float, float, float, float, str]:
    cmd = [binary, "-d", model_dir, "-i", wav_path, "--json-metrics"]
    if delay_ms > 0:
        cmd.extend(["-I", str(delay_ms / 1000.0)])
        
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        wall_sec = time.time() - t0
        
        if proc.returncode != 0:
            return False, "", {}, 0.0, 0.0, 0.0, 0.0, wall_sec, f"Exit code {proc.returncode}: {proc.stderr[-200:]}"
            
        stdout_text = proc.stdout.strip()
        stderr_text = proc.stderr
        
        json_metrics = {}
        audio_sec = 0.0
        step_ms = 0.0
        gpu_mb = 0.0
        
        for line in stderr_text.splitlines():
            m = JSON_RE.search(line)
            if m:
                try:
                    json_metrics = json.loads(m.group(1))
                except json.JSONDecodeError:
                    pass
            am = AUDIO_RE.search(line)
            if am:
                audio_sec = float(am.group(1))
            dm = DECODER_RE.search(line)
            if dm:
                step_ms = float(dm.group(4))
            gm = GPU_RE.search(line)
            if gm:
                gpu_mb = float(gm.group(1))
                
        rtf = (wall_sec / audio_sec) if audio_sec > 0 else 0.0
                    
        return True, stdout_text, json_metrics, audio_sec, step_ms, rtf, gpu_mb, wall_sec, ""
        
    except subprocess.TimeoutExpired:
        return False, "", {}, 0.0, 0.0, 0.0, 0.0, time.time() - t0, "Timeout after 120s"
    except Exception as e:
        return False, "", {}, 0.0, 0.0, 0.0, 0.0, time.time() - t0, str(e)

def main():
    parser = argparse.ArgumentParser(description="Autoresearch Dictation & Realtime Performance Harness")
    parser.add_argument("--manifest", default="samples/benchmark/autoresearch/manifest.json", help="Path to JSON manifest")
    parser.add_argument("--binary", default="./voxtral", help="Path to voxtral binary")
    parser.add_argument("--model", default="voxtral-model", help="Model directory")
    parser.add_argument("--delays", default="240,480,960", help="Comma-separated delays in ms")
    parser.add_argument("--limit", type=int, default=0, help="Max samples to run per dataset split (0 for all)")
    parser.add_argument("--output", default="bench/eval_latest.json", help="Where to save full results")
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    if not manifest_path.exists():
        print(f"Error: Manifest {manifest_path} not found. Run prepare_datasets.py first.", file=sys.stderr)
        return 1
        
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)
        
    if args.limit > 0:
        # Group by dataset and limit
        grouped = {}
        for m in manifest:
            grouped.setdefault(m["dataset"], []).append(m)
        manifest = []
        for ds, items in grouped.items():
            manifest.extend(items[:args.limit])
            
    try:
        delays = [int(d.strip()) for d in args.delays.split(",") if d.strip()]
    except ValueError:
        print("Invalid delays format, expected comma-separated integers.", file=sys.stderr)
        return 1

    results = []
    total_runs = len(manifest) * len(delays)
    run_idx = 0
    
    print(f"Starting eval harness: {len(manifest)} files x {len(delays)} delays = {total_runs} total runs")
    
    for delay in delays:
        print(f"\n--- Testing Delay: {delay} ms ---")
        for item in manifest:
            run_idx += 1
            wav_path = item["audio_filepath"]
            ref_raw = item["reference_text"]
            ds_name = item["dataset"]
            
            print(f"[{run_idx}/{total_runs}] {ds_name} | {Path(wav_path).name}...", end="", flush=True)
            
            success, hyp_raw, metrics, audio_sec, step_ms, rtf, gpu_mb, wall_sec, err = run_voxtral(args.binary, args.model, wav_path, delay)
            
            if not success:
                print(f" FAILED ({err})")
                results.append(EvalResult(
                    audio_filepath=wav_path, dataset=ds_name, split=item["split"],
                    delay_ms=delay, reference_text=ref_raw, hypothesis_text="",
                    wer=1.0, cer=1.0, ttft_ms=0.0, ttf_ms=0.0, endpoint_latency_ms=0.0,
                    audio_sec=0.0, step_ms=0.0, rtf=0.0, gpu_mb=0.0, wall_sec=wall_sec,
                    success=False, error=err
                ))
                continue
                
            is_dictation = "dictation" in ds_name.lower()
            ref_clean = ref_raw.strip() if is_dictation else clean_text(ref_raw)
            hyp_clean = hyp_raw.strip() if is_dictation else clean_text(hyp_raw)
            
            wer_score = 1.0
            cer_score = 1.0
            if ref_clean:
                try:
                    wer_score = jiwer.wer(ref_clean, hyp_clean)
                    cer_score = jiwer.cer(ref_clean, hyp_clean)
                except ValueError:
                    pass # Keep 1.0 if empty hypothesis causes issues
            
            ttft_ms = metrics.get("time_to_first_token_ms", 0.0)
            ttf_ms = metrics.get("time_to_final_ms", 0.0)
            endpoint_latency_ms = max(0.0, ttf_ms - (audio_sec * 1000.0))
            
            print(f" OK | WER: {wer_score:.3f} | TTFT: {ttft_ms}ms | EP Latency: {endpoint_latency_ms:.1f}ms | RTF: {rtf:.3f}")
            
            results.append(EvalResult(
                audio_filepath=wav_path, dataset=ds_name, split=item["split"],
                delay_ms=delay, reference_text=ref_clean, hypothesis_text=hyp_clean,
                wer=wer_score, cer=cer_score, ttft_ms=ttft_ms, ttf_ms=ttf_ms, endpoint_latency_ms=endpoint_latency_ms,
                audio_sec=audio_sec, step_ms=step_ms, rtf=rtf, gpu_mb=gpu_mb,
                wall_sec=wall_sec, success=True
            ))

    # --- SCOREBOARD GENERATION ---
    print("\n\n=== SCOREBOARD ===")
    
    # Group by delay and dataset
    by_delay = {}
    for r in results:
        by_delay.setdefault(r.delay_ms, []).append(r)
        
    for delay, delay_results in sorted(by_delay.items()):
        print(f"\nDelay {delay}ms:")
        
        ds_groups = {}
        for r in delay_results:
            ds_groups.setdefault(r.dataset, []).append(r)
            
        for ds_name, ds_results in ds_groups.items():
            success_runs = [r for r in ds_results if r.success]
            fail_count = len(ds_results) - len(success_runs)
            
            if not success_runs:
                print(f"  {ds_name}: All {len(ds_results)} runs failed.")
                continue
                
            avg_wer = sum(r.wer for r in success_runs) / len(success_runs)
            avg_ttft = sum(r.ttft_ms for r in success_runs) / len(success_runs)
            avg_ttf = sum(r.ttf_ms for r in success_runs) / len(success_runs)
            avg_ep_latency = sum(r.endpoint_latency_ms for r in success_runs) / len(success_runs)
            
            total_audio = sum(r.audio_sec for r in success_runs)
            total_wall = sum(r.wall_sec for r in success_runs)
            overall_rtf = total_wall / total_audio if total_audio > 0 else 0.0
            avg_step_ms = sum(r.step_ms for r in success_runs) / len(success_runs)
            avg_gpu_mb = sum(r.gpu_mb for r in success_runs) / len(success_runs)
            
            print(f"  {ds_name}:")
            print(f"    WER:  {avg_wer:.2%}")
            print(f"    TTFT: {avg_ttft:.1f} ms")
            print(f"    TTF:  {avg_ttf:.1f} ms")
            print(f"    EP Latency: {avg_ep_latency:.1f} ms")
            print(f"    RTF:  {overall_rtf:.4f}x")
            print(f"    Step: {avg_step_ms:.2f} ms")
            print(f"    VRAM: {avg_gpu_mb:.1f} MB")
            if fail_count > 0:
                print(f"    Failures: {fail_count}")

    # Save to JSON
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump([asdict(r) for r in results], f, indent=2)
        
    print(f"\nFull results saved to {out_path}")
    return 0

if __name__ == "__main__":
    sys.exit(main())