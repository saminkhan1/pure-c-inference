import { useState, useEffect, useRef } from "react";
import { Action, ActionPanel, Form, Clipboard, showToast, Toast, getPreferenceValues, closeMainWindow } from "@raycast/api";
import { useForm } from "@raycast/utils";
import { transcribeAudio, startStreamingTranscription } from "../utils/voxtral";

interface Preferences {
  binaryPath: string;
  modelPath: string;
  autoPaste: boolean;
}

interface TranscriptionHandle {
  stop: () => void;
}

async function simulatePaste() {
  try {
    const script = `
      tell application "System Events"
        keystroke "v" using command down
      end tell
    `;
    const { exec } = await import("child_process");
    return new Promise<void>((resolve, reject) => {
      exec(`osascript -e '${script}'`, (error) => {
        if (error) {
          reject(error);
        } else {
          resolve();
        }
      });
    });
  } catch (e) {
    console.error("Failed to simulate paste:", e);
  }
}

export default function Command() {
  const [isTranscribing, setIsTranscribing] = useState(false);
  const [transcript, setTranscript] = useState("");
  const transcriptionRef = useRef<TranscriptionHandle | null>(null);
  const preferences = getPreferenceValues<Preferences>();

  const { handleSubmit, itemProps, setValue, values, reset } = useForm({
    onSubmit: handleCopyToClipboard,
    initialValues: {
      transcription: "",
    },
  });

  useEffect(() => {
    return () => {
      if (transcriptionRef.current) {
        transcriptionRef.current.stop();
      }
    };
  }, []);

  async function handleCopyToClipboard() {
    if (!values.transcription) return;
    await Clipboard.copy(values.transcription);
    await showToast({ style: Toast.Style.Success, title: "Copied to clipboard" });
  }

  async function startRecording() {
    if (isTranscribing) return;
    
    setIsTranscribing(true);
    reset({ transcription: "" });
    setTranscript("");
    
    try {
      await showToast({
        style: Toast.Style.Animated,
        title: "Listening...",
      });

      const stream = await startStreamingTranscription(preferences.binaryPath, {
        modelPath: preferences.modelPath,
        onToken: (text) => {
          setTranscript((prev) => prev + text);
          setValue("transcription", prev => prev + text);
        },
        onComplete: async (text) => {
          const finalText = text.trim();
          setValue("transcription", finalText);
          
          if (preferences.autoPaste && finalText) {
            await Clipboard.copy(finalText);
            await closeMainWindow();
            setTimeout(async () => {
              try {
                await simulatePaste();
              } catch (e) {
                console.error("Paste failed:", e);
              }
            }, 100);
            await showToast({
              style: Toast.Style.Success,
              title: "Transcription complete",
              message: "Text pasted to app",
            });
          } else {
            await showToast({
              style: Toast.Style.Success,
              title: "Transcription complete",
              message: finalText ? `${finalText.substring(0, 50)}...` : "No text captured",
            });
          }
        },
        onError: async (error) => {
          await showToast({
            style: Toast.Style.Failure,
            title: "Transcription error",
            message: error.message,
          });
        },
      });

      transcriptionRef.current = stream;
    } catch (error) {
      console.error("Transcription error:", error);
      await showFailureToast(error, { title: "Failed to start recording" });
      setIsTranscribing(false);
    }
  }

  async function stopRecording() {
    if (!isTranscribing || !transcriptionRef.current) return;
    
    transcriptionRef.current.stop();
    transcriptionRef.current = null;
    setIsTranscribing(false);
  }

  async function handleToggleRecording() {
    if (isTranscribing) {
      await stopRecording();
    } else {
      await startRecording();
    }
  }

  return (
    <Form
      isLoading={isTranscribing}
      actions={
        <ActionPanel>
          <Action
            title={isTranscribing ? "Stop Recording" : "Start Recording"}
            onAction={handleToggleRecording}
            shortcut={{ modifiers: ["cmd"], key: "r" }}
          />
          {!isTranscribing && values.transcription && (
            <Action.SubmitForm
              title="Copy to Clipboard"
              onSubmit={handleSubmit}
              shortcut={{ modifiers: ["cmd"], key: "c" }}
            />
          )}
          {isTranscribing && (
            <Action
              title="Clear"
              onAction={() => {
                reset({ transcription: "" });
                setTranscript("");
              }}
            />
          )}
        </ActionPanel>
      }
    >
      <Form.TextArea
        {...itemProps.transcription}
        title="Voxtral Dictation"
        placeholder={isTranscribing ? "Listening... speak now" : "Press ⌘+R to start recording"}
        enableMarkdown={false}
        autoFocus
      />
      <Form.Description
        title="Status"
        text={isTranscribing ? "🎙️ Recording... Press ⌘+R to stop" : "Ready. Press ⌘+R to start."}
      />
    </Form>
  );
}