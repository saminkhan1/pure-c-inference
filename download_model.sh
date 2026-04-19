#!/bin/bash
# Download Voxtral Realtime 4B model from HuggingFace
#
# Usage: ./download_model.sh [--dir DIR]
#   --dir DIR   Download to DIR (default: voxtral-model)

set -e

MODEL_ID="mistralai/Voxtral-Mini-4B-Realtime-2602"
MODEL_DIR="voxtral-model"
TOTAL_SIZE_GB=8.9

while [[ $# -gt 0 ]]; do
    case $1 in
        --dir) MODEL_DIR="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo "Downloading Voxtral Realtime 4B to ${MODEL_DIR}/"
echo "Model: ${MODEL_ID}"
echo "Size: ~${TOTAL_SIZE_GB}GB (may take 5-15 minutes depending on connection)"
echo ""

mkdir -p "${MODEL_DIR}"

# Files to download
FILES=(
    "consolidated.safetensors"
    "params.json"
    "tekken.json"
)

BASE_URL="https://huggingface.co/${MODEL_ID}/resolve/main"

for file in "${FILES[@]}"; do
    dest="${MODEL_DIR}/${file}"
    if [ -f "${dest}" ]; then
        echo "  [skip] ${file} (already exists)"
    else
        echo "  [download] ${file}..."
        # -L follows redirects, --progress-bar shows progress, -C - resumes partial downloads
        curl -L -o "${dest}" "${BASE_URL}/${file}" --progress-bar -C - || {
            echo "  [error] Failed to download ${file}. Check your connection and try again."
            rm -f "${dest}"
            exit 1
        }
        echo "  [done] ${file}"
    fi
done

echo ""
echo "Download complete. Model files in ${MODEL_DIR}/"
ls -lh "${MODEL_DIR}/"
