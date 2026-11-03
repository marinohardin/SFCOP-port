# Research report: matching SDK .lib object code into a stripped MSVC 6 exe

Sub-agent report, 2026-11-02.

## 1. Tooling for lib->exe function matching

**Ghidra Function ID (FID)**: best fit for per-function COMDAT libs with full mangled names.
- Workflow: import every `.lib` member into one Ghidra project (disable FID/Library Identification/Demangler analyzers while ingesting), then Tools -> Function ID -> "Populate FidDb from programs..." with an optional Common Symbols File. https://ghidradocs.com/11.1_PUBLIC/help/FunctionID/help/topics/FunctionID/FunctionIDPlugin.html
- Scripts in `Ghidra/Features/FunctionID/ghidra_scripts`: `ImportMSLibs.java` (walks `.lib` archives, loads each COFF member via `MSCoffLoader`), `CreateMultipleLibraries.java` (expects `root/compiler/project/version/options`), plus `CreateEmptyFidDatabase`, `RemoveFunctions`, `RepackFid`, headless pre/post scripts. https://github.com/NationalSecurityAgency/ghidra/tree/master/Ghidra/Features/FunctionID/ghidra_scripts
- Relocation handling: FID "full hash" covers mnemonics/addressing form but ignores constant values (relocation-tolerant); "specific hash" adds non-address constants. Tiny functions gated by instruction-count threshold.
- Shipped databases built from VS 1998 (=VC6) through 2015 libs (`building_fid.txt`), so the bundled fidb covers the VC6 CRT (reportedly only common-symbol functions; a custom VC6 fidb is the workaround). https://forum.mattkc.com/viewtopic.php?p=2520
- Batch generator: https://github.com/threatrack/ghidra-fid-generator ; prebuilt fidbs https://github.com/threatrack/ghidra-fidb-repo ; vintage x86 scripts https://github.com/moralrecordings/ghidra-fidb-dos-win16
- New (2026): `rsleigh-fid`, FID-compatible fingerprinting in Rust, v0.7.0 2026-10-16 (maturity unverified). https://docs.rs/rsleigh-fid

**IDA FLAIR**: `pcf` on COFF `.lib` -> `.pat`; `sigmake` -> `.sig`. PAT format masks relocation-affected bytes. Hex-Rays bundles VC6 runtime sigs. FLIRT sigs inside Ghidra: https://github.com/nwmonster/applysig

**rizin/radare2**: `generate-obj-from-lib.py`, `generate-pat-from-obj.py`, `rz-sign -c`, applied with `aaF`. https://book.rizin.re/src/analysis/signatures.html

**Binary Ninja**: SigKit deprecated; successor WARP (Aug 2025) accepts `.lib` directly and masks address-bearing instructions. https://binary.ninja/2025/08/22/warp.html

**Diffing route**: link the SDK libs into a dummy exe/DLL with VC6 `link /DEBUG /OPT:NOREF /INCLUDE:...` to get a symbol-rich binary, then BinDiff / Diaphora / Ghidra Version Tracking ("Exact Function Instructions Match" masks operands). https://hex-rays.com/blog/plugin-focus-diaphora ; https://github.com/google/bindiff

## 2. Synthesizing a PDB for the exe

- pdbgen (wandel): Ghidra script + `pdbgen.exe` (LLVM 16); global function symbols and types; WinDbg-tested; needs the PE's CodeView debug-directory entry; WIP. https://github.com/wandel/pdbgen
- FakePDB (Mixaill): IDA only; names/labels, optional LIB files. https://github.com/Mixaill/FakePDB
- `llvm-pdbutil yaml2pdb`: publics were silently dropped until LLVM PR #157362 (merged 2025-09-10); use a newer LLVM.
- microsoft/pdb-rs: read/create/modify PDBs, "not authoritative". https://github.com/microsoft/pdb-rs
- reccmp does not consume a PDB for the original; the PDB belongs to the recompile. A synthesized PDB for StarFleetOP.exe will not plug into reccmp.
- x64dbg: export labels from Ghidra (https://github.com/ykankaya/ghidra_ExportToX64dbg) rather than a PDB.

## 3. Precedents

- Halo 2 (Xbox, x86): `tools/libsig.py` recognises library code by byte signature from XDK `.lib` files. https://github.com/kirklandsig/halo2-decompiled/wiki
- Cxbx-Reloaded XbSymbolDatabase: OOVPA signatures per XDK version. https://github.com/CookiePLMonster/XbSymbolDatabase
- PS1: ghidra_psx_ldr + psx_psyq_signatures from PsyQ `.LIB/.OBJ`, "a critical starting point". https://github.com/lab313ru/psx_psyq_signatures
- GameCube/Wii decomp-toolkit: built-in SDK signature DB. https://github.com/encounter/decomp-toolkit
- Black & White 1: PE split into relocatable objects with a decomp-toolkit fork, objdiff. https://github.com/openblack/bw1-decomp
- FUEL (fmtk): identifies statically linked CRT/DirectX/Bink to exclude it; regenerates import `.lib` via `dumpbin /EXPORTS`. https://github.com/widberg/fmtk/wiki/Decompilation
- Gap: no documented x86 Windows project recovered the game's own symbols from a shipped mod-SDK static library (unverified absence).

## 4. objdiff / reccmp with original .obj as target

- objdiff diffs two relocatable objects; `objdiff.json` units take arbitrary `target_path`/`base_path`, `build_target` defaults false, so a member extracted from `SharedR.lib` can be the target with no build step. README lists x86/x86_64 (PC); backend iced-x86; demangles MSVC. Releases 3.6.0-3.8.3 fixed COFF symbol sorting, MSVC unwind/catch size inference, `$L` labels. Real i386 COFF use: https://github.com/albovsky/nd-blackmoor-decomp/pull/71
- reccmp: binary-vs-binary only.

## 5. Crypto++ identification and 2000-2001 versions

- Crypto++ 3.0 placed all names in namespace `CryptoPP`, so `?...@CryptoPP@@` is definitive. https://github.com/weidai11/cryptopp/blob/master/History.txt
- 4.0 announced 2000-11-06; 4.1 between Nov 2000 and Nov 2001 (Panama MAC, MARC4, SEC 2 curves); 4.2 released 2001-11-05 (MD4, longer HMAC keys). Pin the version from the class inventory (MD4 present => 4.2; Panama MAC => >= 4.1) and reuse that release's source verbatim. Archived VC6 project files exist (vc60.zip).
