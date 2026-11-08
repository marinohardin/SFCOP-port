# src/hle/gdi32: GDI32

The game's GDI (DESIGN §4.7, §5.6): device contexts, DIB sections, fonts, blits and text, drawn in
software onto host-owned bitmaps. The game uses GDI for little: a memory DC and a DIB section it
blits from, text measured and drawn with one font, and the colour tables of 8-bit sections. USER32's
own drawing (dialogs, `FillRect`) goes through the same `Gdi`.

| File | What |
| --- | --- |
| `Gdi32Defs.h` | The GDI32 constants, `COLORREF` helpers |
| `Bitmap.h`, `.cpp` | A bitmap of 8, 16, 24 or 32 bits (`BI_RGB`, `BI_BITFIELDS`), its pixels from a `PixelMemory` |
| `Font.h`, `.cpp` | This module's font: 95 ASCII glyphs of 5x7 in 6x8 cells (a 5x7 bitmap font), scaled by the requested height; the metrics |
| `Gdi.h`, `Gdi.cpp`, `GdiDraw.cpp` | DCs and objects, `BitBlt`/`StretchBlt`/`PatBlt` with any raster operation, `FillRect`, text, palettes |
| `Screen.h` | `ReadScreen`, `NextScreen`: the screen's size, from the window or the display mode |
| `GuestGdi.h` | What the GDI32 and USER32 shims share: the one `Gdi`, its pixels in guest memory, its handles in win32hle's table |
| `Gdi32Guest.cpp` | The guest shims, exporting `sfcop_hle_gdi32[]`; built only with the lift |
| `*Test.cpp`, `GdiTestRig.h` | doctests on the host heap with counting handles and 8x8 bitmaps written by hand |

## Behaviour

- **Raster operations** are evaluated from the ROP3 code's truth table (pattern, source,
  destination), so every code works, not only the named ones; `SRCCOPY` between bitmaps of one depth
  above 8 bits copies rows. Colours convert through 0x00RRGGBB; into an 8-bit bitmap a colour takes
  the nearest entry of its colour table.
- **StretchBlt** samples the nearest pixel; a negative width or height mirrors and includes its
  origin pixel, as GDI does.
- **Text** is this module's font, whatever face the game asks for: the cell is 6x8 times the
  requested height over 8, rounded, at least 1. `TA_` alignment and `ETO_OPAQUE`/`ETO_CLIPPED` are
  honoured; `dx` spacing is used when given.
- **The screen** answers `GetDeviceCaps` and bounds USER32's cursor: the game's display
  (`GuestGdi.h`'s `screen()`, by `Screen.h`'s `ReadScreen`: the platform window's size; headless,
  with no window, the display mode win32hle's DirectDraw recorded, 640x480 until the game sets
  one), 32 bits, not palettised. A window DC (`GetDC`) draws nowhere: the game's frames are
  DirectDraw's.
- **DirectDraw's DCs.** A surface's `GetDC` is answered by the ddraw4 module through win32hle's own
  GDI (`hle_gdi_surface_dc`), so a GDI32 row given a DC this module did not make runs win32hle's row
  of that name. A blit between one of this module's DCs and a surface's is left to win32hle, which
  does not know this module's DCs.
