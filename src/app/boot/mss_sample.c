// TEMPORARY: replaced by Wave 1 B4 (audio)
/*
 * mss_sample.c - the Miles Sound System sample API (MSS32.DLL's AIL_*
 * digital-sample calls) on SDL2 audio. Upstreamable: yes; it complements
 * win32hle's mss.c, which answers startup, memory and file helpers.
 *
 * What is served: a waveOut-style digital driver (AIL_waveOutOpen/Close),
 * sample handles (allocate/release/init), their format, rate, volume, pan and
 * user data, and double-buffered playback: AIL_sample_buffer_ready names the
 * buffer to fill next, AIL_load_sample_buffer fills it and (re)starts the
 * sample, a zero-length load says the stream has ended. PCM 8/16-bit mono or
 * stereo and IMA ADPCM (AIL_set_sample_adpcm_block_size) are decoded to s16
 * when a buffer is loaded, so the guest may reuse its memory at once.
 *
 * The mixer runs on its own SDL audio device, at 44.1 kHz stereo. With
 * sfcop_mss_silent set (or no audio device) it runs on a thread of its own
 * that consumes the buffers in real time and outputs nothing, so a streamer
 * that waits for a buffer to drain behaves as it would with sound.
 *
 * A starved sample (both buffers played, no end marker) reports SMP_DONE and
 * restarts when the next buffer is loaded.
 */
#define _DARWIN_C_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "sfcop_hle.h"

int sfcop_mss_silent;
int sfcop_mss_manual_clock;

#define SMP_FREE    1u
#define SMP_DONE    2u
#define SMP_PLAYING 4u
#define SMP_STOPPED 8u
#define DIG_F_16BITS 1
#define DIG_F_STEREO 2
#define DIG_F_ADPCM  4
#define DIG_PCM_SIGN  1u
#define DIG_PCM_ORDER 2u
#define OUT_RATE 44100
#define MAX_SAMPLES 64
#define HANDLE_MAGIC 0x4C504D53u        /* "SMPL" */
#define DRIVER_MAGIC 0x56524944u        /* "DIRV" */

typedef struct {
    int16_t *pcm;                       /* interleaved s16, host memory */
    uint32_t frames;
    int channels;
    int full;
} sbuf_t;

typedef struct {
    int used;
    uint32_t handle;                    /* the guest's HSAMPLE */
    uint32_t status;
    int format, rate, volume, pan;
    uint32_t flags, adpcm_block;
    uint32_t user[8];
    sbuf_t buf[2];
    int cur, next_load, ended;
    double pos;                         /* frames into buf[cur] */
} sample_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sample_t g_smp[MAX_SAMPLES];
static int g_master = 127;
static int g_open;                      /* the output is running */
static SDL_AudioDeviceID g_dev;
static uint32_t g_driver, g_error_str;

/* ---- decoding ---- */
static const int k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static const int k_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int16_t ima_nibble(int nib, int *pred, int *idx) {
    int step = k_ima_step[*idx], diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += k_ima_index[nib & 15];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* IMA ADPCM in WAVE blocks: a 4-byte header per channel (first sample,
 * step index), then 4-byte groups of 8 nibbles per channel, in turn. */
uint32_t sfcop_mss_decode_ima(const uint8_t *in, uint32_t len, uint32_t block, int ch, int16_t **out) {
    if (block < 4u * (uint32_t)ch) block = len;
    uint32_t per_block = 1 + (block - 4u * (uint32_t)ch) * 2 / (uint32_t)ch;
    uint32_t blocks = (len + block - 1) / block;
    int16_t *o = (int16_t *)calloc((size_t)blocks * per_block * (size_t)ch + 1, sizeof(int16_t));
    uint32_t frames = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        const uint8_t *p = in + b * block;
        uint32_t n = len - b * block < block ? len - b * block : block;
        if (n < 4u * (uint32_t)ch) break;
        int pred[2], idx[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = (int16_t)(p[4 * c] | p[4 * c + 1] << 8);
            idx[c] = p[4 * c + 2] > 88 ? 88 : p[4 * c + 2];
            o[(size_t)frames * (size_t)ch + (size_t)c] = (int16_t)pred[c];
        }
        uint32_t base = frames + 1, got = 1;
        const uint8_t *d = p + 4 * ch, *end = p + n;
        if (ch == 1) {
            for (uint32_t k = 0; d + k < end; k++) {
                o[base + 2 * k] = ima_nibble(d[k] & 15, &pred[0], &idx[0]);
                o[base + 2 * k + 1] = ima_nibble(d[k] >> 4, &pred[0], &idx[0]);
                got = 1 + 2 * (k + 1);
            }
        } else {
            for (uint32_t g = 0; d + 8 * g + 8 <= end; g++) {
                for (int c = 0; c < 2; c++)
                    for (int k = 0; k < 4; k++) {
                        uint8_t byte = d[8 * g + 4 * (uint32_t)c + (uint32_t)k];
                        uint32_t f = base + 8 * g + 2 * (uint32_t)k;
                        o[2 * (size_t)f + (size_t)c] = ima_nibble(byte & 15, &pred[c], &idx[c]);
                        o[2 * (size_t)(f + 1) + (size_t)c] = ima_nibble(byte >> 4, &pred[c], &idx[c]);
                    }
                got = 1 + 8 * (g + 1);
            }
        }
        frames += got;
    }
    *out = o;
    return frames;
}

