/*
 * REX Player DSP Plugin
 *
 * Parses .rx2/.rex files on-device, decodes DWOP compressed slices
 * (mono or L/delta stereo), and maps them across MIDI notes starting
 * at note 36 (C2). One-shot polyphonic playback with 16 voices.
 *
 * V2 API - instance-based for Signal Chain integration.
 *
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdint.h>
#include <math.h>

#include "rex_parser.h"

/* ------------------------------------------------------------------ */
/* Plugin API definitions (inline to avoid path issues)               */
/* ------------------------------------------------------------------ */

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MAX_REX_FILES    512
#define MAX_VOICES       16
#define FIRST_NOTE       36   /* C2 - first slice mapped here */
#define LOAD_DEBOUNCE_BLOCKS 3 /* ~9ms debounce at 128 frames/block */

static const host_api_v1_t *g_host = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[rex] %s", msg);
        g_host->log(buf);
    }
}

static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    const char *end = strchr(pos, '"');
    if (!end) return -1;
    int len = (int)(end - pos);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, pos, len);
    out[len] = '\0';
    return len;
}

/* ------------------------------------------------------------------ */
/* Voice engine                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* ADSR Envelope                                                       */
/* ------------------------------------------------------------------ */

#define ADSR_IDLE    0
#define ADSR_ATTACK  1
#define ADSR_DECAY   2
#define ADSR_SUSTAIN 3
#define ADSR_RELEASE 4

#define ADSR_MIN_TIME 0.001f  /* minimum segment time to avoid clicks */

typedef struct {
    float value;      /* current envelope level 0.0-1.0 */
    int stage;        /* ADSR_IDLE..ADSR_RELEASE */
    float attack;     /* seconds */
    float decay;      /* seconds */
    float sustain;    /* level 0.0-1.0 */
    float release;    /* seconds */
} adsr_t;

static void adsr_trigger(adsr_t *e) {
    e->stage = ADSR_ATTACK;
    /* Start from current value to avoid clicks on retrigger */
}

static void adsr_release(adsr_t *e) {
    if (e->stage != ADSR_IDLE) {
        e->stage = ADSR_RELEASE;
    }
}

static float adsr_process(adsr_t *e, float sample_rate) {
    float rate;
    switch (e->stage) {
        case ADSR_ATTACK:
            rate = (e->attack > ADSR_MIN_TIME) ? e->attack : ADSR_MIN_TIME;
            e->value += 1.0f / (rate * sample_rate);
            if (e->value >= 1.0f) {
                e->value = 1.0f;
                e->stage = ADSR_DECAY;
            }
            break;
        case ADSR_DECAY:
            rate = (e->decay > ADSR_MIN_TIME) ? e->decay : ADSR_MIN_TIME;
            e->value -= (1.0f - e->sustain) / (rate * sample_rate);
            if (e->value <= e->sustain) {
                e->value = e->sustain;
                e->stage = ADSR_SUSTAIN;
            }
            break;
        case ADSR_SUSTAIN:
            e->value = e->sustain;
            break;
        case ADSR_RELEASE:
            rate = (e->release > ADSR_MIN_TIME) ? e->release : ADSR_MIN_TIME;
            e->value -= e->value / (rate * sample_rate);
            if (e->value < 0.0001f) {
                e->value = 0.0f;
                e->stage = ADSR_IDLE;
            }
            break;
        default: /* ADSR_IDLE */
            e->value = 0.0f;
            break;
    }
    return e->value;
}

typedef struct {
    int active;
    int slice_index;
    float position;     /* current playback position in slice (fractional samples) */
    uint32_t age;       /* for voice stealing (oldest first) */
    uint8_t note;       /* MIDI note that triggered this voice */
    uint8_t velocity;   /* 0-127, for velocity scaling */
    int gate;           /* 1 = key held, 0 = key released */
    adsr_t env;         /* amplitude envelope */
} voice_t;

