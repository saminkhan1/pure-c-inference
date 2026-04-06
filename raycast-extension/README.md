# Voxtral Dictation

Free, local, privacy-preserving speech-to-text dictation for macOS using Apple Silicon.

## Features

- **Local Processing**: Transcribes audio locally using Voxtral model - no cloud API, no internet required
- **Privacy First**: Your audio never leaves your machine
- **Free**: No subscription, no API costs
- **Fast**: Apple Silicon optimized for real-time transcription
- **Real-time Preview**: See transcription as you speak
- **Auto-paste**: Text automatically pasted to your active application
- **Silence Detection**: Automatically stops after 2 seconds of silence

## Requirements

- macOS device with Apple Silicon (M1/M2/M3/M4)
- [Raycast](https://raycast.com/) installed
- [Voxtral](https://github.com/saminkhan1/voxtral.c) binary built with microphone support

## Installation

### From Raycast Store (Coming Soon)

Once published, search "Voxtral Dictation" in Raycast Store and install.

### For Development

```bash
cd raycast-extension
npm install
npm run dev
```

This will load the extension in Raycast. Make sure Raycast is running first.

## Setup

### 1. Build Voxtral

```bash
# Clone and build voxtral
git clone https://github.com/saminkhan1/voxtral.c
cd voxtral.c
make mps

# The binary will be at ./voxtral
```

### 2. Configure Extension

1. Open Raycast and search for "Voxtral Dictation"
2. Set the **Voxtral Binary Path** (e.g., `/usr/local/bin/voxtral` or full path to your build)
3. Set the **Model Path** (e.g., `voxtral-model`)
4. Enable **Auto-Paste** to automatically paste text to your active app

## Usage

1. Open Raycast and search "Dictate"
2. Press **⌘+R** to start recording
3. Speak your text — see real-time transcription
4. Wait for silence (~2 seconds) or press **⌘+R** again to stop
5. Text is automatically pasted to your active application

### Keyboard Shortcuts

- **⌘+R** - Start/Stop Recording
- **⌘+C** - Copy to Clipboard (when not recording)
- **⌘+K** - Clear transcription

## Permissions

- **Microphone**: Required for recording audio (system prompt)
- **Accessibility**: Required for auto-paste (System Settings > Privacy & Security > Accessibility)

## Troubleshooting

- **Transcription fails**: Verify binary path in preferences
- **No audio**: Ensure microphone permissions granted in System Settings > Privacy & Security > Microphone
- **Auto-paste not working**: Enable Accessibility permission in System Settings > Privacy & Security > Accessibility

## License

MIT
