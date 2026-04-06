/*
 * voxtral_mic_macos.c - Microphone capture using AudioQueue Services (macOS)
 *
 * AudioQueue callback runs on its own thread, converts s16le to float,
 * and pushes to a ring buffer protected by a pthread mutex.
 * The main thread polls vox_mic_read() to drain samples.
 *
 * To avoid AudioQueue hangs on stop/start cycles in multithreaded Cocoa apps,
 * we create the queue once and reuse it across vox_mic_start/stop calls.
 */

#ifdef __APPLE__

#include "voxtral_mic.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>

#define MIC_SAMPLE_RATE   16000
#define MIC_NUM_BUFFERS   3
#define MIC_BUF_SAMPLES   1600   /* 100ms per AudioQueue buffer */
#define RING_CAPACITY     160000 /* 10 seconds at 16kHz */

static AudioQueueRef            queue = NULL;
static AudioQueueBufferRef      buffers[MIC_NUM_BUFFERS];
static pthread_mutex_t          ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static float                    ring[RING_CAPACITY];
static int                      ring_head;  /* next write position */
static int                      ring_count; /* samples in ring */
static int                      running;

/* Lock tracking for debugging */
static _Atomic pthread_t        mutex_owner;
static _Atomic int              mutex_locked = 0;
static _Atomic int              mutex_waiters = 0;

static double get_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void debug_lock(const char *ctx) {
    pthread_t self = pthread_self();
    double start = get_now_ms();
    
    if (mutex_locked) {
        if (pthread_equal(mutex_owner, self)) {
            fprintf(stderr, "mic: [DEBUG] %s - thread %p DEADLOCK: already holds lock!\n", ctx, self);
            fflush(stderr);
        } else {
            mutex_waiters++;
            fprintf(stderr, "mic: [DEBUG] %s - thread %p waiting for lock held by %p (waiters: %d)\n", 
                    ctx, self, mutex_owner, (int)mutex_waiters);
            fflush(stderr);
        }
    }
    
    pthread_mutex_lock(&ring_mutex);
    
    if (mutex_locked && !pthread_equal(mutex_owner, self)) {
        mutex_waiters--;
    }
    
    mutex_owner = self;
    mutex_locked = 1;
    double elapsed = get_now_ms() - start;
    if (elapsed > 1.0) {
        fprintf(stderr, "mic: [DEBUG] %s - thread %p acquired lock after %.2f ms\n", ctx, self, elapsed);
    } else {
        fprintf(stderr, "mic: [DEBUG] %s - thread %p acquired lock\n", ctx, self);
    }
    fflush(stderr);
}

static void debug_unlock(const char *ctx) {
    pthread_t self = pthread_self();
    if (!mutex_locked || !pthread_equal(mutex_owner, self)) {
        fprintf(stderr, "mic: [DEBUG] %s - thread %p ERROR: releasing lock it doesn't hold!\n", ctx, self);
    }
    fprintf(stderr, "mic: [DEBUG] %s - thread %p releasing lock\n", ctx, self);
    mutex_locked = 0;
    pthread_mutex_unlock(&ring_mutex);
    fflush(stderr);
}

static int debug_trylock(const char *ctx) {
    pthread_t self = pthread_self();
    int err = pthread_mutex_trylock(&ring_mutex);
    if (err == 0) {
        mutex_owner = self;
        mutex_locked = 1;
        fprintf(stderr, "mic: [DEBUG] %s - thread %p acquired lock (try)\n", ctx, self);
    } else {
        fprintf(stderr, "mic: [DEBUG] %s - thread %p trylock failed (%d), held by %p, waiters: %d\n", 
                ctx, self, err, mutex_owner, (int)mutex_waiters);
    }
    fflush(stderr);
    return err;
}

