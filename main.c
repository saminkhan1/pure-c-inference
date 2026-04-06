/*
 * main.c - CLI entry point for voxtral.c
 *
 * Usage: voxtral -d <model_dir> -i <input.wav> [options]
 */

#include "voxtral.h"
#include "voxtral_kernels.h"
#include "voxtral_audio.h"
#include "voxtral_mic.h"
#include "voxtral_config.h"
#ifdef WEXPROFLOW
#include "voxtral_hotkey.h"
#include "voxtral_paste.h"
#include "voxtral_menubar.h"
#include "voxtral_sound.h"
#endif
#ifdef USE_METAL
#include "voxtral_metal.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#define DEFAULT_FEED_CHUNK 16000 /* 1 second at 16kHz */

/* SIGINT/SIGTERM handler for clean exit */
volatile sig_atomic_t mic_interrupted = 0;
static void sigint_handler(int sig) { (void)sig; mic_interrupted = 1; }

/* ---- wexproflow state (visible to voxtral_menubar.m) ---- */
#ifdef WEXPROFLOW
volatile int wf_event = 0; /* 0=none, 1=toggle, 2=cancel */
pthread_mutex_t wf_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  wf_cond  = PTHREAD_COND_INITIALIZER;

static void wexproflow_hotkey_cb(vox_hotkey_event_t event) {
    pthread_mutex_lock(&wf_mutex);
    wf_event = (event == VOX_HOTKEY_TOGGLE) ? 1 : 2;
    pthread_cond_signal(&wf_cond);
    pthread_mutex_unlock(&wf_mutex);
}
#endif /* WEXPROFLOW */

/* ---- PID file for single-instance ---- */
static int pid_fd = -1;
static char pid_path[512];

static int pid_lock_acquire(void) {
    const char *home = getenv("HOME");
    if (!home) return 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/voxtral", home);
    mkdir(dir, 0755);
    snprintf(pid_path, sizeof(pid_path), "%s/.config/voxtral/voxtral.pid", home);
    pid_fd = open(pid_path, O_CREAT | O_RDWR, 0644);
    if (pid_fd < 0) return 0;
    if (flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "voxtral is already running (pid file: %s)\n", pid_path);
        close(pid_fd);
        pid_fd = -1;
        return -1;
    }
    ftruncate(pid_fd, 0);
    dprintf(pid_fd, "%d\n", getpid());
    return 0;
}

static void pid_lock_release(void) {
    if (pid_fd >= 0) {
        unlink(pid_path);
        flock(pid_fd, LOCK_UN);
        close(pid_fd);
        pid_fd = -1;
    }
}

/* ---- History log ---- */
static void history_append(const char *text) {
    if (!text || !text[0]) return;
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/voxtral/history.log", home);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    dprintf(fd, "[%s] %s\n", ts, text);
    close(fd);
}

static void usage(const char *prog) {
    fprintf(stderr, "voxtral.c — Voxtral Realtime 4B speech-to-text\n\n");
    fprintf(stderr, "Usage: %s -d <model_dir> (-i <input.wav> | --stdin | --from-mic) [options]\n\n", prog);
    fprintf(stderr, "Required:\n");
    fprintf(stderr, "  -d <dir>      Model directory (with consolidated.safetensors, tekken.json)\n");
    fprintf(stderr, "  -i <file>     Input WAV file (16-bit PCM, any sample rate)\n");
    fprintf(stderr, "  --stdin       Read audio from stdin (auto-detect WAV or raw s16le 16kHz mono)\n");
    fprintf(stderr, "  --from-mic    Capture from default microphone (macOS only, Ctrl+C to stop)\n");
    fprintf(stderr, "  --dictate     Hotkey dictation: Option+Space to record, auto-paste on silence\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -I <secs>     Encoder processing interval in seconds (default: 2.0)\n");
    fprintf(stderr, "  --alt <c>     Show alternative tokens within cutoff distance (0.0-1.0)\n");
    fprintf(stderr, "  --monitor     Show non-intrusive symbols inline with output (stderr)\n");
    fprintf(stderr, "  --debug       Debug output (per-layer, per-chunk details)\n");
    fprintf(stderr, "  --silent      No status output (only transcription on stdout)\n");
    fprintf(stderr, "  -h            Show this help\n");
}

