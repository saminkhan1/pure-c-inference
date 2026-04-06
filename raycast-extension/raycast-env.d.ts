/// <reference types="@raycast/api">

/* 🚧 🚧 🚧
 * This file is auto-generated from the extension's manifest.
 * Do not modify manually. Instead, update the `package.json` file.
 * 🚧 🚧 🚧 */

/* eslint-disable @typescript-eslint/ban-types */

type ExtensionPreferences = {
  /** Voxtral Binary Path - Full path to the voxtral binary */
  "binaryPath": string,
  /** Model Path - Path to the voxtral model directory */
  "modelPath": string,
  /** Auto-Paste - Automatically paste transcription to active app when done */
  "autoPaste": boolean
}

/** Preferences accessible in all the extension's commands */
declare type Preferences = ExtensionPreferences

declare namespace Preferences {
  /** Preferences accessible in the `dictate` command */
  export type Dictate = ExtensionPreferences & {}
}

declare namespace Arguments {
  /** Arguments passed to the `dictate` command */
  export type Dictate = {}
}

