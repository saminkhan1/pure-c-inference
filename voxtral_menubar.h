/*
 * voxtral_menubar.h - Menu bar status item API (macOS)
 *
 * Runs NSApplication on the main thread and spawns the provided
 * worker function on a GCD background queue.
 */

#ifndef VOXTRAL_MENUBAR_H
#define VOXTRAL_MENUBAR_H

typedef void *(*vox_worker_fn)(void *user_data);

int  vox_menubar_run(vox_worker_fn worker_fn, void *user_data);

void vox_menubar_set_recording(int active);
void vox_menubar_set_processing(void);
void vox_menubar_set_status(const char *msg);
void vox_menubar_quit(void);
int  vox_menubar_wait_for_quit(void);

/* Show a warning icon + message when a permission is denied.
 * Call from the worker thread; dispatches to main thread internally. */
void vox_menubar_set_error(const char *msg);

/* Update the "Copy Last Transcription" menu item after each dictation.
 * Pass NULL to disable the item. */
void vox_menubar_set_last_text(const char *text);

#endif /* VOXTRAL_MENUBAR_H */
