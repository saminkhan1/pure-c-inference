/*
 * test_mic_cycle.c - bounded start/stop regression for the macOS mic backend
 *
 * Exits 77 when the test cannot run because microphone access is unavailable
 * or the first AudioQueue start never resolves (for example due to TCC).
 */

#include "voxtral_mic.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define TEST_MIC_CYCLES 3
#define TEST_TIMEOUT_SEC 5

struct mic_test_state {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int started_once;
    int result;
    int cycle;
};

static void mic_test_mark(struct mic_test_state *state,
                          int started_once,
                          int done,
                          int result,
                          int cycle) {
    pthread_mutex_lock(&state->mutex);
    if (started_once)
        state->started_once = 1;
    state->cycle = cycle;
    if (done) {
        state->done = 1;
        state->result = result;
    }
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->mutex);
}

static void *mic_test_worker(void *arg) {
    struct mic_test_state *state = (struct mic_test_state *)arg;
    int started_once = 0;

    for (int cycle = 0; cycle < TEST_MIC_CYCLES; cycle++) {
        mic_test_mark(state, started_once, 0, 0, cycle + 1);

        if (vox_mic_start() != 0) {
            vox_mic_cleanup();
            if (!started_once) {
                fprintf(stderr, "SKIP: microphone access is unavailable\n");
                mic_test_mark(state, 0, 1, 77, cycle + 1);
            } else {
                fprintf(stderr, "FAIL: vox_mic_start failed on cycle %d\n", cycle + 1);
                mic_test_mark(state, 1, 1, 1, cycle + 1);
            }
            return NULL;
        }

        started_once = 1;
        mic_test_mark(state, 1, 0, 0, cycle + 1);

        usleep(200000);

        float buf[1600];
        (void)vox_mic_read_available();
        (void)vox_mic_read(buf, 1600);

        vox_mic_stop();
        usleep(50000);
    }

    vox_mic_cleanup();
    mic_test_mark(state, 1, 1, 0, TEST_MIC_CYCLES);
    return NULL;
}

static void deadline_from_now(struct timespec *ts, int seconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

int main(void) {
#ifdef __APPLE__
    pthread_t worker;
    struct mic_test_state state = {
        PTHREAD_MUTEX_INITIALIZER,
        PTHREAD_COND_INITIALIZER,
        0,
        0,
        0,
        0
    };

    if (pthread_create(&worker, NULL, mic_test_worker, &state) != 0) {
        fprintf(stderr, "FAIL: unable to start mic test worker\n");
        return 1;
    }

    pthread_mutex_lock(&state.mutex);
    while (!state.done) {
        struct timespec deadline;
        int rc;

        deadline_from_now(&deadline, TEST_TIMEOUT_SEC);
        rc = pthread_cond_timedwait(&state.cond, &state.mutex, &deadline);
        if (state.done)
            break;
        if (rc == ETIMEDOUT) {
            int started_once = state.started_once;
            int cycle = state.cycle;
            pthread_mutex_unlock(&state.mutex);
            pthread_detach(worker);
            if (!started_once) {
                fprintf(stderr,
                        "SKIP: microphone access did not become available within %d seconds\n",
                        TEST_TIMEOUT_SEC);
                return 77;
            }
            fprintf(stderr, "FAIL: microphone cycle timed out on cycle %d\n", cycle);
            return 1;
        }
    }

    {
        int result = state.result;
        pthread_mutex_unlock(&state.mutex);
        pthread_join(worker, NULL);
        return result;
    }
#else
    fprintf(stderr, "SKIP: macOS only\n");
    return 77;
#endif
}