static uint32_t decode(const sample_t *s, const uint8_t *in, uint32_t len, int16_t **out) {
    int ch = s->format & DIG_F_STEREO ? 2 : 1;
    if (s->format & DIG_F_ADPCM) return sfcop_mss_decode_ima(in, len, s->adpcm_block, ch, out);
    int bytes = s->format & DIG_F_16BITS ? 2 : 1;
    uint32_t frames = len / (uint32_t)(bytes * ch);
    int16_t *o = (int16_t *)malloc((size_t)frames * (size_t)ch * sizeof(int16_t) + 2);
    for (uint32_t i = 0; i < frames * (uint32_t)ch; i++)
        o[i] = bytes == 2 ? (int16_t)(in[2 * i] | in[2 * i + 1] << 8)
             : (int16_t)(((s->flags & DIG_PCM_SIGN) ? (int)(int8_t)in[i] : (int)in[i] - 128) << 8);
    if (ch == 2 && (s->flags & DIG_PCM_ORDER))
        for (uint32_t f = 0; f < frames; f++) { int16_t t = o[2 * f]; o[2 * f] = o[2 * f + 1]; o[2 * f + 1] = t; }
    *out = o;
    return frames;
}

/* ---- the mixer ---- */
static void free_buf(sbuf_t *b) { free(b->pcm); memset(b, 0, sizeof *b); }

/* Advance every playing sample by `frames` output frames, adding into acc
 * (stereo floats) when it is not NULL. Under g_lock. */
static void mix_locked(float *acc, int frames) {
    for (int i = 0; i < MAX_SAMPLES; i++) {
        sample_t *s = &g_smp[i];
        if (!s->used || s->status != SMP_PLAYING) continue;
        double step = (double)(s->rate > 0 ? s->rate : 22050) / OUT_RATE;
        float v = (float)s->volume / 127.0f * (float)g_master / 127.0f;
        float gl = v * (s->pan <= 64 ? 1.0f : (float)(127 - s->pan) / 63.0f);
        float gr = v * (s->pan >= 64 ? 1.0f : (float)s->pan / 64.0f);
        for (int f = 0; f < frames; f++) {
            sbuf_t *b = &s->buf[s->cur];
            uint32_t at = (uint32_t)s->pos;
            while (b->full && at >= b->frames) {     /* this buffer is played out: on to the other */
                s->pos -= b->frames;
                free_buf(b);
                s->cur ^= 1;
                b = &s->buf[s->cur];
                at = (uint32_t)s->pos;
            }
            if (!b->full) { s->status = SMP_DONE; s->pos = 0; break; }   /* starved, or ended */
            if (acc) {
                const int16_t *p = b->pcm + (size_t)at * (size_t)b->channels;
                float l = p[0] / 32768.0f, r = b->channels > 1 ? p[1] / 32768.0f : l;
                acc[2 * f] += l * gl, acc[2 * f + 1] += r * gr;
            }
            s->pos += step;
        }
    }
}

static void sdl_mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    static float acc[2 * 4096];
    int frames = len / 4;
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_lock);
    mix_locked(acc, frames);
    pthread_mutex_unlock(&g_lock);
    int16_t *out = (int16_t *)stream;
    for (int k = 0; k < 2 * frames; k++) {
        float x = acc[k] * 32767.0f;
        out[k] = (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : x);
    }
}

