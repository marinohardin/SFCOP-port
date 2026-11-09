// The DirectPlay 4 values the module speaks (DESIGN §4.3, §5.11, §6.1): result codes, flags,
// player ids, system message types, GUIDs and the host forms of the structures the game passes.
// The numbers are the public DirectPlay definitions; the structures are this module's own, and
// the guest shims (DplayGuest.cpp) read and write the game's layouts field by field.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::dplay {

// An HRESULT as the game reads it from eax.
using HResult = std::uint32_t;
// A player or group id.
using Dpid = std::uint32_t;

// A GUID in its 16-byte memory form (Data1 to Data3 little-endian), as the game stores it.
struct Guid {
    std::array<std::uint8_t, 16> bytes{};

    bool IsNull() const {
        for (std::uint8_t b : bytes) {
            if (b != 0) {
                return false;
            }
        }
        return true;
    }
    friend bool operator==(const Guid &a, const Guid &b) { return a.bytes == b.bytes; }
    friend bool operator!=(const Guid &a, const Guid &b) { return !(a == b); }
    friend bool operator<(const Guid &a, const Guid &b) { return a.bytes < b.bytes; }
};

// A GUID from its registry form's fields.
constexpr Guid makeGuid(std::uint32_t d1, std::uint16_t d2, std::uint16_t d3,
                        std::array<std::uint8_t, 8> d4) {
    Guid g;
    g.bytes[0] = static_cast<std::uint8_t>(d1);
    g.bytes[1] = static_cast<std::uint8_t>(d1 >> 8);
    g.bytes[2] = static_cast<std::uint8_t>(d1 >> 16);
    g.bytes[3] = static_cast<std::uint8_t>(d1 >> 24);
    g.bytes[4] = static_cast<std::uint8_t>(d2);
    g.bytes[5] = static_cast<std::uint8_t>(d2 >> 8);
    g.bytes[6] = static_cast<std::uint8_t>(d3);
    g.bytes[7] = static_cast<std::uint8_t>(d3 >> 8);
    for (int i = 0; i < 8; ++i) {
        g.bytes[8 + i] = d4[i];
    }
    return g;
}

// ---- results ------------------------------------------------------------------------------

constexpr HResult kDpOk = 0x00000000;
constexpr HResult kDpErrAlreadyInitialized = 0x88770005;
constexpr HResult kDpErrAccessDenied = 0x8877000A;
constexpr HResult kDpErrBufferTooSmall = 0x8877001E;
constexpr HResult kDpErrCantAddPlayer = 0x88770028;
constexpr HResult kDpErrCantCreateGroup = 0x88770032;
constexpr HResult kDpErrCantCreatePlayer = 0x8877003C;
constexpr HResult kDpErrCantCreateSession = 0x88770046;
constexpr HResult kDpErrInvalidFlags = 0x88770078;
constexpr HResult kDpErrInvalidObject = 0x88770082;
constexpr HResult kDpErrInvalidPlayer = 0x88770096;
constexpr HResult kDpErrInvalidGroup = 0x8877009B;
constexpr HResult kDpErrNoConnection = 0x887700AA;
constexpr HResult kDpErrNoMessages = 0x887700BE;
constexpr HResult kDpErrNoSessions = 0x887700DC;
constexpr HResult kDpErrSendTooBig = 0x887700E6;
constexpr HResult kDpErrTimeout = 0x887700F0;
constexpr HResult kDpErrUnavailable = 0x887700FA;
constexpr HResult kDpErrSessionLost = 0x88770136;
constexpr HResult kDpErrUninitialized = 0x88770140;
constexpr HResult kDpErrNoNewPlayers = 0x8877014A;
constexpr HResult kDpErrInvalidPassword = 0x88770154;
constexpr HResult kDpErrNotLobbied = 0x8877042E;
constexpr HResult kDpErrInvalidParams = 0x80070057; // E_INVALIDARG
constexpr HResult kDpErrUnsupported = 0x80004001;   // E_NOTIMPL
constexpr HResult kDpErrNoInterface = 0x80004002;   // E_NOINTERFACE
constexpr HResult kDpErrPending = 0x8000000A;       // E_PENDING
constexpr HResult kDpErrGeneric = 0x80004005;       // E_FAIL

// ---- player ids ---------------------------------------------------------------------------

constexpr Dpid kDpidSysMsg = 0;      // DPID_SYSMSG: the sender of a system message
constexpr Dpid kDpidAllPlayers = 0;  // DPID_ALLPLAYERS: every player but the sender
constexpr Dpid kDpidServerPlayer = 1; // DPID_SERVERPLAYER: never given to a player here
constexpr Dpid kDpidUnknown = 0xFFFFFFFF;

