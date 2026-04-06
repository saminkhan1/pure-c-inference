#!/bin/bash
set -e

DIR="samples/benchmark/librispeech"
mkdir -p "$DIR"

if [ ! -f "$DIR/test-clean.tar.gz" ]; then
    echo "Downloading LibriSpeech test-clean (346 MB)..."
    curl -L -o "$DIR/test-clean.tar.gz" https://www.openslr.org/resources/12/test-clean.tar.gz
else
    echo "Archive already downloaded."
fi

if [ ! -d "$DIR/LibriSpeech/test-clean" ]; then
    echo "Extracting archive..."
    tar -xzf "$DIR/test-clean.tar.gz" -C "$DIR"
else
    echo "Archive already extracted."
fi

echo "Checking FLAC to 16kHz WAV conversion..."
# Find all flac files, convert them to wav if the wav doesn't exist
find "$DIR/LibriSpeech/test-clean" -name "*.flac" | while read flac_file; do
    wav_file="${flac_file%.flac}.wav"
    if [ ! -f "$wav_file" ]; then
        ffmpeg -loglevel error -y -i "$flac_file" -ar 16000 -ac 1 "$wav_file" </dev/null
    fi
done

echo "Creating file lists..."
find "$DIR/LibriSpeech/test-clean" -name "*.wav" > "$DIR/all_files.txt"

# Create a small representative subset (mini_files.txt) for quick testing
# Pick 20 files randomly (using shuf or awk)
if command -v shuf >/dev/null 2>&1; then
    shuf -n 20 "$DIR/all_files.txt" > "$DIR/mini_files.txt"
else
    awk 'BEGIN{srand()} {print rand()"\t"$0}' "$DIR/all_files.txt" | sort -n | cut -f2- | head -n 20 > "$DIR/mini_files.txt"
fi

echo ""
echo "Dataset preparation complete!"
echo "-----------------------------------"
echo "Total WAV files: $(wc -l < "$DIR/all_files.txt")"
echo "Mini subset created: $DIR/mini_files.txt"
echo ""
echo "To run the benchmark on the mini subset:"
echo "  ./benchmark.py --samples-root . --files \$(cat $DIR/mini_files.txt)"
echo ""
echo "To run on the full test-clean set:"
echo "  ./benchmark.py --samples-root . --files \$(cat $DIR/all_files.txt)"