/* ------------------------------------------------------------------ */
/* REX file entry                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[512];
    char name[128];
} rex_entry_t;

/* ------------------------------------------------------------------ */
/* Per-Instance State                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Loaded REX file */
    rex_file_t rex;
    int rex_loaded;

    /* Voice engine */
    voice_t voices[MAX_VOICES];
    uint32_t voice_counter;

    /* File browser */
    rex_entry_t files[MAX_REX_FILES];
    int file_count;
    int file_index;
    char file_name[128];

    /* Parameters */
    float gain;
    int start_note;     /* MIDI note for first slice (default 36 = C2) */

    /* Envelope parameters */
    float attack;       /* 0.0 - 2.0 seconds */
    float decay;        /* 0.0 - 2.0 seconds */
    float sustain;      /* 0.0 - 1.0 level */
    float release;      /* 0.0 - 2.0 seconds */
    int mode;           /* 0 = trigger (one-shot), 1 = gate */
    int choke;          /* 0 = off (polyphonic), 1 = on (monophonic choke) */
    int transpose;      /* -12 to +12 semitones, default 0 */

    /* Deferred file loading (debounce for scrolling) */
    int deferred_file_index;      /* file index waiting for debounce */
    int deferred_load_countdown;  /* render blocks remaining before loading */

    /* Module info */
    char module_dir[512];
    char load_error[256];
} rex_instance_t;

/* ------------------------------------------------------------------ */
/* File scanning                                                       */
/* ------------------------------------------------------------------ */

static int rex_entry_cmp(const void *a, const void *b) {
    return strcasecmp(((const rex_entry_t *)a)->name, ((const rex_entry_t *)b)->name);
}

static void scan_rex_files(rex_instance_t *inst, const char *dir_path)
{
    inst->file_count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        plugin_log("Cannot open rex directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && inst->file_count < MAX_REX_FILES) {
        if (entry->d_name[0] == '.') continue;

        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;

        if (strcasecmp(ext, ".rx2") != 0 &&
            strcasecmp(ext, ".rex") != 0 &&
            strcasecmp(ext, ".rcy") != 0) continue;

        rex_entry_t *f = &inst->files[inst->file_count];
        snprintf(f->path, sizeof(f->path), "%s/%s", dir_path, entry->d_name);

        /* Strip extension for display name */
        int name_len = (int)(ext - entry->d_name);
        if (name_len >= (int)sizeof(f->name)) name_len = sizeof(f->name) - 1;
        memcpy(f->name, entry->d_name, name_len);
        f->name[name_len] = '\0';

        inst->file_count++;
    }

    closedir(dir);

    if (inst->file_count > 1) {
        qsort(inst->files, inst->file_count, sizeof(rex_entry_t), rex_entry_cmp);
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Found %d REX files", inst->file_count);
    plugin_log(msg);
}

/* ------------------------------------------------------------------ */
/* Load REX file                                                       */
/* ------------------------------------------------------------------ */

static int load_rex_file(rex_instance_t *inst, const char *path)
{
    /* Unload previous */
    if (inst->rex_loaded) {
        rex_free(&inst->rex);
        inst->rex_loaded = 0;
    }

    /* Stop all voices */
    for (int i = 0; i < MAX_VOICES; i++) {
        inst->voices[i].active = 0;
    }

    /* Read file into memory */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(inst->load_error, sizeof(inst->load_error), "Cannot open file");
        plugin_log(inst->load_error);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 50 * 1024 * 1024) {  /* 50MB max */
        snprintf(inst->load_error, sizeof(inst->load_error), "File too large or empty");
        fclose(fp);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        snprintf(inst->load_error, sizeof(inst->load_error), "Out of memory");
        fclose(fp);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, file_size, fp);
    fclose(fp);

    if ((long)read_bytes != file_size) {
        snprintf(inst->load_error, sizeof(inst->load_error), "Read error");
        free(buf);
        return -1;
    }

    /* Parse */
    int rc = rex_parse(&inst->rex, buf, file_size);
    free(buf);  /* parser copies what it needs */

    if (rc != 0) {
        snprintf(inst->load_error, sizeof(inst->load_error), "%s", inst->rex.error);
        plugin_log(inst->load_error);
        return -1;
    }

    inst->rex_loaded = 1;
    inst->load_error[0] = '\0';

    /* Extract display name */
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    const char *ext = strrchr(fname, '.');
    int name_len = ext ? (int)(ext - fname) : (int)strlen(fname);
    if (name_len >= (int)sizeof(inst->file_name)) name_len = sizeof(inst->file_name) - 1;
    memcpy(inst->file_name, fname, name_len);
    inst->file_name[name_len] = '\0';

    char msg[256];
    snprintf(msg, sizeof(msg), "Loaded: %s (%d slices, %d samples, %.1f BPM)",
             inst->file_name, inst->rex.slice_count, inst->rex.pcm_samples,
             inst->rex.tempo_bpm);
    plugin_log(msg);

    return 0;
}

