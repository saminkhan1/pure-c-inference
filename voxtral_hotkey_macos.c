/*
 * voxtral_hotkey_macos.c - Global hotkey via CGEventTap (macOS)
 *
 * Listens for Command+R (toggle recording) and Escape (cancel recording)
 * using a session-level event tap. Runs its own CFRunLoop on a background
 * pthread. The user callback must be fast (just set a flag + signal condvar).
 */

#ifdef __APPLE__

#include "voxtral_hotkey.h"
#include <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <stdio.h>

#define KEYCODE_R       15
#define KEYCODE_ESCAPE 53

static vox_hotkey_cb_t  user_cb;
static CFMachPortRef    event_tap;
static CFRunLoopRef     tap_runloop;
static CFRunLoopSourceRef tap_source;
static pthread_t        tap_thread;
static int              hotkey_recording_active = 0;

/* Synchronization for startup handshake */
static pthread_mutex_t  tap_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   tap_init_cond  = PTHREAD_COND_INITIALIZER;
static int              tap_init_done  = 0; /* 0=pending, 1=success, -1=failure */

static CGEventRef tap_callback(CGEventTapProxy proxy, CGEventType type,
                               CGEventRef event, void *userdata) {
    (void)proxy; (void)userdata;

    /* Re-enable if macOS disabled the tap. */
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(event_tap, true);
        return event;
    }

    if (!event || type != kCGEventKeyDown)
        return event;

    CGKeyCode keycode = (CGKeyCode)CGEventGetIntegerValueField(
        event, kCGKeyboardEventKeycode);
    CGEventFlags flags = CGEventGetFlags(event);

    /* Command+R → toggle */
    if (keycode == KEYCODE_R &&
        (flags & kCGEventFlagMaskCommand)) {
        if (user_cb) user_cb(VOX_HOTKEY_TOGGLE);
        /* Dictation owns the start/stop hotkey; always swallow it so the
         * foreground app does not refresh or trigger its own shortcut. */
        return NULL;
    }

    /* Escape → cancel (only swallow when recording is active) */
    if (keycode == KEYCODE_ESCAPE &&
        __atomic_load_n(&hotkey_recording_active, __ATOMIC_SEQ_CST)) {
        if (user_cb) user_cb(VOX_HOTKEY_CANCEL);
        return NULL;
    }

    return event;
}

static void *hotkey_thread_func(void *arg) {
    (void)arg;

    CGEventMask mask = (1 << kCGEventKeyDown);

    event_tap = CGEventTapCreate(kCGHIDEventTap,
                                 kCGHeadInsertEventTap,
                                 kCGEventTapOptionDefault,
                                 mask, tap_callback, NULL);
    if (!event_tap) {
        fprintf(stderr,
            "Error: Cannot create event tap.\n"
            "Grant Input Monitoring permission:\n"
            "  System Settings → Privacy & Security → Input Monitoring\n"
            "  Enable the terminal or voxtral binary, then restart.\n");
        pthread_mutex_lock(&tap_init_mutex);
        tap_init_done = -1;
        pthread_cond_signal(&tap_init_cond);
        pthread_mutex_unlock(&tap_init_mutex);
        return NULL;
    }

    tap_source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault,
                                               event_tap, 0);
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    CFRunLoopAddSource(rl, tap_source, kCFRunLoopCommonModes);
    CGEventTapEnable(event_tap, true);

    /* Signal main thread that tap is ready (set tap_runloop under lock
     * so vox_hotkey_stop() never sees a stale NULL on ARM) */
    pthread_mutex_lock(&tap_init_mutex);
    tap_runloop = rl;
    tap_init_done = 1;
    pthread_cond_signal(&tap_init_cond);
    pthread_mutex_unlock(&tap_init_mutex);

    CFRunLoopRun();

    /* Cleanup after CFRunLoopStop */
    CGEventTapEnable(event_tap, false);
    CFRunLoopRemoveSource(tap_runloop, tap_source, kCFRunLoopCommonModes);
    CFRelease(tap_source);
    CFRelease(event_tap);
    tap_source = NULL;
    event_tap = NULL;
    tap_runloop = NULL;

    return NULL;
}

int vox_hotkey_start(vox_hotkey_cb_t cb) {
    user_cb = cb;
    if (pthread_create(&tap_thread, NULL, hotkey_thread_func, NULL) != 0) {
        fprintf(stderr, "Error: Failed to create hotkey thread\n");
        return -1;
    }

    /* Wait for the tap thread to finish initialization */
    pthread_mutex_lock(&tap_init_mutex);
    while (tap_init_done == 0)
        pthread_cond_wait(&tap_init_cond, &tap_init_mutex);
    int ok = tap_init_done;
    tap_init_done = 0; /* reset for potential re-start */
    pthread_mutex_unlock(&tap_init_mutex);

    if (ok < 0) {
        pthread_join(tap_thread, NULL);
        return -1;
    }

    return 0;
}

void vox_hotkey_stop(void) {
    pthread_mutex_lock(&tap_init_mutex);
    CFRunLoopRef rl = tap_runloop;
    pthread_mutex_unlock(&tap_init_mutex);
    if (rl)
        CFRunLoopStop(rl);
    pthread_join(tap_thread, NULL);
}

void vox_hotkey_set_recording(int active) {
    __atomic_store_n(&hotkey_recording_active, active, __ATOMIC_SEQ_CST);
}

#else /* !__APPLE__ */

#include "voxtral_hotkey.h"
#include <stdio.h>

int vox_hotkey_start(vox_hotkey_cb_t cb) {
    (void)cb;
    fprintf(stderr, "Error: Global hotkey is only supported on macOS\n");
    return -1;
}

void vox_hotkey_stop(void) {}

void vox_hotkey_set_recording(int active) { (void)active; }

#endif