// ---- flags --------------------------------------------------------------------------------

// DPSESSIONDESC2.dwFlags
constexpr std::uint32_t kSessionNewPlayersDisabled = 0x00000001;
constexpr std::uint32_t kSessionMigrateHost = 0x00000004;
constexpr std::uint32_t kSessionJoinDisabled = 0x00000020;
constexpr std::uint32_t kSessionKeepAlive = 0x00000040;
constexpr std::uint32_t kSessionPasswordRequired = 0x00000400;
constexpr std::uint32_t kSessionMulticastServer = 0x00000800;
constexpr std::uint32_t kSessionDirectPlayProtocol = 0x00002000;

// Open
constexpr std::uint32_t kOpenJoin = 0x00000001;   // DPOPEN_JOIN
constexpr std::uint32_t kOpenCreate = 0x00000002; // DPOPEN_CREATE
constexpr std::uint32_t kOpenReturnStatus = 0x00000080;

// EnumSessions
constexpr std::uint32_t kEnumSessionsAvailable = 0x00000001;
constexpr std::uint32_t kEnumSessionsAll = 0x00000002;
constexpr std::uint32_t kEnumSessionsAsync = 0x00000010;
constexpr std::uint32_t kEnumSessionsStopAsync = 0x00000020;
constexpr std::uint32_t kEnumSessionsPasswordRequired = 0x00000040;
constexpr std::uint32_t kEnumSessionsReturnStatus = 0x00000080;
// The flag of the enumeration callback's last call: the enumeration has timed out.
constexpr std::uint32_t kEscTimedOut = 0x00000001; // DPESC_TIMEDOUT

// EnumPlayers and EnumGroups; also the flags their callback receives
constexpr std::uint32_t kEnumPlayersLocal = 0x00000008;
constexpr std::uint32_t kEnumPlayersRemote = 0x00000010;
constexpr std::uint32_t kEnumPlayersGroup = 0x00000020;
constexpr std::uint32_t kEnumPlayersSession = 0x00000080;
constexpr std::uint32_t kEnumPlayersSpectator = 0x00000200;

// Player and group flags (CreatePlayer, CreateGroup, GetPlayerFlags)
constexpr std::uint32_t kPlayerSpectator = 0x00000200; // DPPLAYER_SPECTATOR
constexpr std::uint32_t kPlayerLocal = 0x00000008;     // DPPLAYER_LOCAL (GetPlayerFlags)
constexpr std::uint32_t kGroupHidden = 0x00001000;     // DPGROUP_HIDDEN

constexpr std::uint32_t kPlayerTypeGroup = 0;  // DPPLAYERTYPE_GROUP
constexpr std::uint32_t kPlayerTypePlayer = 1; // DPPLAYERTYPE_PLAYER

// Send and SendEx
constexpr std::uint32_t kSendGuaranteed = 0x00000001;
constexpr std::uint32_t kSendSigned = 0x00000020;
constexpr std::uint32_t kSendEncrypted = 0x00000040;
constexpr std::uint32_t kSendAsync = 0x00000200;
constexpr std::uint32_t kSendNoSendCompleteMsg = 0x00000400;

// Receive
constexpr std::uint32_t kReceiveAll = 0x00000001;
constexpr std::uint32_t kReceiveToPlayer = 0x00000002;
constexpr std::uint32_t kReceiveFromPlayer = 0x00000004;
constexpr std::uint32_t kReceivePeek = 0x00000008;

// GetPlayerData and SetPlayerData
constexpr std::uint32_t kGetLocal = 0x00000001; // DPGET_LOCAL (else DPGET_REMOTE)
constexpr std::uint32_t kSetLocal = 0x00000001; // DPSET_LOCAL (else DPSET_REMOTE)
constexpr std::uint32_t kSetGuaranteed = 0x00000002;

// GetCaps
constexpr std::uint32_t kGetCapsGuaranteed = 0x00000001;
constexpr std::uint32_t kCapsIsHost = 0x00000002;
constexpr std::uint32_t kCapsKeepAliveOptimized = 0x00000010;
constexpr std::uint32_t kCapsGuaranteedOptimized = 0x00000020;
constexpr std::uint32_t kCapsGuaranteedSupported = 0x00000040;
constexpr std::uint32_t kCapsAsyncCancelAllSupported = 0x00002000;
constexpr std::uint32_t kCapsAsyncSupported = 0x00010000;

// GetMessageQueue
constexpr std::uint32_t kMessageQueueSend = 0x00000001;
constexpr std::uint32_t kMessageQueueReceive = 0x00000002;

