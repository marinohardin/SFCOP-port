/* Spike stand-in for SDL2_ttf, which is not installed here. TTF_Init fails, so
 * win32hle's GDI text and the host menu draw no glyphs; everything else links.
 * Only the calls runtime/win32hle/{gdidc,screen}.c make are declared. */
#pragma once
#include <SDL2/SDL.h>

typedef struct _TTF_Font TTF_Font;
#define TTF_HINTING_NORMAL 0
#define TTF_HINTING_MONO 2
#define TTF_STYLE_ITALIC 0x02
#define TTF_STYLE_UNDERLINE 0x04

static inline int TTF_Init(void) { return -1; }
static inline int TTF_WasInit(void) { return 0; }
static inline const char *TTF_GetError(void) { return "SDL2_ttf not built into the spike host"; }
static inline TTF_Font *TTF_OpenFont(const char *f, int s) { (void)f; (void)s; return NULL; }
static inline TTF_Font *TTF_OpenFontRW(SDL_RWops *r, int c, int s) { if (r && c) SDL_RWclose(r); (void)s; return NULL; }
static inline void TTF_CloseFont(TTF_Font *f) { (void)f; }
static inline int TTF_FontHeight(const TTF_Font *f) { (void)f; return 12; }
static inline int TTF_FontAscent(const TTF_Font *f) { (void)f; return 10; }
static inline int TTF_FontDescent(const TTF_Font *f) { (void)f; return -2; }
static inline const char *TTF_FontFaceFamilyName(const TTF_Font *f) { (void)f; return NULL; }
static inline void TTF_SetFontHinting(TTF_Font *f, int h) { (void)f; (void)h; }
static inline void TTF_SetFontKerning(TTF_Font *f, int k) { (void)f; (void)k; }
static inline void TTF_SetFontStyle(TTF_Font *f, int s) { (void)f; (void)s; }
static inline int TTF_SizeText(TTF_Font *f, const char *t, int *w, int *h) { (void)f; (void)t; if (w) *w = 0; if (h) *h = 0; return -1; }
static inline int TTF_SizeUTF8(TTF_Font *f, const char *t, int *w, int *h) { return TTF_SizeText(f, t, w, h); }
static inline int TTF_GlyphMetrics(TTF_Font *f, Uint16 c, int *a, int *b, int *d, int *e, int *adv) { (void)f; (void)c; (void)a; (void)b; (void)d; (void)e; if (adv) *adv = 0; return -1; }
static inline SDL_Surface *TTF_RenderText_Solid(TTF_Font *f, const char *t, SDL_Color c) { (void)f; (void)t; (void)c; return NULL; }
static inline SDL_Surface *TTF_RenderText_Blended(TTF_Font *f, const char *t, SDL_Color c) { (void)f; (void)t; (void)c; return NULL; }
static inline SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *f, const char *t, SDL_Color c) { (void)f; (void)t; (void)c; return NULL; }
