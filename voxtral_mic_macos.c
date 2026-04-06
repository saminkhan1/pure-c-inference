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

#define MIC_SAMPLE_RATE   16000
#define MIC_NUM_BUFFERS   3
#define MIC_BUF_SAMPLES   1600   /* 100ms per AudioQueue buffer */
#define RING_CAPACITY     160000 /* 10 seconds at 16kHz */

static AudioQueueRef            queue = NULL;
static AudioQueueBufferRef      buffers[MIC_NUM_BUFFERS];
static pthread_mutex_t          ring_mutex = PTHREAD_MUTEX_INITIALIZER;
static float                    ring[RING_CAPACITY];
static int                      ring_head;
static int                      ring_count;
static int                      running;

static void mic_callback(void *userdata,
                         AudioQueueRef inAQ,
                         AudioQueueBufferRef inBuffer,
                         const AudioTimeStamp *inStartTime,
                         UInt32 inNumberPacketDescriptions,
                         const AudioStreamPacketDescription *inPacketDescs) {
    (void)userdata; (void)inStartTime;
    (void)inNumberPacketDescriptions; (void)inPacketDescs;

    pthread_mutex_lock(&ring_mutex);

    if (!running) {
        pthread_mutex_unlock(&ring_mutex);
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

    OSStatus err = AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
    if (err != noErr) {
        fprintf(stderr, "mic: AudioQueueEnqueueBuffer failed: %d\n", (int)err);
    }

    pthread_mutex_unlock(&ring_mutex);
}

int vox_mic_start(void) {
    pthread_mutex_lock(&ring_mutex);

    if (running) {
        pthread_mutex_unlock(&ring_mutex);
        return 0;
    }

    if (queue != NULL) {
        ring_head = 0;
        ring_count = 0;
        pthread_mutex_unlock(&ring_mutex);

        OSStatus err = AudioQueueReset(queue);
        if (err != noErr) {
            AudioQueueDispose(queue, true);
            queue = NULL;
        }
        pthread_mutex_lock(&ring_mutex);
    }

    if (queue == NULL) {
        AudioStreamBasicDescription fmt = {0};
        fmt.mSampleRate       = MIC_SAMPLE_RATE;
        fmt.mFormatID         = kAudioFormatLinearPCM;
        fmt.mFormatFlags      = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        fmt.mBitsPerChannel   = 16;
        fmt.mChannelsPerFrame = 1;
        fmt.mBytesPerFrame    = 2;
        fmt.mFramesPerPacket  = 1;
        fmt.mBytesPerPacket   = 2;

        OSStatus err = AudioQueueNewInput(&fmt, mic_callback, NULL,
                                           NULL, NULL, 0, &queue);
        if (err != noErr) {
            pthread_mutex_unlock(&ring_mutex);
            fprintf(stderr, "mic: AudioQueueNewInput failed: %d\n", (int)err);
            return -1;
        }

        UInt32 buf_bytes = MIC_BUF_SAMPLES * sizeof(int16_t);
        for (int i = 0; i < MIC_NUM_BUFFERS; i++) {
            AudioQueueAllocateBuffer(queue, buf_bytes, &buffers[i]);
        }
    }

    ring_head = 0;
    ring_count = 0;
    running = 1;
    pthread_mutex_unlock(&ring_mutex);

    for (int i = 0; i < MIC_NUM_BUFFERS; i++) {
        AudioQueueEnqueueBuffer(queue, buffers[i], 0, NULL);
    }

    OSStatus err = AudioQueueStart(queue, NULL);
    if (err != noErr) {
        pthread_mutex_lock(&ring_mutex);
        running = 0;
        pthread_mutex_unlock(&ring_mutex);
        fprintf(stderr, "mic: AudioQueueStart failed: %d\n", (int)err);
        return -1;
    }

    return 0;
}

int vox_mic_read(float *out, int max_samples) {
    pthread_mutex_lock(&ring_mutex);
    int n = ring_count < max_samples ? ring_count : max_samples;
    if (n > 0) {
        int tail = (ring_head - ring_count + RING_CAPACITY) % RING_CAPACITY;
        for (int i = 0; i < n; i++) {
            out[i] = ring[(tail + i) % RING_CAPACITY];
        }
        ring_count -= n;
    }
    pthread_mutex_unlock(&ring_mutex);
    return n;
}

int vox_mic_read_available(void) {
    pthread_mutex_lock(&ring_mutex);
    int n = ring_count;
    pthread_mutex_unlock(&ring_mutex);
    return n;
}

void vox_mic_stop(void) {
    pthread_mutex_lock(&ring_mutex);
    if (!running) {
        pthread_mutex_unlock(&ring_mutex);
        return;
    }
    running = 0;
    pthread_mutex_unlock(&ring_mutex);

    AudioQueuePause(queue);
}

void vox_mic_cleanup(void) {
    pthread_mutex_lock(&ring_mutex);
    if (queue) {
        if (running) {
            running = 0;
            pthread_mutex_unlock(&ring_mutex);
            AudioQueueStop(queue, true);
            pthread_mutex_lock(&ring_mutex);
        }
        AudioQueueDispose(queue, true);
        queue = NULL;
    }
    pthread_mutex_unlock(&ring_mutex);
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