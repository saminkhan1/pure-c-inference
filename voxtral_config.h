/*
 * voxtral_config.h - Configuration file parser
 *
 * Reads key = value pairs from ~/.config/voxtral/config.
 * All keys are optional with sane defaults.
 * CLI flags override config file values.
 */

#ifndef VOXTRAL_CONFIG_H
#define VOXTRAL_CONFIG_H

typedef struct {
    char model_dir[512];       /* path to model weights */
    float silence_threshold;   /* RMS threshold (default 0.01) */
    int silence_duration_ms;   /* auto-stop after N ms silence (default 2000) */
    float processing_interval; /* encoder chunk frequency in seconds (default 2.0) */
    int sound_enabled;         /* play sounds on record start/stop (default 1) */
} vox_config_t;

/* Load config from ~/.config/voxtral/config.
 * Missing file or keys use defaults. Creates config dir if needed. */
void vox_config_load(vox_config_t *cfg);

#endif /* VOXTRAL_CONFIG_H */