static void *silent_mix(void *arg) {
    (void)arg;
    uint32_t last = hle_ticks_ms();
    for (;;) {
        usleep(10000);
        uint32_t now = hle_ticks_ms();
        int frames = (int)((uint64_t)(now - last) * OUT_RATE / 1000);
        last = now;
        pthread_mutex_lock(&g_lock);
        mix_locked(NULL, frames);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void sfcop_mss_advance(int frames) {
    pthread_mutex_lock(&g_lock);
    mix_locked(NULL, frames);
    pthread_mutex_unlock(&g_lock);
}

static void output_open(void) {
    if (g_open) return;
    g_open = 1;
    if (sfcop_mss_manual_clock) return;                  /* a test moves time with sfcop_mss_advance */
    if (!sfcop_mss_silent) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = OUT_RATE, want.format = AUDIO_S16SYS, want.channels = 2, want.samples = 1024, want.callback = sdl_mix;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_dev) {
            SDL_PauseAudioDevice(g_dev, 0);
            fprintf(stderr, "[mss] samples on %s, %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
            return;
        }
        fprintf(stderr, "[mss] no audio device (%s): silent\n", SDL_GetError());
    }
    pthread_t t;
    pthread_create(&t, NULL, silent_mix, NULL);
    pthread_detach(t);
    fprintf(stderr, "[mss] samples silent (timing only)\n");
}

/* ---- handles ---- */
static sample_t *smp(uint32_t h) {
    if (!h || MEM32(h) != HANDLE_MAGIC) return NULL;
    uint32_t i = MEM32(h + 4);
    return i < MAX_SAMPLES && g_smp[i].used && g_smp[i].handle == h ? &g_smp[i] : NULL;
}
static void reset(sample_t *s) {
    free_buf(&s->buf[0]), free_buf(&s->buf[1]);
    s->status = SMP_DONE;
    s->format = DIG_F_16BITS, s->flags = 0, s->rate = 22050, s->volume = 127, s->pan = 64, s->adpcm_block = 0;
    s->cur = s->next_load = s->ended = 0, s->pos = 0;
}

/* AIL_waveOutOpen(&driver, &hwaveout, device, WAVEFORMAT*) -> 0 */
static void a_waveOutOpen(void) {
    output_open();
    if (!g_driver) {
        g_driver = hle_va(hle_alloc(0x100));
        MEM32(g_driver) = DRIVER_MAGIC;
    }
    if (A32(0)) MEM32(A32(0)) = g_driver;
    if (A32(1)) MEM32(A32(1)) = g_driver + 0x80;         /* an HWAVEOUT the guest only keeps */
    RET(0, 4);
}
static void a_waveOutClose(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SAMPLES; i++) if (g_smp[i].used) g_smp[i].status = SMP_DONE;
    pthread_mutex_unlock(&g_lock);
    RETV(1);
}
static void a_allocate_sample_handle(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SAMPLES; i++)
        if (!g_smp[i].used) {
            sample_t *s = &g_smp[i];
            uint32_t h = s->handle;
            memset(s, 0, sizeof *s);
            if (!h) h = hle_va(hle_alloc(0x40));          /* kept for the run: a stale handle stays invalid */
            MEM32(h) = HANDLE_MAGIC, MEM32(h + 4) = (uint32_t)i;
            s->handle = h, s->used = 1;
            reset(s);
            pthread_mutex_unlock(&g_lock);
            RET(h, 1);
        }
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[mss] out of sample handles (%d)\n", MAX_SAMPLES);
    RET(0, 1);
}
static void a_release_sample_handle(void) {
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    if (s) { reset(s); s->used = 0; s->status = SMP_FREE; }
    pthread_mutex_unlock(&g_lock);
    RETV(1);
}
static void a_init_sample(void) {
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    if (s) reset(s);
    pthread_mutex_unlock(&g_lock);
    RETV(1);
}
static void a_end_sample(void) {
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    if (s) {
        free_buf(&s->buf[0]), free_buf(&s->buf[1]);
        s->status = SMP_DONE, s->cur = s->next_load = s->ended = 0, s->pos = 0;
    }
    pthread_mutex_unlock(&g_lock);
    RETV(1);
}
static void a_set_sample_type(void) {                    /* (s, format, flags) */
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    if (s) s->format = (int)A32(1), s->flags = A32(2);
    pthread_mutex_unlock(&g_lock);
    RETV(3);
}
#define SETTER(fn, field, cast)                          \
    static void fn(void) {                               \
        pthread_mutex_lock(&g_lock);                     \
        sample_t *s = smp(A32(0));                       \
        if (s) s->field = (cast)A32(1);                  \
        pthread_mutex_unlock(&g_lock);                   \
        RETV(2);                                         \
    }