/* ------------------------------------------------------------------ */
/* V2 API: create_instance                                             */
/* ------------------------------------------------------------------ */

static void* v2_create_instance(const char *module_dir, const char *json_defaults)
{
    rex_instance_t *inst = (rex_instance_t *)calloc(1, sizeof(rex_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    inst->gain = 1.0f;
    inst->start_note = FIRST_NOTE;
    inst->file_index = 0;
    strcpy(inst->file_name, "No REX loaded");

    /* Envelope defaults: transparent (instant attack, full sustain, no release) */
    inst->attack = 0.0f;
    inst->decay = 0.0f;
    inst->sustain = 1.0f;
    inst->release = 0.0f;
    inst->mode = 0;   /* trigger (one-shot) */
    inst->choke = 0;  /* off (polyphonic) */
    inst->transpose = 0;

    /* Scan for REX files */
    char rex_dir[512];
    snprintf(rex_dir, sizeof(rex_dir), "%s/loops", module_dir);
    scan_rex_files(inst, rex_dir);

    /* If no files in loops/, try module dir itself */
    if (inst->file_count == 0) {
        scan_rex_files(inst, module_dir);
    }

    /* Restore state from defaults if provided */
    if (json_defaults && json_defaults[0]) {
        char name[128];
        float f;

        if (json_get_string(json_defaults, "file_name", name, sizeof(name)) > 0) {
            /* Find file by name */
            for (int i = 0; i < inst->file_count; i++) {
                if (strcmp(inst->files[i].name, name) == 0) {
                    inst->file_index = i;
                    break;
                }
            }
        }

        if (json_get_number(json_defaults, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
        }
        if (json_get_number(json_defaults, "start_note", &f) == 0) {
            inst->start_note = (int)f;
            if (inst->start_note < 0) inst->start_note = 0;
            if (inst->start_note > 127) inst->start_note = 127;
        }
        if (json_get_number(json_defaults, "attack", &f) == 0) {
            inst->attack = f;
            if (inst->attack < 0.0f) inst->attack = 0.0f;
            if (inst->attack > 2.0f) inst->attack = 2.0f;
        }
        if (json_get_number(json_defaults, "decay", &f) == 0) {
            inst->decay = f;
            if (inst->decay < 0.0f) inst->decay = 0.0f;
            if (inst->decay > 2.0f) inst->decay = 2.0f;
        }
        if (json_get_number(json_defaults, "sustain", &f) == 0) {
            inst->sustain = f;
            if (inst->sustain < 0.0f) inst->sustain = 0.0f;
            if (inst->sustain > 1.0f) inst->sustain = 1.0f;
        }
        if (json_get_number(json_defaults, "release", &f) == 0) {
            inst->release = f;
            if (inst->release < 0.0f) inst->release = 0.0f;
            if (inst->release > 2.0f) inst->release = 2.0f;
        }
        if (json_get_number(json_defaults, "transpose", &f) == 0) {
            inst->transpose = (int)f;
            if (inst->transpose < -12) inst->transpose = -12;
            if (inst->transpose > 12) inst->transpose = 12;
        }
        {
            char str[16];
            if (json_get_string(json_defaults, "mode", str, sizeof(str)) > 0) {
                if (strcmp(str, "trigger") == 0) inst->mode = 0;
                else if (strcmp(str, "gate") == 0) inst->mode = 1;
            }
            if (json_get_string(json_defaults, "choke", str, sizeof(str)) > 0) {
                if (strcmp(str, "off") == 0) inst->choke = 0;
                else if (strcmp(str, "on") == 0) inst->choke = 1;
            }
        }
    }

    /* Load first/selected file */
    if (inst->file_count > 0) {
        load_rex_file(inst, inst->files[inst->file_index].path);
    }

    plugin_log("REX Player initialized");
    return inst;
}

/* ------------------------------------------------------------------ */
/* V2 API: destroy_instance                                            */
/* ------------------------------------------------------------------ */

static void v2_destroy_instance(void *instance)
{
    rex_instance_t *inst = (rex_instance_t *)instance;
    if (!inst) return;

    if (inst->rex_loaded) {
        rex_free(&inst->rex);
    }

    free(inst);
    plugin_log("REX Player destroyed");
}

/* ------------------------------------------------------------------ */
/* V2 API: on_midi                                                     */
/* ------------------------------------------------------------------ */

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source)
{
    rex_instance_t *inst = (rex_instance_t *)instance;
    if (!inst || !inst->rex_loaded || len < 2) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note = msg[1];
    uint8_t velocity = (len > 2) ? msg[2] : 0;

    if (status == 0x90 && velocity > 0) {
        /* Note On - trigger slice */
        int slice_index = (int)note - inst->start_note;
        if (slice_index < 0 || slice_index >= inst->rex.slice_count) return;

        /* Check slice has audio */
        rex_slice_t *slice = &inst->rex.slices[slice_index];
        if (slice->sample_length == 0) return;

        /* Choke: silence all other active voices */
        if (inst->choke) {
            for (int i = 0; i < MAX_VOICES; i++) {
                inst->voices[i].active = 0;
            }
        }

        /* Find a free voice, or steal the oldest */
        int best = -1;
        uint32_t oldest_age = UINT32_MAX;

        for (int i = 0; i < MAX_VOICES; i++) {
            if (!inst->voices[i].active) {
                best = i;
                break;
            }
            if (inst->voices[i].age < oldest_age) {
                oldest_age = inst->voices[i].age;
                best = i;
            }
        }

        if (best < 0) best = 0;  /* shouldn't happen */

        voice_t *v = &inst->voices[best];
        v->active = 1;
        v->slice_index = slice_index;
        v->position = 0;
        v->age = ++inst->voice_counter;
        v->note = note;
        v->velocity = velocity;
        v->gate = 1;

        /* Initialize and trigger envelope */
        v->env.attack = inst->attack;
        v->env.decay = inst->decay;
        v->env.sustain = inst->sustain;
        v->env.release = inst->release;
        v->env.value = 0.0f;
        adsr_trigger(&v->env);
    }
    else if ((status == 0x80) || (status == 0x90 && velocity == 0)) {
        /* Note Off - release matching voices */
        for (int i = 0; i < MAX_VOICES; i++) {
            voice_t *v = &inst->voices[i];
            if (v->active && v->note == note && v->gate) {
                v->gate = 0;
                if (inst->mode == 1) {
                    /* Gate mode: enter release stage */
                    adsr_release(&v->env);
                }
                /* Trigger mode: do nothing, let slice play out */
            }
        }
    }
    else if (status == 0xB0) {
        /* Control Change */
        if (note == 123) {
            /* All notes off */
            for (int i = 0; i < MAX_VOICES; i++) {
                inst->voices[i].active = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* V2 API: set_param                                                   */
/* ------------------------------------------------------------------ */

static void v2_set_param(void *instance, const char *key, const char *val)
{
    rex_instance_t *inst = (rex_instance_t *)instance;
    if (!inst) return;

    if (strcmp(key, "preset") == 0 || strcmp(key, "file_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->file_count && idx != inst->file_index) {
            /* Update index and display name immediately for responsive UI */
            inst->file_index = idx;
            strncpy(inst->file_name, inst->files[idx].name, sizeof(inst->file_name) - 1);
            inst->file_name[sizeof(inst->file_name) - 1] = '\0';
            /* Defer actual file load (debounce for fast scrolling) */
            inst->deferred_file_index = idx;
            inst->deferred_load_countdown = LOAD_DEBOUNCE_BLOCKS;
        }
    }
    else if (strcmp(key, "next_file") == 0 || strcmp(key, "next_preset") == 0) {
        if (inst->file_count > 0) {
            inst->file_index = (inst->file_index + 1) % inst->file_count;
            strncpy(inst->file_name, inst->files[inst->file_index].name, sizeof(inst->file_name) - 1);
            inst->file_name[sizeof(inst->file_name) - 1] = '\0';
            inst->deferred_file_index = inst->file_index;
            inst->deferred_load_countdown = LOAD_DEBOUNCE_BLOCKS;
        }
    }
    else if (strcmp(key, "prev_file") == 0 || strcmp(key, "prev_preset") == 0) {
        if (inst->file_count > 0) {
            inst->file_index = (inst->file_index - 1 + inst->file_count) % inst->file_count;
            strncpy(inst->file_name, inst->files[inst->file_index].name, sizeof(inst->file_name) - 1);
            inst->file_name[sizeof(inst->file_name) - 1] = '\0';
            inst->deferred_file_index = inst->file_index;
            inst->deferred_load_countdown = LOAD_DEBOUNCE_BLOCKS;
        }
    }
    else if (strcmp(key, "gain") == 0) {
        inst->gain = (float)atof(val);
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 2.0f) inst->gain = 2.0f;
    }
    else if (strcmp(key, "start_note") == 0) {
        inst->start_note = atoi(val);
        if (inst->start_note < 0) inst->start_note = 0;
        if (inst->start_note > 127) inst->start_note = 127;
    }
    else if (strcmp(key, "attack") == 0) {
        inst->attack = (float)atof(val);
        if (inst->attack < 0.0f) inst->attack = 0.0f;
        if (inst->attack > 2.0f) inst->attack = 2.0f;
    }
    else if (strcmp(key, "decay") == 0) {
        inst->decay = (float)atof(val);
        if (inst->decay < 0.0f) inst->decay = 0.0f;
        if (inst->decay > 2.0f) inst->decay = 2.0f;
    }
    else if (strcmp(key, "sustain") == 0) {
        inst->sustain = (float)atof(val);
        if (inst->sustain < 0.0f) inst->sustain = 0.0f;
        if (inst->sustain > 1.0f) inst->sustain = 1.0f;
    }
    else if (strcmp(key, "release") == 0) {
        inst->release = (float)atof(val);
        if (inst->release < 0.0f) inst->release = 0.0f;
        if (inst->release > 2.0f) inst->release = 2.0f;
    }
    else if (strcmp(key, "mode") == 0) {
        if (strcmp(val, "trigger") == 0) inst->mode = 0;
        else if (strcmp(val, "gate") == 0) inst->mode = 1;
    }
    else if (strcmp(key, "choke") == 0) {
        if (strcmp(val, "off") == 0) inst->choke = 0;
        else if (strcmp(val, "on") == 0) inst->choke = 1;
    }
    else if (strcmp(key, "transpose") == 0) {
        inst->transpose = atoi(val);
        if (inst->transpose < -12) inst->transpose = -12;
        if (inst->transpose > 12) inst->transpose = 12;
    }
    else if (strcmp(key, "all_notes_off") == 0 || strcmp(key, "panic") == 0) {
        for (int i = 0; i < MAX_VOICES; i++) {
            inst->voices[i].active = 0;
        }
    }
    else if (strcmp(key, "state") == 0) {
        /* Restore state from JSON */
        float f;
        char name[128];

        if (json_get_string(val, "file_name", name, sizeof(name)) > 0) {
            for (int i = 0; i < inst->file_count; i++) {
                if (strcmp(inst->files[i].name, name) == 0) {
                    if (i != inst->file_index) {
                        inst->file_index = i;
                        strncpy(inst->file_name, inst->files[i].name, sizeof(inst->file_name) - 1);
                        inst->file_name[sizeof(inst->file_name) - 1] = '\0';
                        inst->deferred_file_index = i;
                        inst->deferred_load_countdown = LOAD_DEBOUNCE_BLOCKS;
                    }
                    break;
                }
            }
        } else if (json_get_number(val, "file_index", &f) == 0) {
            int idx = (int)f;
            if (idx >= 0 && idx < inst->file_count && idx != inst->file_index) {
                inst->file_index = idx;
                strncpy(inst->file_name, inst->files[idx].name, sizeof(inst->file_name) - 1);
                inst->file_name[sizeof(inst->file_name) - 1] = '\0';
                inst->deferred_file_index = idx;
                inst->deferred_load_countdown = LOAD_DEBOUNCE_BLOCKS;
            }
        }

        if (json_get_number(val, "gain", &f) == 0) {
            inst->gain = f;
            if (inst->gain < 0.0f) inst->gain = 0.0f;
            if (inst->gain > 2.0f) inst->gain = 2.0f;
        }
        if (json_get_number(val, "start_note", &f) == 0) {
            inst->start_note = (int)f;
            if (inst->start_note < 0) inst->start_note = 0;
            if (inst->start_note > 127) inst->start_note = 127;
        }
        if (json_get_number(val, "attack", &f) == 0) {
            inst->attack = f;
            if (inst->attack < 0.0f) inst->attack = 0.0f;
            if (inst->attack > 2.0f) inst->attack = 2.0f;
        }
        if (json_get_number(val, "decay", &f) == 0) {
            inst->decay = f;
            if (inst->decay < 0.0f) inst->decay = 0.0f;
            if (inst->decay > 2.0f) inst->decay = 2.0f;
        }
        if (json_get_number(val, "sustain", &f) == 0) {
            inst->sustain = f;
            if (inst->sustain < 0.0f) inst->sustain = 0.0f;
            if (inst->sustain > 1.0f) inst->sustain = 1.0f;
        }
        if (json_get_number(val, "release", &f) == 0) {
            inst->release = f;
            if (inst->release < 0.0f) inst->release = 0.0f;
            if (inst->release > 2.0f) inst->release = 2.0f;
        }
        if (json_get_number(val, "transpose", &f) == 0) {
            inst->transpose = (int)f;
            if (inst->transpose < -12) inst->transpose = -12;
            if (inst->transpose > 12) inst->transpose = 12;
        }
        {
            char str[16];
            if (json_get_string(val, "mode", str, sizeof(str)) > 0) {
                if (strcmp(str, "trigger") == 0) inst->mode = 0;
                else if (strcmp(str, "gate") == 0) inst->mode = 1;
            }
            if (json_get_string(val, "choke", str, sizeof(str)) > 0) {
                if (strcmp(str, "off") == 0) inst->choke = 0;
                else if (strcmp(str, "on") == 0) inst->choke = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* V2 API: get_param                                                   */
/* ------------------------------------------------------------------ */

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len)
{
    rex_instance_t *inst = (rex_instance_t *)instance;
    if (!inst) return -1;

    if (strcmp(key, "preset") == 0 || strcmp(key, "file_index") == 0) {
        return snprintf(buf, buf_len, "%d", inst->file_index);
    }
    else if (strcmp(key, "preset_name") == 0 || strcmp(key, "file_name") == 0) {
        strncpy(buf, inst->file_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    else if (strcmp(key, "preset_count") == 0 || strcmp(key, "file_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->file_count);
    }
    else if (strcmp(key, "slice_count") == 0) {
        if (inst->rex_loaded) {
            return snprintf(buf, buf_len, "%d", inst->rex.slice_count);
        }
        return snprintf(buf, buf_len, "0");
    }
    else if (strcmp(key, "tempo") == 0) {
        if (inst->rex_loaded) {
            return snprintf(buf, buf_len, "%.1f", inst->rex.tempo_bpm);
        }
        return snprintf(buf, buf_len, "0");
    }
    else if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->gain);
    }
    else if (strcmp(key, "start_note") == 0) {
        return snprintf(buf, buf_len, "%d", inst->start_note);
    }
    else if (strcmp(key, "attack") == 0) {
        return snprintf(buf, buf_len, "%.3f", inst->attack);
    }
    else if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.3f", inst->decay);
    }
    else if (strcmp(key, "sustain") == 0) {
        return snprintf(buf, buf_len, "%.3f", inst->sustain);
    }
    else if (strcmp(key, "release") == 0) {
        return snprintf(buf, buf_len, "%.3f", inst->release);
    }
    else if (strcmp(key, "mode") == 0) {
        return snprintf(buf, buf_len, "%s", inst->mode ? "gate" : "trigger");
    }
    else if (strcmp(key, "choke") == 0) {
        return snprintf(buf, buf_len, "%s", inst->choke ? "on" : "off");
    }
    else if (strcmp(key, "transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->transpose);
    }
    else if (strcmp(key, "bank_name") == 0) {
        /* For chain compatibility: bank = folder */
        strncpy(buf, "REX Loops", buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    else if (strcmp(key, "patch_in_bank") == 0) {
        return snprintf(buf, buf_len, "%d", inst->file_index + 1);
    }
    else if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "1");
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"file_name\":\"%s\",\"file_index\":%d,\"gain\":%.2f,\"start_note\":%d,"
            "\"attack\":%.3f,\"decay\":%.3f,\"sustain\":%.3f,\"release\":%.3f,"
            "\"mode\":\"%s\",\"choke\":\"%s\",\"transpose\":%d}",
            inst->file_name, inst->file_index, inst->gain, inst->start_note,
            inst->attack, inst->decay, inst->sustain, inst->release,
            inst->mode ? "gate" : "trigger", inst->choke ? "on" : "off",
            inst->transpose);
    }
    else if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy =
            "{"
                "\"modes\":null,"
                "\"levels\":{"
                    "\"root\":{"
                        "\"label\":\"REX\","
                        "\"list_param\":\"preset\","
                        "\"count_param\":\"preset_count\","
                        "\"name_param\":\"preset_name\","
                        "\"children\":null,"
                        "\"knobs\":[\"gain\",\"start_note\",\"transpose\",\"attack\",\"decay\",\"sustain\",\"release\",\"mode\",\"choke\"],"
                        "\"params\":["
                            "{\"key\":\"gain\",\"label\":\"Gain\"},"
                            "{\"key\":\"start_note\",\"label\":\"Start Note\"},"
                            "{\"key\":\"transpose\",\"label\":\"Transpose\"},"
                            "{\"key\":\"attack\",\"label\":\"Attack\"},"
                            "{\"key\":\"decay\",\"label\":\"Decay\"},"
                            "{\"key\":\"sustain\",\"label\":\"Sustain\"},"
                            "{\"key\":\"release\",\"label\":\"Release\"},"
                            "{\"key\":\"mode\",\"label\":\"Mode\"},"
                            "{\"key\":\"choke\",\"label\":\"Choke\"}"
                        "]"
                    "}"
                "}"
            "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }
    else if (strcmp(key, "chain_params") == 0) {
        const char *params =
            "["
                "{\"key\":\"preset\",\"name\":\"File\",\"type\":\"int\",\"min\":0,\"max\":9999},"
                "{\"key\":\"gain\",\"name\":\"Gain\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.01},"
                "{\"key\":\"start_note\",\"name\":\"Start Note\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
                "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.001},"
                "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.001},"
                "{\"key\":\"sustain\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
                "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":2,\"step\":0.001},"
                "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"trigger\",\"gate\"]},"
                "{\"key\":\"choke\",\"name\":\"Choke\",\"type\":\"enum\",\"options\":[\"off\",\"on\"]},"
                "{\"key\":\"transpose\",\"name\":\"Transpose\",\"type\":\"int\",\"min\":-12,\"max\":12,\"step\":1}"
            "]";
        int len = strlen(params);
        if (len < buf_len) {
            strcpy(buf, params);
            return len;
        }
        return -1;
    }
    else if (strcmp(key, "load_error") == 0) {
        if (inst->load_error[0]) {
            strncpy(buf, inst->load_error, buf_len - 1);
            buf[buf_len - 1] = '\0';
            return strlen(buf);
        }
        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* V2 API: get_error                                                   */
/* ------------------------------------------------------------------ */

static int v2_get_error(void *instance, char *buf, int buf_len)
{
    rex_instance_t *inst = (rex_instance_t *)instance;
    if (!inst || !inst->load_error[0]) return 0;

    int len = strlen(inst->load_error);
    if (len >= buf_len) len = buf_len - 1;
    memcpy(buf, inst->load_error, len);
    buf[len] = '\0';
    return len;
}

/* ------------------------------------------------------------------ */
/* V2 API: render_block                                                */
/* ------------------------------------------------------------------ */

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames)
{
    rex_instance_t *inst = (rex_instance_t *)instance;

    /* Handle deferred file load (debounce for scrolling) */
    if (inst && inst->deferred_load_countdown > 0) {
        inst->deferred_load_countdown--;
        if (inst->deferred_load_countdown == 0) {
            load_rex_file(inst, inst->files[inst->deferred_file_index].path);
        }
    }

    /* Clear output */
    memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    if (!inst || !inst->rex_loaded || !inst->rex.pcm_data) return;

    float gain = inst->gain;
    float rate = powf(2.0f, inst->transpose / 12.0f);

    /* Mix all active voices */
    int is_stereo = (inst->rex.pcm_channels == 2);

    for (int v = 0; v < MAX_VOICES; v++) {
        voice_t *voice = &inst->voices[v];
        if (!voice->active) continue;

        rex_slice_t *slice = &inst->rex.slices[voice->slice_index];
        const int16_t *pcm = inst->rex.pcm_data;
        int slice_start = (int)slice->sample_offset;
        int slice_end = slice_start + (int)slice->sample_length;
        int pcm_limit = inst->rex.pcm_samples;
        float vel_scale = voice->velocity / 127.0f;
        int slice_done = 0;

        for (int i = 0; i < frames; i++) {
            /* Process envelope */
            float env_val = adsr_process(&voice->env, (float)MOVE_SAMPLE_RATE);

            /* Check if envelope has finished (release complete) */
            if (voice->env.stage == ADSR_IDLE) {
                voice->active = 0;
                break;
            }

            int ipos = (int)voice->position;
            int pos = slice_start + ipos;

            if (!slice_done && (pos >= slice_end || pos >= pcm_limit)) {
                /* Slice audio finished - force envelope into release */
                slice_done = 1;
                adsr_release(&voice->env);
            }

            float sample_l, sample_r;
            if (slice_done) {
                /* No more audio data - envelope is releasing to zero */
                sample_l = sample_r = 0.0f;
            } else {
                float frac = voice->position - (float)ipos;
                int pos1 = pos + 1;
                int pos1_valid = (pos1 < slice_end && pos1 < pcm_limit);

                if (is_stereo) {
                    float s0_l = (float)pcm[pos * 2];
                    float s0_r = (float)pcm[pos * 2 + 1];
                    float s1_l = pos1_valid ? (float)pcm[pos1 * 2] : s0_l;
                    float s1_r = pos1_valid ? (float)pcm[pos1 * 2 + 1] : s0_r;
                    sample_l = (s0_l + frac * (s1_l - s0_l)) * gain * env_val * vel_scale;
                    sample_r = (s0_r + frac * (s1_r - s0_r)) * gain * env_val * vel_scale;
                } else {
                    float s0 = (float)pcm[pos];
                    float s1 = pos1_valid ? (float)pcm[pos1] : s0;
                    sample_l = sample_r = (s0 + frac * (s1 - s0)) * gain * env_val * vel_scale;
                }
                voice->position += rate;
            }

            /* Soft clip */
            if (sample_l > 32767.0f) sample_l = 32767.0f;
            if (sample_l < -32768.0f) sample_l = -32768.0f;
            if (sample_r > 32767.0f) sample_r = 32767.0f;
            if (sample_r < -32768.0f) sample_r = -32768.0f;

            /* Mix into stereo output */
            int32_t left = (int32_t)out_interleaved_lr[i * 2] + (int16_t)sample_l;
            int32_t right = (int32_t)out_interleaved_lr[i * 2 + 1] + (int16_t)sample_r;

            /* Clamp after mixing */
            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;

            out_interleaved_lr[i * 2] = (int16_t)left;
            out_interleaved_lr[i * 2 + 1] = (int16_t)right;
        }
    }
}

/* ------------------------------------------------------------------ */
/* V2 API table and entry point                                        */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .get_error = v2_get_error,
    .render_block = v2_render_block
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    plugin_log("REX Player V2 API initialized");
    return &g_plugin_api_v2;
}
