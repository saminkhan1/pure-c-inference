import { spawn } from "child_process";
import { access, constants } from "fs/promises";
import { showToast, Toast } from "@raycast/api";

interface TranscribeOptions {
  modelPath?: string;
  timeout?: number;
}

interface TranscribeResult {
  text: string;
}

interface StreamOptions {
  modelPath?: string;
  onToken?: (text: string) => void;
  onComplete?: (text: string) => void;
  onError?: (error: Error) => void;
}

export async function transcribeAudio(
  binaryPath: string,
  options: TranscribeOptions = {}
): Promise<TranscribeResult> {
  const modelPath = options.modelPath || "voxtral-model";
  const timeout = options.timeout || 30000;

  if (!binaryPath) {
    throw new Error("Binary path is required");
  }

  const exists = await access(binaryPath, constants.F_OK)
    .then(() => true)
    .catch(() => false);
  if (!exists) {
    const error = new Error(`Voxtral binary not found at: ${binaryPath}`);
    await showToast({
      style: Toast.Style.Failure,
      title: "Binary not found",
      message: "Please check the binary path in preferences",
    });
    throw error;
  }

  return new Promise((resolve, reject) => {
    const args = ["-d", modelPath, "--from-mic", "--silent"];

    const proc = spawn(binaryPath, args, {
      stdio: ["ignore", "pipe", "pipe"],
    });

    let stdout = "";
    let timedOut = false;

    const timeoutId = setTimeout(() => {
      timedOut = true;
      proc.kill();
      reject(new Error("Transcription timed out"));
    }, timeout);

    proc.stdout.on("data", (data) => {
      stdout += data.toString();
    });

    proc.on("close", (code) => {
      clearTimeout(timeoutId);
      if (timedOut) return;
      if (code === 0 || code === null) {
        resolve({ text: stdout.trim() });
      } else {
        reject(new Error(`Transcription failed with code ${code}`));
      }
    });

    proc.on("error", (err) => {
      clearTimeout(timeoutId);
      reject(new Error(`Failed to run voxtral: ${err.message}`));
    });
  });
}

export async function startStreamingTranscription(
  binaryPath: string,
  options: StreamOptions = {}
): Promise<{ stop: () => void }> {
  const modelPath = options.modelPath || "voxtral-model";

  if (!binaryPath) {
    throw new Error("Binary path is required");
  }

  const exists = await access(binaryPath, constants.F_OK)
    .then(() => true)
    .catch(() => false);
  if (!exists) {
    throw new Error(`Voxtral binary not found at: ${binaryPath}`);
  }

  const args = ["-d", modelPath, "--from-mic", "--silent"];

  const proc = spawn(binaryPath, args, {
    stdio: ["ignore", "pipe", "pipe"],
  });

  let fullText = "";
  let hasError = false;

  proc.stdout.on("data", (data) => {
    const text = data.toString();
    fullText += text;
    if (options.onToken) {
      options.onToken(text);
    }
  });

  proc.on("close", (code) => {
    if (!hasError && options.onComplete) {
      options.onComplete(fullText);
    }
  });

  proc.on("error", (err) => {
    hasError = true;
    if (options.onError) {
      options.onError(new Error(`Failed to run voxtral: ${err.message}`));
    }
  });

  return {
    stop: () => {
      proc.kill();
      if (options.onComplete) {
        options.onComplete(fullText);
      }
    },
  };
}
