#!/usr/bin/env python3
"""Writes the application icon as an SVG page: a gold hexagon badge with "SFC2" and an
"ORION PIRATES" band over a green hex grid. Drawn here, with no image from anywhere else.

    python3 packaging/icon/draw_icon.py OUT.html

Render OUT.html at 1024x1024 with a transparent background (packaging/icon/README.md), then
packaging/icon/package_icon.py writes the per-platform files. Needs the Avenir Next font (macOS).
"""

import math, sys
# The palette: a dark green field, after the original game's box, with the orange of a phaser.
T = dict(tile=("#040b06", "#06120a", "#2c7a34"), glow=("#2fa84a", "#e0641a", "#17806a"),
         hexes="#5cc46a", rim="#6fd47e", title="SFC2", band="ORION PIRATES", band_size=96)
out = sys.argv[1]
def hexpts(cx, cy, r, rot=-90):
    return " ".join(f"{cx + r*math.cos(math.radians(rot + 60*i)):.1f},{cy + r*math.sin(math.radians(rot + 60*i)):.1f}" for i in range(6))
r = 63; w = math.sqrt(3) * r
H, Hin = hexpts(512, 512, 366), hexpts(512, 512, 312)
BAR = "M96,524 H928 L988,624 L928,724 H96 L36,624 Z"
html = f'''<!doctype html><html><head><meta charset="utf-8"><style>html,body{{margin:0;background:transparent;overflow:hidden}}</style></head><body>
<svg width="1024" height="1024" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">
<defs>
  <pattern id="hc" width="{w:.3f}" height="{3*r}" patternUnits="userSpaceOnUse">
    <path d="M{w/2:.3f},0 L{w:.3f},{r/2} L{w:.3f},{1.5*r} L{w/2:.3f},{2*r} L0,{1.5*r} L0,{r/2} Z M{w/2:.3f},{2*r} L{w/2:.3f},{3*r}" fill="none" stroke="{T['hexes']}" stroke-width="4.5"/>
  </pattern>
  <filter id="b30" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="78"/></filter>
  <filter id="b4" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="12"/></filter>
  <filter id="sh" x="-30%" y="-30%" width="160%" height="170%"><feDropShadow dx="0" dy="18" stdDeviation="20" flood-color="#000" flood-opacity=".7"/></filter>
  <radialGradient id="tile" cx=".5" cy=".42" r=".75">
    <stop offset="0" stop-color="{T['tile'][0]}"/><stop offset=".62" stop-color="{T['tile'][1]}"/><stop offset="1" stop-color="{T['tile'][2]}"/>
  </radialGradient>
  <clipPath id="tc"><rect width="1024" height="1024" rx="232"/></clipPath>
  <radialGradient id="tm" cx="512" cy="512" r="630" gradientUnits="userSpaceOnUse">
    <stop offset=".25" stop-color="#fff" stop-opacity=".25"/><stop offset=".8" stop-color="#fff" stop-opacity=".95"/><stop offset="1" stop-color="#fff" stop-opacity=".5"/>
  </radialGradient>
  <mask id="tmask"><rect width="1024" height="1024" fill="url(#tm)"/></mask>
  <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#fff3b0"/><stop offset=".28" stop-color="#f7c33a"/><stop offset=".55" stop-color="#c47a12"/><stop offset=".8" stop-color="#f2b12c"/><stop offset="1" stop-color="#8a4f0a"/>
  </linearGradient>
  <linearGradient id="bar" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#5a3410"/><stop offset=".5" stop-color="#2a1606"/><stop offset="1" stop-color="#4a2a0c"/>
  </linearGradient>
  <linearGradient id="chrome" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffffff"/><stop offset=".45" stop-color="#d5d9e2"/><stop offset=".52" stop-color="#8e94a4"/><stop offset=".75" stop-color="#c9ced9"/><stop offset="1" stop-color="#f4f6fa"/>
  </linearGradient>
  <radialGradient id="burst"><stop offset="0" stop-color="#fff"/><stop offset=".25" stop-color="#ffe98a"/><stop offset=".6" stop-color="#ff8a1e" stop-opacity=".7"/><stop offset="1" stop-color="#ff5a00" stop-opacity="0"/></radialGradient>
</defs>
<rect width="1024" height="1024" rx="232" fill="url(#tile)"/>
<g clip-path="url(#tc)">
  <g filter="url(#b30)" opacity=".75">
    <ellipse cx="226" cy="798" rx="286" ry="180" fill="{T['glow'][0]}"/>
    <ellipse cx="843" cy="241" rx="270" ry="166" fill="{T['glow'][1]}"/>
    <ellipse cx="602" cy="873" rx="270" ry="150" fill="{T['glow'][2]}"/>
  </g>
  <rect width="1024" height="1024" fill="url(#hc)" mask="url(#tmask)" opacity=".8"/>
  <line x1="-30" y1="919" x2="849" y2="187" stroke="#ffb020" stroke-width="27" filter="url(#b4)" opacity=".8"/>
  <line x1="-30" y1="919" x2="849" y2="187" stroke="#fff1a8" stroke-width="8"/>
  <circle cx="849" cy="187" r="104" fill="url(#burst)"/>
</g>
<rect x="3" y="3" width="1018" height="1018" rx="229" fill="none" stroke="{T['rim']}" stroke-opacity=".55" stroke-width="6"/>

<polygon points="{H}" fill="#07060f" fill-opacity=".86" stroke="#2a1503" stroke-width="66" stroke-linejoin="round"/>
<polygon points="{H}" fill="none" stroke="url(#gold)" stroke-width="45" stroke-linejoin="round"/>
<polygon points="{Hin}" fill="none" stroke="#fff3b0" stroke-opacity=".35" stroke-width="4.5"/>

<g filter="url(#sh)" font-family="'Avenir Next',sans-serif" font-weight="800" font-style="italic" font-size="262" text-anchor="middle" letter-spacing="-3">
  <text x="512" y="470" fill="none" stroke="#11131c" stroke-width="36" stroke-linejoin="round">{T['title']}</text>
  <text x="512" y="470" fill="url(#chrome)" stroke="#fff" stroke-opacity=".5" stroke-width="3">{T['title']}</text>
</g>
<g filter="url(#sh)">
  <path d="{BAR}" fill="url(#bar)" stroke="#2a1503" stroke-width="33" stroke-linejoin="round"/>
  <path d="{BAR}" fill="url(#bar)" stroke="url(#gold)" stroke-width="18" stroke-linejoin="round"/>
  <text x="512" y="658" text-anchor="middle" font-family="'Avenir Next',sans-serif" font-weight="800" font-size="{T['band_size']}" letter-spacing="5" fill="url(#gold)" stroke="#2a1503" stroke-width="2">{T['band']}</text>
</g>
</svg></body></html>'''
open(out, "w", encoding="utf-8").write(html)