/* Drain pending tokens from stream and print to stdout */
static int first_token = 1;
static float alt_cutoff = -1; /* <0 means disabled */

static void drain_tokens(vox_stream_t *s) {
    if (alt_cutoff < 0) {
        /* Fast path: no alternatives */
        const char *tokens[64];
        int n;
        while ((n = vox_stream_get(s, tokens, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                const char *t = tokens[i];
                if (first_token) {
                    while (*t == ' ') t++;
                    first_token = 0;
                }
                fputs(t, stdout);
            }
            fflush(stdout);
        }
    } else {
        /* Alternatives mode */
        const int n_alt = 3;
        const char *tokens[64 * 3];
        int n;
        while ((n = vox_stream_get_alt(s, tokens, 64, n_alt)) > 0) {
            for (int i = 0; i < n; i++) {
                const char *best = tokens[i * n_alt];
                if (!best) continue;
                /* Check for alternatives */
                int has_alt = 0;
                for (int a = 1; a < n_alt; a++) {
                    if (tokens[i * n_alt + a]) { has_alt = 1; break; }
                }
                if (has_alt) {
                    fputc('[', stdout);
                    for (int a = 0; a < n_alt; a++) {
                        const char *alt = tokens[i * n_alt + a];
                        if (!alt) break;
                        if (a > 0) fputc('|', stdout);
                        const char *t = alt;
                        if (a == 0 && first_token) {
                            while (*t == ' ') t++;
                            first_token = 0;
                        }
                        fputs(t, stdout);
                    }
                    fputc(']', stdout);
                } else {
                    const char *t = best;
                    if (first_token) {
                        while (*t == ' ') t++;
                        first_token = 0;
                    }
                    fputs(t, stdout);
                }
            }
            fflush(stdout);
        }
    }
}

/* Feed audio in chunks, printing tokens as they become available.
 * feed_chunk controls granularity: smaller = more responsive token output. */
static int feed_chunk = DEFAULT_FEED_CHUNK;
static void feed_and_drain(vox_stream_t *s, const float *samples, int n_samples) {
    int off = 0;
    while (off < n_samples) {
        int chunk = n_samples - off;
        if (chunk > feed_chunk) chunk = feed_chunk;
        vox_stream_feed(s, samples + off, chunk);
        off += chunk;
        drain_tokens(s);
    }
}

int main(int argc, char **argv) {
    const char *model_dir = NULL;
    const char *input_wav = NULL;
    int verbosity = 1; /* 0=silent, 1=normal, 2=debug */
    int use_stdin = 0;
    int use_mic = 0;
    int use_wexproflow = 0;
    float interval = -1.0f; /* <0 means use default */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_wav = argv[++i];
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            interval = (float)atof(argv[++i]);
            if (interval <= 0) {
                fprintf(stderr, "Error: -I requires a positive number of seconds\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--alt") == 0 && i + 1 < argc) {
            alt_cutoff = (float)atof(argv[++i]);
            if (alt_cutoff < 0 || alt_cutoff > 1) {
                fprintf(stderr, "Error: --alt requires a value between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--stdin") == 0) {
            use_stdin = 1;
        } else if (strcmp(argv[i], "--from-mic") == 0) {
            use_mic = 1;
        } else if (strcmp(argv[i], "--dictate") == 0 ||
                   strcmp(argv[i], "--wexproflow") == 0) {
            use_wexproflow = 1;
        } else if (strcmp(argv[i], "--monitor") == 0) {
            extern int vox_monitor;
            vox_monitor = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            verbosity = 2;
        } else if (strcmp(argv[i], "--silent") == 0) {
            verbosity = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!model_dir || (!input_wav && !use_stdin && !use_mic && !use_wexproflow)) {
        usage(argv[0]);
        return 1;
    }
    if ((input_wav ? 1 : 0) + use_stdin + use_mic + use_wexproflow > 1) {
        fprintf(stderr, "Error: -i, --stdin, --from-mic, and --dictate are mutually exclusive\n");
        return 1;
    }

    vox_verbose = verbosity;
    vox_verbose_audio = (verbosity >= 2) ? 1 : 0;

#ifdef USE_METAL
    vox_metal_init();
#endif

    /* Load model */
    vox_ctx_t *ctx = vox_load(model_dir);
    if (!ctx) {
        fprintf(stderr, "Failed to load model from %s\n", model_dir);
        return 1;
    }

    /* ---- wexproflow mode ---- */
    if (use_wexproflow) {
#ifndef WEXPROFLOW
        fprintf(stderr, "Error: --dictate requires building with 'make wexproflow'\n");
#ifdef USE_METAL
        vox_metal_shutdown();
#endif
        vox_free(ctx);
        return 1;
#else
        /* Check Accessibility permission (needed for Cmd+V injection) */
        if (!vox_paste_check_access()) {
            vox_free(ctx);
            return 1;
        }

        /* Install SIGINT/SIGTERM handler before starting hotkey thread */
        struct sigaction sa;
        sa.sa_handler = sigint_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        /* Start global hotkey listener */
        if (vox_hotkey_start(wexproflow_hotkey_cb) != 0) {
            vox_free(ctx);
            return 1;
        }

        fprintf(stderr, "wexproflow ready. Option+Space to record, Escape to cancel. Ctrl+C to quit.\n");

        #define WF_SILENCE_THRESH  0.01f   /* RMS threshold (~-40 dBFS) */
        #define WF_WINDOW          160     /* 10ms at 16kHz */
        #define WF_SILENCE_PASS    60      /* 600ms pass-through (natural pauses) */
        #define WF_AUTO_STOP_MS    2000.0  /* 2.0s wall-clock silence → auto-stop */

        enum { WF_IDLE, WF_RECORDING } wf_state = WF_IDLE;
        vox_stream_t *wf_stream = NULL;
        int wf_first_token = 1;
        int wf_silence_count = 0;
        int wf_was_skipping = 0;
        int wf_heard_speech = 0;
        struct timeval wf_last_speech_tv;

        /* Drain tokens from stream and type them into the frontmost app */
        #define wf_drain_tokens() do { \
            const char *_toks[64]; \
            int _n; \
            while ((_n = vox_stream_get(wf_stream, _toks, 64)) > 0) { \
                for (int _i = 0; _i < _n; _i++) { \
                    const char *_t = _toks[_i]; \
                    if (wf_first_token) { \
                        while (*_t == ' ') _t++; \
                        if (*_t) wf_first_token = 0; \
                    } \
                    if (*_t) vox_type_text(_t); \
                } \
            } \
        } while (0)

        while (!mic_interrupted) {
            if (wf_state == WF_IDLE) {
                /* Block until hotkey event or SIGINT */
                pthread_mutex_lock(&wf_mutex);
                while (wf_event == 0 && !mic_interrupted)
                    pthread_cond_wait(&wf_cond, &wf_mutex);
                int ev = wf_event;
                wf_event = 0;
                pthread_mutex_unlock(&wf_mutex);

                if (mic_interrupted) break;
                if (ev != 1) continue; /* ignore cancel/escape when idle */

                /* Start mic + streaming transcription */
                if (vox_mic_start() != 0) {
                    fprintf(stderr, "Error: Failed to start microphone\n");
                    continue;
                }
                wf_stream = vox_stream_init(ctx);
                if (!wf_stream) {
                    fprintf(stderr, "Error: Failed to init stream\n");
                    vox_mic_stop();
                    continue;
                }
                vox_stream_set_continuous(wf_stream, 1);
                if (interval > 0)
                    vox_set_processing_interval(wf_stream, interval);
                wf_first_token = 1;
                wf_silence_count = 0;
                wf_was_skipping = 0;
                wf_heard_speech = 0;
                gettimeofday(&wf_last_speech_tv, NULL);
                wf_state = WF_RECORDING;
                vox_hotkey_set_recording(1);
                fprintf(stderr, "Recording...\n");
                continue;
            }

            /* WF_RECORDING: poll for hotkey events + read mic + type tokens */
            int ev = 0;
            pthread_mutex_lock(&wf_mutex);
            ev = wf_event;
            wf_event = 0;
            pthread_mutex_unlock(&wf_mutex);

            if (ev == 2) {
                /* Escape: cancel — stop without flushing */
                vox_mic_stop();
                vox_stream_free(wf_stream);
                wf_stream = NULL;
                wf_state = WF_IDLE;
                vox_hotkey_set_recording(0);
                fprintf(stderr, "\nCancelled.\n");
                continue;
            }

            if (ev == 1) {
                /* Option+Space again: stop recording, flush remaining tokens */
                vox_mic_stop();
                vox_stream_finish(wf_stream);
                wf_drain_tokens();
                vox_stream_free(wf_stream);
                wf_stream = NULL;
                wf_state = WF_IDLE;
                vox_hotkey_set_recording(0);
                fprintf(stderr, "\nDone.\n");
                continue;
            }

            /* Read mic samples */
            float mic_buf[4800]; /* 300ms max read */
            int n = vox_mic_read(mic_buf, 4800);
            if (n == 0) {
                usleep(10000); /* 10ms idle */
                continue;
            }

            /* Silence detection in 10ms windows — same logic as --from-mic.
             * Voice + short pauses are fed to the stream; extended silence
             * is skipped (flushed) to avoid wasting encoder/decoder time. */
            int off = 0;
            while (off + WF_WINDOW <= n) {
                float energy = 0;
                for (int i = 0; i < WF_WINDOW; i++) {
                    float v = mic_buf[off + i];
                    energy += v * v;
                }
                float rms = sqrtf(energy / WF_WINDOW);

                if (rms > WF_SILENCE_THRESH) {
                    if (wf_was_skipping)
                        wf_was_skipping = 0;
                    wf_heard_speech = 1;
                    gettimeofday(&wf_last_speech_tv, NULL);
                    vox_stream_feed(wf_stream, mic_buf + off, WF_WINDOW);
                    wf_silence_count = 0;
                } else {
                    wf_silence_count++;
                    if (wf_silence_count <= WF_SILENCE_PASS) {
                        vox_stream_feed(wf_stream, mic_buf + off, WF_WINDOW);
                    } else if (!wf_was_skipping) {
                        wf_was_skipping = 1;
                        vox_stream_flush(wf_stream);
                    }
                }
                off += WF_WINDOW;
            }
            /* Feed any remaining samples (< 1 window) */
            if (off < n)
                vox_stream_feed(wf_stream, mic_buf + off, n - off);

            /* Auto-stop: 2s wall-clock since last speech → finish and paste.
             * Wall-clock is immune to flush blocking (~11s) and ambient noise
             * spikes that would reset a sample counter. */
            struct timeval wf_now;
            gettimeofday(&wf_now, NULL);
            double wf_silent_ms = (wf_now.tv_sec  - wf_last_speech_tv.tv_sec)  * 1000.0 +
                                  (wf_now.tv_usec - wf_last_speech_tv.tv_usec) / 1000.0;
            if (wf_heard_speech && wf_silent_ms > WF_AUTO_STOP_MS) {
                vox_mic_stop();
                vox_stream_finish(wf_stream);
                wf_drain_tokens();
                vox_stream_free(wf_stream);
                wf_stream = NULL;
                wf_state = WF_IDLE;
                vox_hotkey_set_recording(0);
                wf_heard_speech = 0;
                fprintf(stderr, "\nDone.\n");
                continue;
            }

            /* Type out any new tokens */
            wf_drain_tokens();
        }

        /* Cleanup */
        if (wf_stream) {
            if (wf_state == WF_RECORDING) vox_mic_stop();
            vox_hotkey_set_recording(0);
            vox_stream_free(wf_stream);
        }
        vox_hotkey_stop();
        vox_free(ctx);
#ifdef USE_METAL
        vox_metal_shutdown();
#endif
        return 0;
#endif /* WEXPROFLOW */
    }

    vox_stream_t *s = vox_stream_init(ctx);
    if (!s) {
        fprintf(stderr, "Failed to init stream\n");
        vox_free(ctx);
        return 1;
    }
    if (alt_cutoff >= 0)
        vox_stream_set_alt(s, 3, alt_cutoff);
    if (interval > 0) {
        vox_set_processing_interval(s, interval);
        feed_chunk = (int)(interval * VOX_SAMPLE_RATE);
        if (feed_chunk < 160) feed_chunk = 160;
        if (feed_chunk > DEFAULT_FEED_CHUNK) feed_chunk = DEFAULT_FEED_CHUNK;
    }

    /* Enable continuous mode for live sources (auto-restart decoder) */
    if (use_mic || use_stdin)
        vox_stream_set_continuous(s, 1);

    if (use_mic) {
        /* Microphone capture with silence cancellation */
        if (vox_mic_start() != 0) {
            vox_stream_free(s);
            vox_free(ctx);
            return 1;
        }

        /* Install SIGINT handler for clean Ctrl+C exit */
        struct sigaction sa;
        sa.sa_handler = sigint_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);

        if (vox_verbose >= 1)
            fprintf(stderr, "Listening (Ctrl+C to stop)...\n");

        /* Silence cancellation state */
        #define MIC_WINDOW 160          /* 10ms at 16kHz */
        #define SILENCE_THRESH 0.002f   /* RMS threshold (~-54 dBFS) */
        #define SILENCE_PASS 60         /* pass-through windows (600ms) */
        float mic_buf[4800]; /* 300ms max read */
        int silence_count = 0;
        int was_skipping = 0; /* were we skipping silence? */
        int overbuf_warned = 0;

        while (!mic_interrupted) {
            /* Over-buffer detection */
            int avail = vox_mic_read_available();
            if (avail > 80000) { /* > 5 seconds buffered */
                if (!overbuf_warned) {
                    fprintf(stderr, "Warning: can't keep up, skipping audio\n");
                    overbuf_warned = 1;
                }
                /* Drain all but last ~1 second */
                float discard[4800];
                while (vox_mic_read_available() > 16000)
                    vox_mic_read(discard, 4800);
                silence_count = 0;
                was_skipping = 0;
            } else if (avail < 32000) { /* < 2 seconds: clear warning */
                overbuf_warned = 0;
            }

            int n = vox_mic_read(mic_buf, 4800);
            if (n == 0) {
                usleep(10000); /* 10ms idle sleep */
                continue;
            }

            /* Process in 10ms windows for silence cancellation */
            int off = 0;
            while (off + MIC_WINDOW <= n) {
                /* Compute RMS energy of this window */
                float energy = 0;
                for (int i = 0; i < MIC_WINDOW; i++) {
                    float v = mic_buf[off + i];
                    energy += v * v;
                }
                float rms = sqrtf(energy / MIC_WINDOW);

                if (rms > SILENCE_THRESH) {
                    /* Voice detected */
                    if (was_skipping)
                        was_skipping = 0;
                    vox_stream_feed(s, mic_buf + off, MIC_WINDOW);
                    silence_count = 0;
                } else {
                    /* Silence detected */
                    silence_count++;
                    if (silence_count <= SILENCE_PASS) {
                        /* Short silence: pass through (natural word gap) */
                        vox_stream_feed(s, mic_buf + off, MIC_WINDOW);
                    } else if (!was_skipping) {
                        /* Entering silence: flush buffered audio */
                        was_skipping = 1;
                        vox_stream_flush(s);
                    }
                }
                off += MIC_WINDOW;
            }

            /* Feed any remaining samples (< 1 window) */
            if (off < n)
                vox_stream_feed(s, mic_buf + off, n - off);

            drain_tokens(s);
        }

        vox_mic_stop();
        if (vox_verbose >= 1)
            fprintf(stderr, "\nStopping...\n");
    } else if (use_stdin) {
        /* Read enough to detect WAV vs raw and parse WAV header */
        uint8_t hdr[4096];
        size_t hdr_read = fread(hdr, 1, sizeof(hdr), stdin);
        if (hdr_read < 4) {
            fprintf(stderr, "Not enough data on stdin\n");
            vox_stream_free(s);
            vox_free(ctx);
            return 1;
        }

        /* Offset into hdr[] where PCM data starts (0 = raw s16le) */
        size_t pcm_offset = 0;

        if (hdr_read >= 44 && memcmp(hdr, "RIFF", 4) == 0 &&
            memcmp(hdr + 8, "WAVE", 4) == 0) {
            /* Parse WAV header to find data chunk */
            int wav_fmt = 0, wav_ch = 0, wav_rate = 0, wav_bits = 0;
            const uint8_t *p = hdr + 12;
            const uint8_t *end = hdr + hdr_read;
            int found_data = 0;
            while (p + 8 <= end) {
                uint32_t csz = (uint32_t)(p[4] | (p[5]<<8) | (p[6]<<16) | (p[7]<<24));
                if (memcmp(p, "fmt ", 4) == 0 && csz >= 16 && p + 8 + csz <= end) {
                    wav_fmt  = p[8] | (p[9]<<8);
                    wav_ch   = p[10] | (p[11]<<8);
                    wav_rate  = p[12] | (p[13]<<8) | (p[14]<<16) | (p[15]<<24);
                    wav_bits = p[22] | (p[23]<<8);
                } else if (memcmp(p, "data", 4) == 0) {
                    pcm_offset = (size_t)(p + 8 - hdr);
                    found_data = 1;
                    break;
                }
                if (p + 8 + csz > end) break;
                p += 8 + csz;
                if (csz & 1) p++;
            }
            if (!found_data || wav_fmt != 1 || wav_bits != 16 || wav_ch < 1) {
                fprintf(stderr, "Invalid WAV on stdin (fmt=%d bits=%d)\n",
                        wav_fmt, wav_bits);
                vox_stream_free(s); vox_free(ctx); return 1;
            }
            if (wav_rate != VOX_SAMPLE_RATE || wav_ch != 1) {
                fprintf(stderr, "WAV stdin streaming requires 16kHz mono "
                        "(got %dHz %dch). Use: ffmpeg -i pipe:0 "
                        "-ar 16000 -ac 1 -f s16le pipe:1\n", wav_rate, wav_ch);
                vox_stream_free(s); vox_free(ctx); return 1;
            }
            if (vox_verbose >= 2)
                fprintf(stderr, "Streaming WAV s16le 16kHz mono from stdin\n");
        } else {
            if (vox_verbose >= 2)
                fprintf(stderr, "Streaming raw s16le 16kHz mono from stdin\n");
        }

        /* Feed any PCM data already in the header buffer */
        size_t pcm_in_hdr = hdr_read - pcm_offset;
        size_t pcm_frames = pcm_in_hdr / 2;
        if (pcm_frames > 0) {
            const int16_t *src = (const int16_t *)(hdr + pcm_offset);
            float fbuf[2048];
            for (size_t i = 0; i < pcm_frames; i++)
                fbuf[i] = src[i] / 32768.0f;
            vox_stream_feed(s, fbuf, (int)pcm_frames);
            drain_tokens(s);
        }

        /* Stream the rest incrementally */
        int16_t raw_buf[4096];
        float fbuf[4096];
        while (1) {
            size_t nread = fread(raw_buf, sizeof(int16_t), 4096, stdin);
            if (nread == 0) break;
            for (size_t i = 0; i < nread; i++)
                fbuf[i] = raw_buf[i] / 32768.0f;
            vox_stream_feed(s, fbuf, (int)nread);
            drain_tokens(s);
        }
    } else {
        /* File input: load WAV, feed in chunks */
        int n_samples = 0;
        float *samples = vox_load_wav(input_wav, &n_samples);
        if (!samples) {
            fprintf(stderr, "Failed to load %s\n", input_wav);
            vox_stream_free(s);
            vox_free(ctx);
            return 1;
        }
        if (vox_verbose >= 1)
            fprintf(stderr, "Audio: %d samples (%.1f seconds)\n",
                    n_samples, (float)n_samples / VOX_SAMPLE_RATE);

        feed_and_drain(s, samples, n_samples);
        free(samples);
    }

    vox_stream_finish(s);
    drain_tokens(s);
    fputs("\n", stdout);
    fflush(stdout);

    vox_stream_free(s);
    vox_free(ctx);
#ifdef USE_METAL
    vox_metal_shutdown();
#endif
    return 0;
}
