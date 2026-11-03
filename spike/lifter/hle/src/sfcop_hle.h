/*
 * sfcop_hle.h - the HLE modules P1-2 adds on top of pcrecomp's win32hle.
 *
 * Each module exports a shim table in win32hle's form. The host registers
 * them before recomp_host_init(), so where a name is already answered by
 * win32hle (IDirectSoundBuffer::QueryInterface, RaiseException, ...) ours
 * wins: hle_resolve returns the first registration.
 *
 * Upstreamable modules (generic Win32/DirectX behaviour, no game knowledge)
 * say so in their header comment; game-specific ones say they are not.
 */
#ifndef SFCOP_HLE_H
#define SFCOP_HLE_H

#include <stdint.h>
#include "win32hle.h"

extern const win32hle_shim sfcop_hle_dinput[];     /* dinput.c */
extern const win32hle_shim sfcop_hle_mss[];        /* mss_sample.c */
extern const win32hle_shim sfcop_hle_ds3d[];       /* ds3d.c */
extern const win32hle_shim sfcop_hle_widefile[];   /* kernel32_wide.c */
extern const win32hle_shim sfcop_hle_misc[];       /* misc_stubs.c */
extern const win32hle_shim sfcop_hle_game[];       /* sfcop_game.c */

/* mss_sample.c: 1 makes every sample silent (handles, status and buffer
 * timing still behave); set before the first AIL_waveOutOpen. */
extern int sfcop_mss_silent;
/* For tests: no output and no mixer thread; time moves only when
 * sfcop_mss_advance(frames at 44.1 kHz) is called. */
extern int sfcop_mss_manual_clock;
void sfcop_mss_advance(int frames);
/* IMA ADPCM in WAVE blocks to interleaved s16 (malloc'd into *out): frames. */
uint32_t sfcop_mss_decode_ima(const uint8_t *in, uint32_t len, uint32_t block, int channels, int16_t **out);
/* sfcop_game.c: per-run setup once the image is mapped. */
void sfcop_game_init(void);

/* ---- helpers shared by the modules (hle_util.c) ---- */
/* The win32hle implementation of a name, skipping our own registrations, so
 * an override can hand a call on. NULL if win32hle has none. */
recomp_func_t sfcop_hle_original(const char *name);
/* 1 if the 16 bytes at guest va equal guid. */
int sfcop_guid_eq(uint32_t va, const uint8_t *guid);
/* A guest UTF-16 string as UTF-8 (the Windows ANSI code page is close enough
 * for the file names this game uses) into out; returns out. NULL -> NULL. */
char *sfcop_wide_to_utf8(uint32_t wva, char *out, size_t n);
/* UTF-8 into a guest UTF-16 buffer of n characters, NUL-terminated; returns
 * the characters written without the NUL. */
uint32_t sfcop_utf8_to_wide(const char *s, uint32_t wva, uint32_t n);
/* Log a gap once per name: "[hle-todo] name: note". */
void sfcop_todo_once(const char *name, const char *note);

#endif /* SFCOP_HLE_H */
