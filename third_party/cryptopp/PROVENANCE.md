# Crypto++ 3.1: provenance

| | |
|---|---|
| Version | **3.1** (`readme.txt`: "Version 3.1  4/27/1999") |
| Source | `https://cryptopp.com/crypto31.zip`. The origin timed out on 2026-11-02, so the Wayback copy `https://web.archive.org/web/20240714225437id_/https://cryptopp.com/crypto31.zip` was used. |
| SHA-256 of zip | `0d4eb329cd59cac3c7f3a992ecab16b279749a902d4b10e3ebf4e949c4e14f5b` (430,747 bytes) |
| Licence | `src/license.txt`: "Compilation Copyright (c) 1995-1999 by Wei Dai". Most modules are public domain. Thirteen modules keep their authors' copyright with permission to use, modify and distribute (cast, haval, idea, mars, md2, md5/md5mac, rijndael, serpent, sha, twofish, zbits/ztrees/zdeflate). The licence requires keeping the notice and has a patent caveat (IDEA, RC5, RC6, MARS). There is no copyleft, so vendoring is fine. |
| Vendored | `src/`: 90 library `.cpp` files plus headers, `license.txt`, `readme.txt`, `cryptlib.dsp`, `cryptest.dsp/.dsw` and `Makefile`. Left out: the test driver (`test.cpp`, `bench.*`, `validat*.cpp`, `validate.h`) and the 58 `.dat` test-vector files. |
| Reproduce | `scripts/fetch_sources.sh && scripts/vendor.sh` |

## Version evidence

The version was pinned from `OP API - R2.1/Scripts/Libraries/cryptlibR.lib`, the SDK's Crypto++ build dated 2001-06-16.
`scripts/cryptopp_inventory.py` writes `evidence/cryptlib_version_markers.md` and `evidence/cryptlib_members.tsv`.

1. **Build path.** The lib's anonymous-namespace symbols embed
   `D:\Work\Sfc\crypto31\{ecp,mars,oaep,seal,secshare,twofish}.cpp`, so Taldren built it from a folder named `crypto31`.
2. **Object list.** The lib has 90 object files. They match the 90 `.cpp` sources listed in 3.1's `cryptlib.dsp`
   exactly, with none missing and none extra.
3. **Feature introductions** from `History.txt`:
   - Present: everything that 3.1 added (ARC4, Rijndael, Twofish, Serpent). DMAC and CBC-MAC are header-only
     templates with no instances, as expected.
   - Present: the pre-3.2 name `TripleDES`.
   - Absent: everything that 3.2 added or renamed (DES_EDE2/DES_EDE3, DES_XEX3, ECDSA,
     DefaultEncryptorWithMAC).
   - Absent: everything from 4.0 (Skipjack, CAST-256, Panama, SHA-2, XTR, sockets), 4.1 (MARC4), 4.2 (MD4) and
     5.0 (ESIGN).
4. **Headers.** The SDK ships three Crypto++ headers in `Scripts/shared/Source/Taldren/Common/Crypto/`:
   `cryptlib.h`, `crc.h` and `config.h`. Taldren added `_func_start(...)` / `_func_end` call-trace macros to them.
   With those macros stripped, all three are identical to the 3.1 release.

## Is it in the game?

**No.** Crypto++ code is linked into **none** of the shipped binaries. `../evidence/rtti_namespaces.md`
(script `scripts/rtti_namespaces.py`) shows:

- The game binaries have zero `@CryptoPP@@` RTTI type descriptors and zero `CryptoPP` strings. This holds for
  `StarFleetOP.exe` 2.5.5.2, the 2.5.0.0 copy in the server kit, `ServerPlatform.exe`, and the 90 mission
  scripts.
- The lib itself does have such descriptors (18). The game binaries are built with `/GR` (2,096 type descriptors
  in the exe), so a linked polymorphic Crypto++ class would leave a type descriptor behind.
- The game does its own CRC (`tCRC32` in `Common/Crypto/CRC32.cpp`, polynomial 0x04C11DB7). WONAPI has its own
  crypto (`WONAPI::BigInteger`, Blowfish, ElGamal, MD5; see `../wonapi/`).
- The 32 exe addresses that `analysis/scripts/libmatch.py` attributes to `cryptlibR.lib` are relocation-masked
  look-alikes. Examples are `std::vector<Integer>::insert` and 16-byte accessors. They should not be treated as
  Crypto++ code. (Reported to P0-3 and P0-7.)

The script templates still link `cryptlibR.lib`, presumably because `SharedR` headers pull in `Common/Crypto`.
The linker drops all of it.

## Use in the port

No Crypto++ code is needed to rebuild or run the game. This tree is kept for two reasons:

- To byte-match or rebuild `cryptlibR.lib` itself, if a script or tool ever turns out to need it.
- To make clear that the Crypto++ names in the SDK headers are 3.1.

The original `cryptlib.dsp` Release flags are `/MT /W3 /GX /O2 /D NDEBUG /D WIN32 /D _WINDOWS /D USE_PRECOMPILED_HEADERS /Yu"pch.h"`.
Whether Taldren changed these flags is not known. The SDK's own `.dsp` files use `/GR`, and the lib's type
descriptors could equally come from exception throw-info, which does not need `/GR`. To rebuild, also apply the
trace macros (`_func_start`/`_func_end`) the way Taldren's headers do.
