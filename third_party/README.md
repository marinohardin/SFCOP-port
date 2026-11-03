# third_party/ (task P0-6)

`INVENTORY.md` lists the third-party code in `StarFleetOP.exe` 2.5.5.2 and what to do with each piece.
Each vendored tree has a `PROVENANCE.md`.

| Path | What |
|---|---|
| `INVENTORY.md` | Table of components, evidence, versions, licences and port strategy |
| `cryptopp/src/` | Crypto++ 3.1 library sources (vendored). It is in the SDK lib but **not** in the exe. |
| `cryptopp/evidence/` | Version markers and object list for `cryptlibR.lib` |
| the out-of-repo headers folder | WONAPI headers, **reference only** (leaked source, no licence) |
| `wonapi/evidence/` | Exe vs archive comparison of classes and `WS_*` statuses |
| `gamespy/` | Component detection and provenance; no source vendored |
| `evidence/` | RTTI namespace counts across binaries; middleware imports |
| `scripts/` | Everything that produced the above |

## Reproduce

Run from the repo root. Large downloads go to `$SFCOP_ROOT/work/`; override
with `SFCOP_WORK`.

```sh
G="reference/installed game/Starfleet Command Orion Pirates"
L="$G/OP API - R2.1/Scripts/Libraries"
W=$SFCOP_ROOT/work

# 1. fetch pinned sources into work/ (sha256- and commit-pinned), then copy the vendored subsets
sh third_party/scripts/fetch_sources.sh
sh third_party/scripts/vendor.sh

# 2. Crypto++ version pin (needs `nm`, which ships with the Xcode command-line tools)
python3 -I third_party/scripts/cryptopp_inventory.py "$L/cryptlibR.lib" third_party/cryptopp/evidence third_party/cryptopp/src/cryptlib.dsp

# 3. which namespaces / libraries are actually linked
python3 -I third_party/scripts/rtti_namespaces.py third_party/evidence/rtti_namespaces.md \
  "$G/StarFleetOP.exe" reference/SFCOPServer/Assets/StarFleetOP.exe reference/SFCOPServer/ServerPlatform.exe \
  "$G/Assets/Scripts/Bon_Heater.scr" "$G/BugslayerUtil.dll" "$L/cryptlibR.lib"

# 4. WONAPI exe vs archive
python3 -I third_party/scripts/wonapi_compare.py "$G/StarFleetOP.exe" "$(ls -d $W/src/WON/*network*/wonapi)" \
  third_party/wonapi/evidence/exe_vs_madebr_wonapi.md

# 5. GameSpy components
python3 -I third_party/scripts/gamespy_components.py "$G/StarFleetOP.exe" third_party/gamespy/components.md

# 6. middleware imports (needs pefile: python3 -m venv $W/third_party/venv && $W/third_party/venv/bin/pip install pefile)
$W/third_party/venv/bin/python -I third_party/scripts/middleware_imports.py "$G/StarFleetOP.exe" third_party/evidence/middleware_imports.md
```

Rerunning steps 1 and 2 on 2026-11-02 produced byte-identical vendored trees.

## Rules for this directory

- Vendor only code with a usable licence. Leaked code gets headers only, marked "reference" in `PROVENANCE.md`.
- No test suites, binaries, Microsoft headers or Dinkumware headers. Reference those by path:
  - the toolchain tarball at `work/toolchain/dl/msvc6.4.tar.gz`
  - the SDK's `reference/.../OP API - R2.1/STL/Include`
