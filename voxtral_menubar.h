/*
 * voxtral_menubar.h - Menu bar status item API (macOS)
 *
 * Runs NSApplication on the main thread and spawns the provided
 * worker function on a background pthread.
 */

#ifndef VOXTRAL_MENUBAR_H
#define VOXTRAL_MENUBAR_H

/* Function signature for the background worker (wexproflow main loop).
 * Receives the same user_data pointer passed to vox_menubar_run. */
typedef void *(*vox_worker_fn)(void *user_data);

/* Start the menu bar app. Must be called from the main thread.
 * Spawns worker_fn on a background pthread, then enters NSApplication
 * run loop (never returns until the app quits).
 * Returns the worker thread's exit code (0 on success). */
int vox_menubar_run(vox_worker_fn worker_fn, void *user_data);

/* Update menu bar icon state. Thread-safe (dispatches to main queue).
 * active=1: recording (filled icon), active=0: idle (outline icon). */
void vox_menubar_set_recording(int active);

/* Request app termination from any thread. Thread-safe. */
void vox_menubar_quit(void);

#endif /* VOXTRAL_MENUBAR_H */