SETTER(a_set_sample_playback_rate, rate, int)
SETTER(a_set_sample_volume, volume, int)
SETTER(a_set_sample_pan, pan, int)
SETTER(a_set_sample_adpcm_block_size, adpcm_block, uint32_t)
static void a_sample_volume(void) {
    sample_t *s = smp(A32(0));
    RET(s ? (uint32_t)s->volume : 0u, 1);
}
static void a_set_digital_master_volume(void) { g_master = (int)A32(1) > 127 ? 127 : (int)A32(1); RETV(2); }
static void a_set_sample_user_data(void) {               /* (s, index, value) */
    sample_t *s = smp(A32(0));
    if (s && A32(1) < 8) s->user[A32(1)] = A32(2);
    RETV(3);
}
static void a_sample_user_data(void) {                   /* (s, index) */
    sample_t *s = smp(A32(0));
    RET(s && A32(1) < 8 ? s->user[A32(1)] : 0u, 2);
}
static void a_sample_status(void) {
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    uint32_t st = s ? s->status : SMP_FREE;
    pthread_mutex_unlock(&g_lock);
    RET(st, 1);
}
/* (driver, rate, format): about 50 ms of the format, in 2 KB units */
static void a_minimum_sample_buffer_size(void) {
    int fmt = (int)A32(2), rate = (int)A32(1) > 0 ? (int)A32(1) : 22050;
    int bpf = (fmt & DIG_F_16BITS ? 2 : 1) * (fmt & DIG_F_STEREO ? 2 : 1);
    if (fmt & DIG_F_ADPCM) bpf = 1;                      /* 4 bits a sample: at most a byte a frame */
    uint32_t n = (uint32_t)(rate * bpf / 20);
    n = (n + 2047) & ~2047u;
    RET(n < 2048 ? 2048u : n, 3);
}
static void a_sample_buffer_ready(void) {                /* -> 0 or 1: fill that one; -1: both full */
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    uint32_t r = 0xFFFFFFFFu;
    if (s && !s->ended && !s->buf[s->next_load].full) r = (uint32_t)s->next_load;
    pthread_mutex_unlock(&g_lock);
    RET(r, 1);
}
static void a_load_sample_buffer(void) {                 /* (s, buffer, data, len) */
    uint32_t n = A32(1), len = A32(3);
    int16_t *pcm = NULL;
    pthread_mutex_lock(&g_lock);
    sample_t *s = smp(A32(0));
    if (s && n < 2) {
        if (!len || !A32(2)) {
            s->ended = 1;                                /* no more data: stop when the rest is played */
        } else {
            uint32_t frames = decode(s, (const uint8_t *)hle_ptr(A32(2)), len, &pcm);
            sbuf_t *b = &s->buf[n];
            free_buf(b);
            b->pcm = pcm, b->frames = frames, b->channels = s->format & DIG_F_STEREO ? 2 : 1, b->full = frames > 0;
            s->next_load = (int)n ^ 1;
            s->ended = 0;
            if (s->status != SMP_PLAYING) {              /* the first buffer, or a restart after starving */
                if (!s->buf[s->cur].full) s->cur = (int)n, s->pos = 0;
                s->status = SMP_PLAYING;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    RETV(4);
}
static void a_serve(void) { RETV(0); }                   /* the mixer has its own thread */
static void a_last_error(void) {
    if (!g_error_str) g_error_str = hle_guest_strdup("");
    RET(g_error_str, 0);
}
static void a_set_DirectSound_HWND(void) { RET(0, 2); }
/* (dir) -> the previous one. win32hle's mss.c answers with a host static,
 * which the guest cannot be handed; this keeps the string in guest memory. */
static void a_set_redist_directory(void) {
    static uint32_t prev;
    if (!prev) prev = hle_va(hle_alloc(260));
    uint32_t old = hle_guest_strdup((const char *)hle_ptr(prev));
    if (A32(0)) snprintf((char *)hle_ptr(prev), 260, "%s", ASTR(0));
    RET(old, 1);
}

const win32hle_shim sfcop_spike_mss[] = {
    { "_AIL_waveOutOpen@16", a_waveOutOpen }, { "_AIL_waveOutClose@4", a_waveOutClose },
    { "_AIL_allocate_sample_handle@4", a_allocate_sample_handle },
    { "_AIL_release_sample_handle@4", a_release_sample_handle },
    { "_AIL_init_sample@4", a_init_sample }, { "_AIL_end_sample@4", a_end_sample },
    { "_AIL_set_sample_type@12", a_set_sample_type },
    { "_AIL_set_sample_playback_rate@8", a_set_sample_playback_rate },
    { "_AIL_set_sample_adpcm_block_size@8", a_set_sample_adpcm_block_size },
    { "_AIL_set_sample_volume@8", a_set_sample_volume }, { "_AIL_sample_volume@4", a_sample_volume },
    { "_AIL_set_sample_pan@8", a_set_sample_pan },
    { "_AIL_set_digital_master_volume@8", a_set_digital_master_volume },
    { "_AIL_set_sample_user_data@12", a_set_sample_user_data }, { "_AIL_sample_user_data@8", a_sample_user_data },
    { "_AIL_sample_status@4", a_sample_status },
    { "_AIL_minimum_sample_buffer_size@12", a_minimum_sample_buffer_size },
    { "_AIL_sample_buffer_ready@4", a_sample_buffer_ready }, { "_AIL_load_sample_buffer@16", a_load_sample_buffer },
    { "_AIL_serve@0", a_serve }, { "_AIL_last_error@0", a_last_error },
    { "_AIL_set_DirectSound_HWND@8", a_set_DirectSound_HWND },
    { "_AIL_set_redist_directory@4", a_set_redist_directory },
    { 0, 0 }
};
