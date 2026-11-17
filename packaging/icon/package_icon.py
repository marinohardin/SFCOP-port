#!/usr/bin/env python3
"""Writes the per-platform icon files from the full-bleed 1024 px master that draw_icon.py's page
renders to: macos/NAME.icns (an 824 px body with a soft shadow on Apple's 1024 grid),
windows/NAME.ico (16 to 256 px), linux/LINUXNAME-<size>.png (16 to 512 px) and source/.
Needs Pillow and macOS's iconutil.

    python3 packaging/icon/package_icon.py MASTER.png OUTDIR SFCOP sfcop
"""
import os, shutil, subprocess, sys
from PIL import Image, ImageFilter
master, out, name, lname = sys.argv[1:5]
src = Image.open(master).convert("RGBA")
for d in ("macos", "windows", "linux", "source"):
    os.makedirs(os.path.join(out, d), exist_ok=True)
# macOS: Apple's grid, an 824 px body centred on a 1024 canvas with a soft drop shadow.
body = src.resize((824, 824), Image.LANCZOS)
shadow = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
mask = Image.new("L", (1024, 1024), 0); mask.paste(body.split()[3], (100, 112))
shadow.putalpha(mask.point(lambda a: a * 0.45).filter(ImageFilter.GaussianBlur(14)))
mac = Image.alpha_composite(shadow, Image.new("RGBA", (1024, 1024), (0, 0, 0, 0)))
mac.alpha_composite(body, (100, 100))
mac.save(os.path.join(out, "macos", "icon-macos-1024.png"))
iconset = os.path.join(out, "macos", name + ".iconset"); os.makedirs(iconset, exist_ok=True)
for s in (16, 32, 128, 256, 512):
    mac.resize((s, s), Image.LANCZOS).save(f"{iconset}/icon_{s}x{s}.png")
    mac.resize((2 * s, 2 * s), Image.LANCZOS).save(f"{iconset}/icon_{s}x{s}@2x.png")
subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(out, "macos", name + ".icns")], check=True)
shutil.rmtree(iconset)
# Windows: full bleed, every size Explorer asks for.
src.save(os.path.join(out, "windows", name + ".ico"),
         sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
for s in (16, 24, 32, 48, 64, 128, 256, 512):
    src.resize((s, s), Image.LANCZOS).save(os.path.join(out, "linux", f"{lname}-{s}.png"))
shutil.copy(master, os.path.join(out, "source", "icon-1024.png"))