// EnumConnections: the connections DirectPlay itself provides
constexpr std::uint32_t kConnectionDirectPlay = 0x00000001;

// ---- system messages (DPMSG_*.dwType) -----------------------------------------------------

constexpr std::uint32_t kSysAddPlayerToGroup = 0x0002;
constexpr std::uint32_t kSysCreatePlayerOrGroup = 0x0003;
constexpr std::uint32_t kSysDestroyPlayerOrGroup = 0x0005;
constexpr std::uint32_t kSysDeletePlayerFromGroup = 0x0021;
constexpr std::uint32_t kSysSessionLost = 0x0031;
constexpr std::uint32_t kSysHost = 0x0101;
constexpr std::uint32_t kSysSetPlayerOrGroupData = 0x0102;
constexpr std::uint32_t kSysSetPlayerOrGroupName = 0x0103;
constexpr std::uint32_t kSysSetSessionDesc = 0x0104;

// ---- structure sizes the game passes in dwSize ---------------------------------------------

constexpr std::uint32_t kSessionDescBytes = 0x50; // DPSESSIONDESC2
constexpr std::uint32_t kNameBytes = 16;          // DPNAME
constexpr std::uint32_t kCapsBytes = 40;          // DPCAPS

// ---- GUIDs --------------------------------------------------------------------------------

// The TCP/IP service provider, the one connection this module can initialise.
constexpr Guid kSpTcpIp = makeGuid(0x36E95EE0, 0x8577, 0x11CF,
                                   {0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82});
// The modem service provider: listed, as every DirectPlay installation listed it, because the
// game's connection list reads its entry unconditionally (STARFLEETOP 0x00616700); initialising
// it answers DPERR_UNAVAILABLE, as on a machine without a modem.
constexpr Guid kSpModem = makeGuid(0x44EAA760, 0xCB68, 0x11CF,
                                   {0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E});
// Address data types (DPAID_*)
constexpr Guid kAidTotalSize = makeGuid(0x1318F560, 0x912C, 0x11D0,
                                        {0x9D, 0xAA, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB});
constexpr Guid kAidServiceProvider = makeGuid(0x07D916C0, 0xE0AF, 0x11CF,
                                              {0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E});
constexpr Guid kAidINet = makeGuid(0xC4A54DA0, 0xE0AF, 0x11CF,
                                   {0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E});
constexpr Guid kAidINetPort = makeGuid(0xE4524541, 0x8EA5, 0x11D1,
                                       {0x8A, 0x96, 0x00, 0x60, 0x97, 0xB0, 0x14, 0x11});
constexpr Guid kIidIUnknown = makeGuid(0x00000000, 0x0000, 0x0000,
                                       {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46});
constexpr Guid kIidIDirectPlay4A = makeGuid(0x0AB1C531, 0x4745, 0x11D1,
                                            {0xA7, 0xA1, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC});
constexpr Guid kIidIDirectPlayLobby3A = makeGuid(0x2DB72491, 0x652C, 0x11D1,
                                                 {0xA7, 0xA8, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC});

// ---- host forms of the game's structures --------------------------------------------------

// DPNAME: a short and a long name, each absent (a null pointer) or a string.
struct Name {
    std::optional<std::string> shortName;
    std::optional<std::string> longName;

    friend bool operator==(const Name &a, const Name &b) {
        return a.shortName == b.shortName && a.longName == b.longName;
    }
};

// DPSESSIONDESC2, without dwSize and dwReserved1/2.
struct SessionDesc {
    std::uint32_t flags = 0;
    Guid instance;
    Guid application;
    std::uint32_t maxPlayers = 0; // 0: no limit
    std::uint32_t currentPlayers = 0;
    std::optional<std::string> name;
    std::optional<std::string> password;
    std::array<std::uint32_t, 4> user{};
};

// DPCAPS, without dwSize.
struct Caps {
    std::uint32_t flags = 0;
    std::uint32_t maxBufferSize = 0;
    std::uint32_t maxQueueSize = 0;
    std::uint32_t maxPlayers = 0;
    std::uint32_t hundredBaud = 0;
    std::uint32_t latency = 0;
    std::uint32_t maxLocalPlayers = 0;
    std::uint32_t headerLength = 0;
    std::uint32_t timeout = 0;
};

// A structure the game receives in a buffer of its own, laid out in the guest's form: `bytes`
// as they go into the buffer, with a pointer field at each offset in `pointers`. Such a field
// holds an offset from the start of the buffer; the shim adds the buffer's guest address. A
// null pointer is 0 and is not listed.
struct Blob {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> pointers;
};

} // namespace sfcop::hle::dplay
