/*
 * voxtral_hotkey.h - Global hotkey detection API
 *
 * macOS implementation uses CGEventTap (CoreGraphics).
 * Other platforms: stubs that return errors.
 */

#ifndef VOXTRAL_HOTKEY_H
#define VOXTRAL_HOTKEY_H

typedef enum {
    VOX_HOTKEY_TOGGLE,   /* Command+R: start/stop recording */
    VOX_HOTKEY_CANCEL    /* Escape: cancel current recording */
} vox_hotkey_event_t;

typedef void (*vox_hotkey_cb_t)(vox_hotkey_event_t event);

/* Start listening for global hotkeys on a background thread.
 * Callback is invoked from the hotkey thread — must be fast/non-blocking.
 * Returns 0 on success, -1 on error (e.g. Input Monitoring not granted). */
int vox_hotkey_start(vox_hotkey_cb_t cb);

/* Stop listening and clean up. */
void vox_hotkey_stop(void);

/* Inform the hotkey layer whether recording is active.
 * When not active, Escape is passed through to other apps. */
void vox_hotkey_set_recording(int active);

#endif /* VOXTRAL_HOTKEY_H */
