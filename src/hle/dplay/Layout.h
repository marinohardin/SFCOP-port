// The structures the game reads from a buffer it gives DirectPlay, laid out as the 32-bit game
// lays them out (public DirectPlay 4 layouts): DPNAME, DPSESSIONDESC2 and the DPMSG_* system
// messages Receive returns. Strings and data the structure points to follow it in the same
// buffer, NUL-terminated, as DirectPlay places them; the pointer fields hold offsets (Blob).
#pragma once

#include "hle/dplay/DplayDefs.h"

#include <cstdint>
#include <vector>

namespace sfcop::hle::dplay {

// DPNAME { dwSize = 16, dwFlags = 0, lpszShortNameA, lpszLongNameA }, then the strings.
Blob layoutName(const Name &name);

// DPSESSIONDESC2 (0x50 bytes; dwSize 0x50, dwReserved1/2 zero), then the strings.
Blob layoutSessionDesc(const SessionDesc &desc);

// DPMSG_CREATEPLAYERORGROUP (48 bytes): dwType, dwPlayerType, dpId, dwCurrentPlayers, lpData,
// dwDataSize, dpnName (16), dpIdParent, dwFlags; then the data and the strings.
Blob layoutCreatePlayerOrGroup(std::uint32_t playerType, Dpid id, std::uint32_t currentPlayers,
                               const std::vector<std::uint8_t> &data, const Name &name,
                               std::uint32_t flags);

// DPMSG_DESTROYPLAYERORGROUP (52 bytes): dwType, dwPlayerType, dpId, lpLocalData,
// dwLocalDataSize, lpRemoteData, dwRemoteDataSize, dpnName (16), dpIdParent, dwFlags.
Blob layoutDestroyPlayerOrGroup(std::uint32_t playerType, Dpid id,
                                const std::vector<std::uint8_t> &localData,
                                const std::vector<std::uint8_t> &remoteData, const Name &name,
                                std::uint32_t flags);

// DPMSG_ADDPLAYERTOGROUP and DPMSG_DELETEPLAYERFROMGROUP (12 bytes): dwType, dpIdGroup,
// dpIdPlayer.
Blob layoutGroupMembership(std::uint32_t type, Dpid group, Dpid player);

// DPMSG_SETPLAYERORGROUPDATA (20 bytes): dwType, dwPlayerType, dpId, lpData, dwDataSize.
Blob layoutSetPlayerOrGroupData(std::uint32_t playerType, Dpid id,
                                const std::vector<std::uint8_t> &data);

// DPMSG_SETPLAYERORGROUPNAME (28 bytes): dwType, dwPlayerType, dpId, dpnName (16).
Blob layoutSetPlayerOrGroupName(std::uint32_t playerType, Dpid id, const Name &name);

// DPMSG_SETSESSIONDESC (84 bytes): dwType, then the DPSESSIONDESC2.
Blob layoutSetSessionDesc(const SessionDesc &desc);

// A message that is its dwType only: DPMSG_SESSIONLOST, DPMSG_HOST.
Blob layoutTypeOnly(std::uint32_t type);

} // namespace sfcop::hle::dplay
