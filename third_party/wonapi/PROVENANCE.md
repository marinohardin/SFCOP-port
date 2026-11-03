# WONAPI: provenance (reference headers only)

**Status: leaked proprietary source. Only headers are vendored, and only as reference for naming types and
laying out classes during decompilation. Do not compile them into the port, and do not distribute them.**

| | |
|---|---|
| What | WON.net client SDK "WONAPI", second generation. It lives in namespace `WONAPI`, uses its own `WONCrypt` (`BigInteger`, `Blowfish`, `ElGamal`, `MD5Digest`) and has the `AsyncOp`/`Completion` model. It is not the 1999 "Titan" API. |
| Source | `https://github.com/madebr/WON` at commit `7774426f70d7fcf6c8b69d6336ed0438bb5159e6`, directory `" world-opponent-network/wonapi"` (the name starts with a space). Cloned to `work/src/WON`. |
| Licence | **None.** The source files carry no WON or Sierra copyright header and no licence. The repo has no licence (GitHub: `null`) and calls itself an archive. Its README says the server code was received under circumstances that prevent its release, and that the WONAPI comes from the AVP2 and Dark Reign 2 releases. Neither claim could be verified. `WONAPI.plg` records a rebuild from `D:\Sierra\won_dev\Won_src\Complete_wonapi\` on a modern Windows machine, which points to a copy of Sierra's internal tree. Treat it as a leak. |
| Vendored | `include/`: 124 headers, 556 KB. This is the top-level `WONAPI.h`, `WONShared*.h`, `WONStatus.h` and `WONTypes.h`, plus every header of `WONCommon`, `WONSocket`, `WONAuth`, `WONCrypt`, `WONDir` and `WONServer`, plus nine `WONMisc` headers for the ops the exe uses (DetectFirewall, GetMOTD, HTTPGet/HTTP engine/cache/URL, CheckValidVersion, GameSpySupport). No `.cpp`, nothing from GUI, Lobby, Update or Routing, and no third-party code (the GameSpy `gqueryreporting`/`nonport` files are excluded). |
| Not vendored | The `.cpp` sources, which hold the wire-format packing. Read them in `work/src/WON/ world-opponent-network/wonapi/` and do not copy them into the repo. |
| Reproduce | `scripts/fetch_sources.sh && scripts/vendor.sh` |

## Does it match the exe?

Run `scripts/wonapi_compare.py`; the output is in `evidence/exe_vs_madebr_wonapi.md`.

- **Classes.** The exe's RTTI has 50 classes in `WONAPI`, and 49 of them are defined in this tree. Examples:
  `AsyncOp`, `RefCount`, `CompletionBase<SmartPtr<AsyncOp>>`, `ParamCompletion`, `DirEntity`,
  `Auth2Certificate`, `AuthSession`, `PeerAuthOp`, `LightGetCertOp`, `GetDirOp`, `DetectFirewallOp`,
  `GetMOTDOp`, `HTTPGetOp`, `SocketThreadEx`, `QueueSocket`, `BigInteger`, `CryptTransform`. The one class
  missing is `WONAPIPumpThread`. In the archive the pump thread is a member of `WONAPICore` (`mPumpThread`), so
  the class was renamed or folded in later.
- **Status strings.** The exe embeds 302 `WS_*` status names, and 262 of them appear in this tree.
  - The 40 missing names are all `WS_RoutingServ_*`. The tree has `WS_RoutingServG2_*` in their place.
  - The tree adds 35 names the exe lacks, for example `WS_AuthServ_KeyExpired` and `WS_DBProxyServ_*`.
- **CD key registry path.** The exe uses `SOFTWARE\WON\CDKeys`. The tree uses `SOFTWARE\Sierra\CDKeys`
  (`WONAuth/CDKey.cpp`).

**Verdict:** the exe uses the same WONAPI generation as this tree, but a slightly earlier snapshot. That fits a
2001 Taldren build against a 2002-2003 Sierra tree. Class names, member layout and the `WS_*` table are a usable
reference, but check every struct offset against the exe.

## Other WON code examined

- **Homeworld `src/Titan`** (`https://github.com/aheadley/homeworld` at commit `c1e7f492eaea`, sparse-cloned to
  `work/src/homeworld`): the first-generation Titan headers from about 1999.
  - Its `crypt/` uses Crypto++ (`cryptoFiles/nr.h`) and `auth/` holds the Auth1 types. The exe uses none of these
    classes.
  - Its message headers (`msg/Dir`, `msg/Auth`, `msg/Firewall`) document the server message families, which are
    the same on the wire.
  - Licence: the Relic EULA, which allows non-commercial use only and redistribution only to RDN members. The
    GitHub copy was re-posted outside those terms. **Reference only, not vendored.**
- **`TitanApi/`** in the same WON archive is a fuller copy of the Titan generation, with the same leak status and
  the same verdict.

## Use in the port

The port should not ship WONAPI. Two paths:

- **(a)** Keep the decompiled `tWonLogon`/`tWONInterSwitchConnection` code and reimplement the small part of
  WONAPI it calls (directory get, Auth2 certificate, peer auth, firewall detect, MOTD over HTTP) as clean-room
  code. Use these headers for names and the exe for behaviour.
- **(b)** Replace the WON login entirely with the project's own lobby and auth. The server side is
  `FlashZ/won_oss_server` (AGPL, clean-room) or our own.

## Location (changed 2026-11-03 by the integrator)

The 124 reference headers are **not in the repository**. They live outside it at
`$SFCOP_ROOT/work/src/wonapi-headers-ref/` (recreate with
`scripts/fetch_sources.sh && scripts/vendor.sh`, then move `include/` there). Nothing derived from
this source may be committed; the repository keeps only this provenance note, the comparison
evidence and the scripts.