/* AudioQueue input callback — runs on AudioQueue's own thread */
static void mic_callback(void *userdata,
                          AudioQueueRef inAQ,
                          AudioQueueBufferRef inBuffer,
                          const AudioTimeStamp *inStartTime,
                          UInt32 inNumberPacketDescriptions,
                          const AudioStreamPacketDescription *inPacketDescs) {
    (void)userdata; (void)inStartTime;
    (void)inNumberPacketDescriptions; (void)inPacketDescs;

    fprintf(stderr, "mic: callback AT START (thread %p, queue %p)\n", pthread_self(), inAQ);
    fflush(stderr);

    if (debug_trylock("callback") != 0) {
        debug_lock("callback (wait)");
    }

    if (!running) {
        fprintf(stderr, "mic: callback ignored (running=0, queue %p, thread %p)\n", inAQ, pthread_self());
        debug_unlock("callback (ignored)");
        return;
    }

    int16_t *raw = (int16_t *)inBuffer->mAudioData;
    int n = (int)(inBuffer->mAudioDataByteSize / sizeof(int16_t));

    for (int i = 0; i < n; i++) {
        ring[ring_head] = raw[i] / 32768.0f;
        ring_head = (ring_head + 1) % RING_CAPACITY;
        if (ring_count < RING_CAPACITY)
            ring_count++;
    }

    /* Re-enqueue buffer for next capture. Holding ring_mutex ensures that
     * vox_mic_stop() can't set running=0 and call AudioQueueStop between our
     * check and the enqueue. */
    fprintf(stderr, "mic: callback enqueuing buffer %p (thread %p)...\n", inBuffer, pthread_self());
    fflush(stderr);
    OSStatus err = AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    if (err != noErr) {
        fprintf(stderr, "mic: callback AudioQueueEnqueueBuffer returned %d\n", (int)err);
        fflush(stderr);
    }
    
    debug_unlock("callback (finished)");
}

int vox_mic_start(void) {
    fprintf(stderr, "mic: vox_mic_start entering (thread %p)...\n", pthread_self());
    debug_lock("vox_mic_start");
    
    if (running) {
        debug_unlock("vox_mic_start (already running)");
        fprintf(stderr, "mic: vox_mic_start already running\n");
        return 0;
    }

    /* Reuse existing queue if available (paused state from previous recording) */
    if (queue != NULL) {
        fprintf(stderr, "mic: reusing existing queue %p\n", queue);
        
        /* Clear ring buffer for fresh recording while we hold the lock */
        ring_head = 0;
        ring_count = 0;
        
        /* Release lock before AudioQueueReset which can trigger callbacks */
        debug_unlock("vox_mic_start (pre-Reset)");
        
        /* Reset the queue to clear any stale state from previous recording */
        fprintf(stderr, "mic: AudioQueueReset (queue %p)...\n", queue);
        fflush(stderr);
        OSStatus err = AudioQueueReset(queue);
        if (err != noErr) {
            fprintf(stderr, "mic: AudioQueueReset failed: %d, will recreate queue\n", (int)err);
            /* Need to re-acquire lock to modify queue */
            debug_lock("vox_mic_start (post-Reset failed)");
            AudioQueueDispose(queue, true);
            queue = NULL;
            debug_unlock("vox_mic_start (disposed)");
        } else {
            fprintf(stderr, "mic: AudioQueueReset succeeded\n");
            /* Re-acquire lock before continuing */
            debug_lock("vox_mic_start (post-Reset)");
        }
    }
    
    /* Create new queue if needed (first time or reset failed) */
    if (queue == NULL) {
        /* First-time initialization: create new queue */
        AudioStreamBasicDescription fmt = {0};
        fmt.mSampleRate       = MIC_SAMPLE_RATE;
        fmt.mFormatID         = kAudioFormatLinearPCM;
        fmt.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBitsPerChannel   = 16;
        fmt.mChannelsPerFrame = 1;
        fmt.mBytesPerFrame    = 2;
        fmt.mFramesPerPacket  = 1;
        fmt.mBytesPerPacket   = 2;

        fprintf(stderr, "mic: AudioQueueNewInput (thread %p)...\n", pthread_self());
        fflush(stderr);
        /* NULL runloop/mode means callbacks run on a system-managed thread. */
        OSStatus err = AudioQueueNewInput(&fmt, mic_callback, NULL,
                                           NULL, NULL, 0, &queue);
        if (err != noErr) {
            debug_unlock("vox_mic_start (failed NewInput)");
            fprintf(stderr, "mic: AudioQueueNewInput failed: %d\n", (int)err);
            return -1;
        }
        fprintf(stderr, "mic: created queue %p\n", queue);

        /* Allocate buffers */
        UInt32 buf_bytes = MIC_BUF_SAMPLES * sizeof(int16_t);
        for (int i = 0; i < MIC_NUM_BUFFERS; i++) {
            AudioQueueAllocateBuffer(queue, buf_bytes, &buffers[i]);
            fprintf(stderr, "mic: allocated buffer %d (%p)\n", i, buffers[i]);
        }
    }
    
    /* Clear ring buffer for fresh recording */
    ring_head = 0;
    ring_count = 0;
    running = 1;
    
    /* RELEASING LOCK before potentially triggering callbacks via Enqueue/Start. */
    debug_unlock("vox_mic_start (releasing lock for setup)");

    for (int i = 0; i < MIC_NUM_BUFFERS; i++) {
        fprintf(stderr, "mic: enqueuing buffer %d (%p, thread %p)...\n", i, buffers[i], pthread_self());
        AudioQueueEnqueueBuffer(queue, buffers[i], 0, NULL);
    }

    fprintf(stderr, "mic: AudioQueueStart (queue %p, thread %p)...\n", queue, pthread_self());
    fflush(stderr);
    double t0 = get_now_ms();
    OSStatus err = AudioQueueStart(queue, NULL);
    fprintf(stderr, "mic: AudioQueueStart returned %d (thread %p, took %.2f ms)\n", 
            (int)err, pthread_self(), get_now_ms() - t0);
    fflush(stderr);

    if (err != noErr) {
        debug_lock("vox_mic_start (failed Start recovery)");
        running = 0;
        debug_unlock("vox_mic_start (failed Start)");
        fprintf(stderr, "mic: AudioQueueStart failed: %d\n", (int)err);
        return -1;
    }
    
    return 0;
}

