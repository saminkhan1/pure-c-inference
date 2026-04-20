#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_MODEL_DIR="${HOME}/voxtral-model"

echo "Voxtral beta installer"
echo
echo "Enter the model directory to use for dictation."
echo "Press Return to accept the default:"
echo "  ${DEFAULT_MODEL_DIR}"
echo
printf "Model directory: "
read -r MODEL_DIR

if [ -z "${MODEL_DIR}" ]; then
    MODEL_DIR="${DEFAULT_MODEL_DIR}"
fi

"${SCRIPT_DIR}/install_beta.sh" --model-dir "${MODEL_DIR}"

echo
echo "Voxtral is installed. Press Return to close this window."
read -r _
