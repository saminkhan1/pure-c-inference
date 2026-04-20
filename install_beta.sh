#!/bin/bash

set -euo pipefail

APP_NAME="Voxtral.app"
PLIST_NAME="com.voxtral.agent"

usage() {
    cat <<'EOF'
Usage:
  ./install_beta.sh --model-dir /path/to/voxtral-model [options]

Options:
  --app-source /path/to/Voxtral.app
  --plist-template /path/to/com.voxtral.agent.plist
  --install-dir /Applications
EOF
}

escape_sed_replacement() {
    printf '%s' "$1" | sed -e 's/[&|]/\\&/g'
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_SOURCE="${SCRIPT_DIR}/${APP_NAME}"
PLIST_TEMPLATE="${SCRIPT_DIR}/${PLIST_NAME}.plist"
INSTALL_DIR=""
MODEL_DIR=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --model-dir)
            [ "$#" -ge 2 ] || { usage; exit 1; }
            MODEL_DIR="$2"
            shift 2
            ;;
        --app-source)
            [ "$#" -ge 2 ] || { usage; exit 1; }
            APP_SOURCE="$2"
            shift 2
            ;;
        --plist-template)
            [ "$#" -ge 2 ] || { usage; exit 1; }
            PLIST_TEMPLATE="$2"
            shift 2
            ;;
        --install-dir)
            [ "$#" -ge 2 ] || { usage; exit 1; }
            INSTALL_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

[ -n "$MODEL_DIR" ] || {
    echo "Error: --model-dir is required" >&2
    usage
    exit 1
}

[ -d "$MODEL_DIR" ] || {
    echo "Error: model directory not found: $MODEL_DIR" >&2
    exit 1
}

[ -f "${MODEL_DIR}/consolidated.safetensors" ] || {
    echo "Error: missing ${MODEL_DIR}/consolidated.safetensors" >&2
    exit 1
}

[ -f "${MODEL_DIR}/tekken.json" ] || {
    echo "Error: missing ${MODEL_DIR}/tekken.json" >&2
    exit 1
}

[ -d "$APP_SOURCE" ] || {
    echo "Error: app bundle not found: $APP_SOURCE" >&2
    exit 1
}

[ -f "$PLIST_TEMPLATE" ] || {
    echo "Error: launch agent template not found: $PLIST_TEMPLATE" >&2
    exit 1
}

if [ -z "$INSTALL_DIR" ]; then
    if [ -w "/Applications" ]; then
        INSTALL_DIR="/Applications"
    else
        INSTALL_DIR="${HOME}/Applications"
    fi
fi

APP_DEST="${INSTALL_DIR}/${APP_NAME}"
PLIST_DEST="${HOME}/Library/LaunchAgents/${PLIST_NAME}.plist"
CONFIG_DIR="${HOME}/.config/voxtral"
UID_LABEL="gui/$(id -u)/${PLIST_NAME}"

mkdir -p "$INSTALL_DIR" "${HOME}/Library/LaunchAgents" "$CONFIG_DIR"

launchctl bootout "$UID_LABEL" 2>/dev/null || true
rm -rf "$APP_DEST"
cp -R "$APP_SOURCE" "$INSTALL_DIR/"
xattr -rd com.apple.quarantine "$APP_DEST" 2>/dev/null || true

VOXTRAL_BIN_ESCAPED="$(escape_sed_replacement "${APP_DEST}/Contents/MacOS/voxtral")"
MODEL_DIR_ESCAPED="$(escape_sed_replacement "$MODEL_DIR")"
HOME_ESCAPED="$(escape_sed_replacement "$HOME")"

sed -e "s|__VOXTRAL_BIN__|${VOXTRAL_BIN_ESCAPED}|g" \
    -e "s|__MODEL_DIR__|${MODEL_DIR_ESCAPED}|g" \
    -e "s|__HOME__|${HOME_ESCAPED}|g" \
    "$PLIST_TEMPLATE" > "$PLIST_DEST"

plutil -lint "$PLIST_DEST" >/dev/null
launchctl bootstrap "gui/$(id -u)" "$PLIST_DEST"

echo
echo "Installed Voxtral to ${APP_DEST}"
echo "Model directory: ${MODEL_DIR}"
echo "LaunchAgent: ${PLIST_DEST}"
echo "Logs: ${CONFIG_DIR}/voxtral.log"
