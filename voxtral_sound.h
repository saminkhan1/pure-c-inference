/*
 * voxtral_sound.h - Audio feedback via system sounds (macOS)
 *
 * Uses AudioServicesPlaySystemSound for zero-dependency audio feedback.
 * Call from any thread — AudioServices is thread-safe.
 */

#ifndef VOXTRAL_SOUND_H
#define VOXTRAL_SOUND_H

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>

/* System sound IDs: 1113 = short tick (start), 1114 = short pop (stop) */
static inline void vox_sound_start(void) {
    AudioServicesPlaySystemSound(1113);
}
static inline void vox_sound_stop(void) {
    AudioServicesPlaySystemSound(1114);
}

#else

static inline void vox_sound_start(void) {}
static inline void vox_sound_stop(void) {}

#endif
#endif /* VOXTRAL_SOUND_H */