int vox_mic_read(float *out, int max_samples) {
    debug_lock("vox_mic_read");
    int n = ring_count < max_samples ? ring_count : max_samples;
    if (n > 0) {
        int tail = (ring_head - ring_count + RING_CAPACITY) % RING_CAPACITY;
        for (int i = 0; i < n; i++) {
            out[i] = ring[(tail + i) % RING_CAPACITY];
        }
        ring_count -= n;
    }
    debug_unlock("vox_mic_read");
    return n;
}

int vox_mic_read_available(void) {
    debug_lock("vox_mic_read_available");
    int n = ring_count;
    debug_unlock("vox_mic_read_available");
    return n;
}

void vox_mic_stop(void) {
    debug_lock("vox_mic_stop");
    if (!running) {
        debug_unlock("vox_mic_stop (not running)");
        return;
    }
    running = 0;
    debug_unlock("vox_mic_stop (pre-Pause)");

    /* Use Pause to keep queue alive for reuse */
    fprintf(stderr, "mic: AudioQueuePause (queue %p, thread %p)...\n", queue, pthread_self());
    fflush(stderr);
    OSStatus err = AudioQueuePause(queue);
    if (err != noErr) {
        fprintf(stderr, "mic: AudioQueuePause failed: %d\n", (int)err);
    }
    fprintf(stderr, "mic: recording paused (queue %p kept alive for reuse)\n", queue);
}

void vox_mic_cleanup(void) {
    debug_lock("vox_mic_cleanup");
    if (queue) {
        if (running) {
            running = 0;
            debug_unlock("vox_mic_cleanup (pre-Stop)");
            AudioQueueStop(queue, true);
            debug_lock("vox_mic_cleanup (post-Stop)");
        }
        fprintf(stderr, "mic: AudioQueueDispose (queue %p, thread %p)\n", queue, pthread_self());
        fflush(stderr);
        AudioQueueDispose(queue, true);
        queue = NULL;
    }
    debug_unlock("vox_mic_cleanup");
}



#else /* !__APPLE__ */

#include "voxtral_mic.h"
#include <stdio.h>

int vox_mic_start(void) {
    fprintf(stderr, "Microphone capture is not supported on this platform\n");
    return -1;
}

int vox_mic_read(float *out, int max_samples) {
    (void)out; (void)max_samples;
    return 0;
}

int vox_mic_read_available(void) { return 0; }
void vox_mic_stop(void) {}
void vox_mic_cleanup(void) {}

#endif
