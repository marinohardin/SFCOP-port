// The guest shims of the DirectPlay module (src/hle/README.md, "Implementing a module"): every
// method of IDirectPlay4A and IDirectPlayLobby3A, one shim per row of src/hle/surface.tsv, in
// pcrecomp's win32hle form. Each reads its arguments from the guest stack, reads the guest's
// structures field by field (DPSESSIONDESC2, DPNAME, DPCAPS, DPCOMPOUNDADDRESSELEMENT), calls the
// module, writes what it answers back into the guest's buffers, and purges the stack its row's
// stack_bytes give. Methods the game never calls answer E_NOTIMPL.
//
// The game makes both objects with CoCreateInstance (analysis/netcode/directplay.md); DirectPlay
// is not an import. This file gives win32hle's CoCreateInstance the two classes when it is
// loaded (hle_com_register_class, below), so it still exports only sfcop_hle_dplay[]. A guest
// object is its vtable, a count at +4 that is not used, and the module object it stands for.
//
// Waits on the game's thread give the machine up (hle_block_begin). Callbacks into the game
// (EnumSessions, EnumPlayers, EnumConnections, EnumAddress) run on the caller's thread with the
// structures they are handed in this thread's scratch.
extern "C" {
#include "win32hle.h"
}

#include "hle/com/IDirectPlay4A.h"
#include "hle/com/IDirectPlayLobby3A.h"
#include "hle/dplay/Address.h"
#include "hle/dplay/DirectPlay.h"
#include "hle/dplay/DplayRuntime.h"
#include "hle/dplay/Layout.h"
#include "hle/dplay/Lobby.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>

namespace sfcop::hle::dplay {
namespace {

namespace com = sfcop::hle::com;

constexpr std::uint32_t kClassNotRegistered = 0x80040154; // REGDB_E_CLASSNOTREG
constexpr std::uint32_t kObjectBytes = 8;                 // vtable, count
constexpr std::size_t kMaxString = 4096;                  // a name or a password
constexpr std::uint32_t kMaxAddressBytes = 0x10000;
constexpr std::uint32_t kMaxAddressElements = 64;
constexpr std::uint32_t kElementBytes = 24; // DPCOMPOUNDADDRESSELEMENT

std::recursive_mutex g_lock; // the two maps only; never held while the module runs
std::map<std::uint32_t, std::unique_ptr<DirectPlay>> g_players; // by guest object
std::map<std::uint32_t, std::unique_ptr<Lobby>> g_lobbies;       // by guest object
std::uint32_t g_vtPlay, g_vtLobby;

void buildVtables();

DirectPlay *playOf(std::uint32_t self) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    auto found = g_players.find(self);
    return found == g_players.end() ? nullptr : found->second.get();
}
Lobby *lobbyOf(std::uint32_t self) {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    auto found = g_lobbies.find(self);
    return found == g_lobbies.end() ? nullptr : found->second.get();
}

#define SFCOP_DP_OR_FAIL(argc)                                                                 \
    DirectPlay *dp = playOf(A32(0));                                                           \
    if (dp == nullptr) {                                                                       \
        RET(kDpErrInvalidObject, argc);                                                        \
    }
#define SFCOP_LOBBY_OR_FAIL(argc)                                                              \
    Lobby *lobby = lobbyOf(A32(0));                                                            \
    if (lobby == nullptr) {                                                                    \
        RET(kDpErrInvalidObject, argc);                                                        \
    }

// ---- guest memory ------------------------------------------------------------------------

bool guidIs(const void *guid, const void *expected) {
    return guid != nullptr && std::memcmp(guid, expected, 16) == 0;
}
Guid guidAt(std::uint32_t va) {
    Guid g;
    if (va != 0) {
        std::memcpy(g.bytes.data(), hle_ptr(va), 16);
    }
    return g;
}
std::uint32_t scratchGuid(const Guid &g) { return hle_scratch_copy(g.bytes.data(), 16); }
std::optional<std::string> stringAt(std::uint32_t va) {
    if (va == 0) {
        return std::nullopt;
    }
    const char *p = static_cast<const char *>(hle_ptr(va));
    return std::string(p, strnlen(p, kMaxString));
}
std::vector<std::uint8_t> bytesAt(std::uint32_t va, std::uint32_t n) {
    if (va == 0 || n == 0) {
        return {};
    }
    const auto *p = static_cast<const std::uint8_t *>(hle_ptr(va));
    return std::vector<std::uint8_t>(p, p + n);
}
// DPNAME: dwSize, dwFlags, lpszShortNameA, lpszLongNameA. A null DPNAME is no name.
Name nameAt(std::uint32_t va) {
    Name n;
    if (va != 0) {
        n.shortName = stringAt(MEM32(va + 8));
        n.longName = stringAt(MEM32(va + 12));
    }
    return n;
}
// DPSESSIONDESC2 (Layout.h has the offsets).
SessionDesc descAt(std::uint32_t va) {
    SessionDesc d;
    d.flags = MEM32(va + 4);
    d.instance = guidAt(va + 8);
    d.application = guidAt(va + 24);
    d.maxPlayers = MEM32(va + 40);
    d.currentPlayers = MEM32(va + 44);
    d.name = stringAt(MEM32(va + 48));
    d.password = stringAt(MEM32(va + 52));
    for (std::uint32_t i = 0; i < 4; ++i) {
        d.user[i] = MEM32(va + 64 + 4 * i);
    }
    return d;
}
// The size of the address at `va`, from its leading DPAID_TotalSize chunk; 0 if it has none.
std::uint32_t addressBytesAt(std::uint32_t va) {
    if (va == 0 || !guidIs(hle_ptr(va), kAidTotalSize.bytes.data()) || MEM32(va + 16) != 4) {
        return 0;
    }
    std::uint32_t total = MEM32(va + 20);
    return total >= 24 && total <= kMaxAddressBytes ? total : 0;
}
// Copies a Blob to `va`, turning its pointer fields' offsets into guest addresses.
void putBlob(std::uint32_t va, const Blob &blob) {
    if (blob.bytes.empty()) {
        return;
    }
    std::memcpy(hle_ptr(va), blob.bytes.data(), blob.bytes.size());
    for (std::uint32_t at : blob.pointers) {
        MEM32(va + at) += va;
    }
}
std::uint32_t scratchBlob(const Blob &blob) {
    std::uint32_t va = hle_scratch(static_cast<std::uint32_t>(blob.bytes.size()) + 1);
    putBlob(va, blob);
    return va;
}
// The size-query convention: room in *size, else the size needed and DPERR_BUFFERTOOSMALL.
std::uint32_t answer(const Blob &blob, std::uint32_t data, std::uint32_t sizeVa) {
    if (sizeVa == 0) {
        return kDpErrInvalidParams;
    }
    std::uint32_t needed = static_cast<std::uint32_t>(blob.bytes.size());
    if (data == 0 || MEM32(sizeVa) < needed) {
        MEM32(sizeVa) = needed;
        return kDpErrBufferTooSmall;
    }
    putBlob(data, blob);
    MEM32(sizeVa) = needed;
    return kDpOk;
}
std::uint32_t answerBytes(const std::vector<std::uint8_t> &bytes, std::uint32_t data,
                          std::uint32_t sizeVa) {
    Blob blob;
    blob.bytes = bytes;
    return answer(blob, data, sizeVa);
}
std::uint32_t callGuest(std::uint32_t fn, std::initializer_list<std::uint32_t> args) {
    std::vector<std::uint32_t> list(args);
    return hle_call_guest(fn, static_cast<int>(list.size()), list.data());
}

// ---- objects -----------------------------------------------------------------------------

Environment guestEnvironment(const Platform &platform) {
    platform::Clock *clock = platform.clock;
    Environment env = environmentFor(
        platform,
        [clock](std::uint32_t ms) {
            hle_block_begin();
            (void)clock->Sleep(ms);
            hle_block_end();
        },
        [](std::uint32_t handle) {
            mach_enter();
            hle_set_event(handle);
            mach_leave();
        });
    // Release() deletes the object on the game's thread; the service thread's signal needs the
    // machine that thread holds, so the join happens with the machine given up.
    env.blocking = [](const std::function<void()> &work) {
        hle_block_begin();
        work();
        hle_block_end();
    };
    return env;
}

// CoCreateInstance(CLSID_DirectPlay, ..., IID_IDirectPlay4A, out). Without a Transport the
// class is not there, as on a machine without DirectPlay.
std::uint32_t createDirectPlay(const std::uint8_t *iid, std::uint32_t *out) {
    if (out != nullptr) {
        *out = 0;
    }
    if (!guidIs(iid, &com::kIID_IDirectPlay4A) && !guidIs(iid, kIidIUnknown.bytes.data())) {
        return kDpErrNoInterface;
    }
    Config config;
    std::random_device random;
    config.seed = (static_cast<std::uint64_t>(random()) << 32) ^ random() ^ hle_ticks_ms();
    std::unique_ptr<DirectPlay> made = hle::dplay::createDirectPlay(currentPlatform(),
                                                                    guestEnvironment, config);
    if (!made) {
        return kClassNotRegistered;
    }
    buildVtables();
    std::uint32_t obj = hle_com_new(g_vtPlay, kObjectBytes);
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        g_players[obj] = std::move(made);
    }
    if (out != nullptr) {
        *out = obj;
    }
    return kDpOk;
}

// CoCreateInstance(CLSID_DirectPlayLobby, ..., IID_IDirectPlayLobby3A, out).
std::uint32_t createLobby(const std::uint8_t *iid, std::uint32_t *out) {
    if (out != nullptr) {
        *out = 0;
    }
    if (!guidIs(iid, &com::kIID_IDirectPlayLobby3A) && !guidIs(iid, kIidIUnknown.bytes.data())) {
        return kDpErrNoInterface;
    }
    buildVtables();
    std::uint32_t obj = hle_com_new(g_vtLobby, kObjectBytes);
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        g_lobbies[obj] = std::make_unique<Lobby>();
    }
    if (out != nullptr) {
        *out = obj;
    }
    return kDpOk;
}

// Registered when the program is loaded; win32hle's class table is zero-initialised storage.
struct RegisterClasses {
    RegisterClasses() {
        hle_com_register_class(reinterpret_cast<const std::uint8_t *>(&com::kCLSID_DirectPlay),
                               createDirectPlay);
        hle_com_register_class(
            reinterpret_cast<const std::uint8_t *>(&com::kCLSID_DirectPlayLobby), createLobby);
    }
};
const RegisterClasses g_registerClasses;

// ---- IDirectPlay4A -----------------------------------------------------------------------

void p_QueryInterface() { // (this, riid, ppv)
    SFCOP_DP_OR_FAIL(3);
    std::uint32_t out = A32(2);
    if (out == 0) {
        RET(kDpErrInvalidParams, 3);
    }
    const void *iid = APTR(1);
    if (guidIs(iid, &com::kIID_IDirectPlay4A) || guidIs(iid, kIidIUnknown.bytes.data())) {
        dp->AddRef();
        MEM32(out) = A32(0);
        RET(kDpOk, 3);
    }
    MEM32(out) = 0;
    RET(kDpErrNoInterface, 3);
}
void p_AddRef() { // (this)
    SFCOP_DP_OR_FAIL(1);
    RET(dp->AddRef(), 1);
}
void p_Release() { // (this)
    SFCOP_DP_OR_FAIL(1);
    std::uint32_t left = dp->Release();
    if (left == 0) {
        std::unique_ptr<DirectPlay> gone;
        {
            std::lock_guard<std::recursive_mutex> guard(g_lock);
            auto found = g_players.find(A32(0));
            gone = std::move(found->second);
            g_players.erase(found);
        }
        gone.reset();
        hle_com_free(A32(0));
    }
    RET(left, 1);
}
void p_AddPlayerToGroup() { // (this, idGroup, idPlayer)
    SFCOP_DP_OR_FAIL(3);
    RET(dp->AddPlayerToGroup(A32(1), A32(2)), 3);
}
void p_Close() { // (this)
    SFCOP_DP_OR_FAIL(1);
    RET(dp->Close(), 1);
}
void p_CreateGroup() { // (this, lpidGroup, lpGroupName, lpData, dwDataSize, dwFlags)
    SFCOP_DP_OR_FAIL(6);
    if (A32(1) == 0) {
        RET(kDpErrInvalidParams, 6);
    }
    Dpid id = 0;
    HResult hr = dp->CreateGroup(&id, nameAt(A32(2)), bytesAt(A32(3), A32(4)), A32(5));
    if (hr == kDpOk) {
        MEM32(A32(1)) = id;
    }
    RET(hr, 6);
}
void p_CreatePlayer() { // (this, lpidPlayer, lpPlayerName, hEvent, lpData, dwDataSize, dwFlags)
    SFCOP_DP_OR_FAIL(7);
    if (A32(1) == 0) {
        RET(kDpErrInvalidParams, 7);
    }
    Dpid id = 0;
    HResult hr =
        dp->CreatePlayer(&id, nameAt(A32(2)), A32(3), bytesAt(A32(4), A32(5)), A32(6));
    if (hr == kDpOk) {
        MEM32(A32(1)) = id;
    }
    RET(hr, 7);
}
void p_DeletePlayerFromGroup() { // (this, idGroup, idPlayer)
    SFCOP_DP_OR_FAIL(3);
    RET(dp->DeletePlayerFromGroup(A32(1), A32(2)), 3);
}
void p_DestroyGroup() { // (this, idGroup)
    SFCOP_DP_OR_FAIL(2);
    RET(dp->DestroyGroup(A32(1)), 2);
}
void p_DestroyPlayer() { // (this, idPlayer)
    SFCOP_DP_OR_FAIL(2);
    RET(dp->DestroyPlayer(A32(1)), 2);
}
// EnumPlayersCallback2(dpId, dwPlayerType, lpName, dwFlags, lpContext)
PlayerCallback playerCallback(std::uint32_t fn, std::uint32_t context) {
    return [fn, context](const PlayerEntry &p) {
        std::uint32_t mark = hle_scratch_mark();
        std::uint32_t name = scratchBlob(layoutName(p.name));
        std::uint32_t go = callGuest(fn, {p.id, p.playerType, name, p.flags, context});
        hle_scratch_release(mark);
        return go != 0;
    };
}
void p_EnumGroups() { // (this, lpguidInstance, lpEnumPlayersCallback2, lpContext, dwFlags)
    SFCOP_DP_OR_FAIL(5);
    if (A32(2) == 0) {
        RET(kDpErrInvalidParams, 5);
    }
    Guid instance = guidAt(A32(1));
    RET(dp->EnumGroups(A32(1) != 0 ? &instance : nullptr, playerCallback(A32(2), A32(3)), A32(4)),
        5);
}
void p_EnumPlayers() { // (this, lpguidInstance, lpEnumPlayersCallback2, lpContext, dwFlags)
    SFCOP_DP_OR_FAIL(5);
    if (A32(2) == 0) {
        RET(kDpErrInvalidParams, 5);
    }
    Guid instance = guidAt(A32(1));
    RET(dp->EnumPlayers(A32(1) != 0 ? &instance : nullptr, playerCallback(A32(2), A32(3)),
                        A32(4)),
        5);
}
// (this, lpsd, dwTimeout, lpEnumSessionsCallback2, lpContext, dwFlags); the callback is
// (lpThisSD, lpdwTimeOut, dwFlags, lpContext).
void p_EnumSessions() {
    SFCOP_DP_OR_FAIL(6);
    std::uint32_t flags = A32(5);
    if (flags & kEnumSessionsStopAsync) {
        RET(dp->EnumSessions(SessionDesc{}, 0, nullptr, flags), 6);
    }
    if (A32(1) == 0 || MEM32(A32(1)) != kSessionDescBytes || A32(3) == 0) {
        RET(kDpErrInvalidParams, 6);
    }
    std::uint32_t fn = A32(3), context = A32(4);
    SessionCallback callback = [fn, context](const SessionDesc *s, std::uint32_t &timeout,
                                             std::uint32_t escFlags) {
        std::uint32_t mark = hle_scratch_mark();
        std::uint32_t desc = s != nullptr ? scratchBlob(layoutSessionDesc(*s)) : 0;
        std::uint32_t timeoutVa = hle_scratch(4);
        MEM32(timeoutVa) = timeout;
        std::uint32_t go = callGuest(fn, {desc, timeoutVa, escFlags, context});
        timeout = MEM32(timeoutVa);
        hle_scratch_release(mark);
        return go != 0;
    };
    RET(dp->EnumSessions(descAt(A32(1)), A32(2), callback, flags), 6);
}
void p_GetCaps() { // (this, lpDPCaps, dwFlags)
    SFCOP_DP_OR_FAIL(3);
    std::uint32_t va = A32(1);
    if (va == 0 || MEM32(va) != kCapsBytes) {
        RET(kDpErrInvalidParams, 3);
    }
    Caps c;
    HResult hr = dp->GetCaps(&c, A32(2));
    if (hr == kDpOk) {
        MEM32(va + 4) = c.flags;
        MEM32(va + 8) = c.maxBufferSize;
        MEM32(va + 12) = c.maxQueueSize;
        MEM32(va + 16) = c.maxPlayers;
        MEM32(va + 20) = c.hundredBaud;
        MEM32(va + 24) = c.latency;
        MEM32(va + 28) = c.maxLocalPlayers;
        MEM32(va + 32) = c.headerLength;
        MEM32(va + 36) = c.timeout;
    }
    RET(hr, 3);
}
void p_GetMessageCount() { // (this, idPlayer, lpdwCount)
    SFCOP_DP_OR_FAIL(3);
    if (A32(2) == 0) {
        RET(kDpErrInvalidParams, 3);
    }
    std::uint32_t count = 0;
    HResult hr = dp->GetMessageCount(A32(1), &count);
    if (hr == kDpOk) {
        MEM32(A32(2)) = count;
    }
    RET(hr, 3);
}
void p_GetPlayerAddress() { // (this, idPlayer, lpData, lpdwDataSize)
    SFCOP_DP_OR_FAIL(4);
    std::vector<std::uint8_t> address;
    HResult hr = dp->GetPlayerAddress(A32(1), &address);
    if (hr != kDpOk) {
        RET(hr, 4);
    }
    RET(answerBytes(address, A32(2), A32(3)), 4);
}
void p_GetPlayerData() { // (this, idPlayer, lpData, lpdwDataSize, dwFlags)
    SFCOP_DP_OR_FAIL(5);
    std::vector<std::uint8_t> data;
    HResult hr = dp->GetPlayerData(A32(1), &data, A32(4));
    if (hr != kDpOk) {
        RET(hr, 5);
    }
    RET(answerBytes(data, A32(2), A32(3)), 5);
}
void p_GetPlayerName() { // (this, idPlayer, lpData, lpdwDataSize)
    SFCOP_DP_OR_FAIL(4);
    Name name;
    HResult hr = dp->GetPlayerName(A32(1), &name);
    if (hr != kDpOk) {
        RET(hr, 4);
    }
    RET(answer(layoutName(name), A32(2), A32(3)), 4);
}
void p_GetSessionDesc() { // (this, lpData, lpdwDataSize)
    SFCOP_DP_OR_FAIL(3);
    SessionDesc d;
    HResult hr = dp->GetSessionDesc(&d);
    if (hr != kDpOk) {
        RET(hr, 3);
    }
    RET(answer(layoutSessionDesc(d), A32(1), A32(2)), 3);
}
void p_Initialize() { // (this, lpGUID): obsolete; a created object is initialised already
    SFCOP_DP_OR_FAIL(2);
    RET(kDpErrAlreadyInitialized, 2);
}
void p_Open() { // (this, lpsd, dwFlags)
    SFCOP_DP_OR_FAIL(3);
    if (A32(1) == 0 || MEM32(A32(1)) != kSessionDescBytes) {
        RET(kDpErrInvalidParams, 3);
    }
    RET(dp->Open(descAt(A32(1)), A32(2)), 3);
}
void p_Receive() { // (this, lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize)
    SFCOP_DP_OR_FAIL(6);
    std::uint32_t fromVa = A32(1), toVa = A32(2), data = A32(4), sizeVa = A32(5);
    if (fromVa == 0 || toVa == 0 || sizeVa == 0) {
        RET(kDpErrInvalidParams, 6);
    }
    Dpid from = MEM32(fromVa), to = MEM32(toVa);
    std::uint32_t size = data != 0 ? MEM32(sizeVa) : 0;
    ReceivedMessage m;
    HResult hr = dp->Receive(&from, &to, A32(3), &size, &m);
    if (hr == kDpErrBufferTooSmall) {
        MEM32(sizeVa) = size;
    } else if (hr == kDpOk) {
        putBlob(data, m.blob);
        MEM32(fromVa) = from;
        MEM32(toVa) = to;
        MEM32(sizeVa) = size;
    }
    RET(hr, 6);
}
void p_Send() { // (this, idFrom, idTo, dwFlags, lpData, dwDataSize)
    SFCOP_DP_OR_FAIL(6);
    const auto *data = static_cast<const std::uint8_t *>(APTR(4));
    RET(dp->Send(A32(1), A32(2), A32(3), data, A32(5)), 6);
}
void p_SetPlayerData() { // (this, idPlayer, lpData, dwDataSize, dwFlags)
    SFCOP_DP_OR_FAIL(5);
    if (A32(2) == 0 && A32(3) != 0) {
        RET(kDpErrInvalidParams, 5);
    }
    RET(dp->SetPlayerData(A32(1), bytesAt(A32(2), A32(3)), A32(4)), 5);
}
void p_SetPlayerName() { // (this, idPlayer, lpPlayerName, dwFlags)
    SFCOP_DP_OR_FAIL(4);
    RET(dp->SetPlayerName(A32(1), nameAt(A32(2)), A32(3)), 4);
}
void p_SetSessionDesc() { // (this, lpSessDesc, dwFlags)
    SFCOP_DP_OR_FAIL(3);
    if (A32(1) == 0 || MEM32(A32(1)) != kSessionDescBytes) {
        RET(kDpErrInvalidParams, 3);
    }
    RET(dp->SetSessionDesc(descAt(A32(1)), A32(2)), 3);
}
// (this, lpguidApplication, lpEnumCallback, lpContext, dwFlags); the callback is
// (lpguidSP, lpConnection, dwConnectionSize, lpName, dwFlags, lpContext).
void p_EnumConnections() {
    SFCOP_DP_OR_FAIL(5);
    if (A32(2) == 0) {
        RET(kDpErrInvalidParams, 5);
    }
    std::uint32_t fn = A32(2), context = A32(3);
    Guid application = guidAt(A32(1));
    ConnectionCallback callback = [fn, context](const Connection &c) {
        std::uint32_t mark = hle_scratch_mark();
        std::uint32_t sp = scratchGuid(c.serviceProvider);
        std::uint32_t size = static_cast<std::uint32_t>(c.address.size());
        std::uint32_t address = hle_scratch_copy(c.address.data(), size);
        std::uint32_t name = scratchBlob(layoutName(c.name));
        std::uint32_t go = callGuest(fn, {sp, address, size, name, c.flags, context});
        hle_scratch_release(mark);
        return go != 0;
    };
    RET(dp->EnumConnections(A32(1) != 0 ? &application : nullptr, callback, A32(4)), 5);
}
void p_InitializeConnection() { // (this, lpConnection, dwFlags)
    SFCOP_DP_OR_FAIL(3);
    std::uint32_t va = A32(1);
    std::uint32_t bytes = addressBytesAt(va);
    const auto *address = bytes != 0 ? static_cast<const std::uint8_t *>(hle_ptr(va)) : nullptr;
    RET(dp->InitializeConnection(address, bytes, A32(2)), 3);
}
void p_GetPlayerFlags() { // (this, idPlayer, lpdwFlags)
    SFCOP_DP_OR_FAIL(3);
    if (A32(2) == 0) {
        RET(kDpErrInvalidParams, 3);
    }
    std::uint32_t flags = 0;
    HResult hr = dp->GetPlayerFlags(A32(1), &flags);
    if (hr == kDpOk) {
        MEM32(A32(2)) = flags;
    }
    RET(hr, 3);
}
// (this, idFrom, idTo, dwFlags, lpData, dwDataSize, dwPriority, dwTimeout, lpContext,
// lpdwMsgID)
void p_SendEx() {
    SFCOP_DP_OR_FAIL(10);
    const auto *data = static_cast<const std::uint8_t *>(APTR(4));
    std::uint32_t id = 0;
    HResult hr = dp->SendEx(A32(1), A32(2), A32(3), data, A32(5), A32(6), A32(7), &id);
    if (A32(9) != 0) {
        MEM32(A32(9)) = id;
    }
    RET(hr, 10);
}
void p_GetMessageQueue() { // (this, idFrom, idTo, dwFlags, lpdwNumMsgs, lpdwNumBytes)
    SFCOP_DP_OR_FAIL(6);
    std::uint32_t messages = 0, bytes = 0;
    HResult hr = dp->GetMessageQueue(A32(1), A32(2), A32(3), &messages, &bytes);
    if (hr == kDpOk) {
        if (A32(4) != 0) {
            MEM32(A32(4)) = messages;
        }
        if (A32(5) != 0) {
            MEM32(A32(5)) = bytes;
        }
    }
    RET(hr, 6);
}
void p_CancelMessage() { // (this, dwMsgID, dwFlags)
    SFCOP_DP_OR_FAIL(3);
    RET(dp->CancelMessage(A32(1), A32(2)), 3);
}

// The methods the game never calls (analysis/netcode/directplay.md).
void p_EnumGroupPlayers() {
    RET(kDpErrUnsupported, 6);
}
void p_GetGroupData() {
    RET(kDpErrUnsupported, 5);
}
void p_GetGroupName() {
    RET(kDpErrUnsupported, 4);
}
void p_GetPlayerCaps() {
    RET(kDpErrUnsupported, 4);
}
void p_SetGroupData() {
    RET(kDpErrUnsupported, 5);
}
void p_SetGroupName() {
    RET(kDpErrUnsupported, 4);
}
void p_AddGroupToGroup() {
    RET(kDpErrUnsupported, 3);
}
void p_CreateGroupInGroup() {
    RET(kDpErrUnsupported, 7);
}
void p_DeleteGroupFromGroup() {
    RET(kDpErrUnsupported, 3);
}
void p_EnumGroupsInGroup() {
    RET(kDpErrUnsupported, 6);
}
void p_GetGroupConnectionSettings() {
    RET(kDpErrUnsupported, 5);
}
void p_SecureOpen() {
    RET(kDpErrUnsupported, 5);
}
void p_SendChatMessage() {
    RET(kDpErrUnsupported, 5);
}
void p_SetGroupConnectionSettings() {
    RET(kDpErrUnsupported, 4);
}
void p_StartSession() {
    RET(kDpErrUnsupported, 3);
}
void p_GetGroupFlags() {
    RET(kDpErrUnsupported, 3);
}
void p_GetGroupParent() {
    RET(kDpErrUnsupported, 3);
}
void p_GetPlayerAccount() {
    RET(kDpErrUnsupported, 5);
}
void p_GetGroupOwner() {
    RET(kDpErrUnsupported, 3);
}
void p_SetGroupOwner() {
    RET(kDpErrUnsupported, 3);
}
void p_CancelPriority() {
    RET(kDpErrUnsupported, 4);
}

// ---- IDirectPlayLobby3A ------------------------------------------------------------------

void l_QueryInterface() { // (this, riid, ppv)
    SFCOP_LOBBY_OR_FAIL(3);
    std::uint32_t out = A32(2);
    if (out == 0) {
        RET(kDpErrInvalidParams, 3);
    }
    const void *iid = APTR(1);
    if (guidIs(iid, &com::kIID_IDirectPlayLobby3A) || guidIs(iid, kIidIUnknown.bytes.data())) {
        lobby->AddRef();
        MEM32(out) = A32(0);
        RET(kDpOk, 3);
    }
    MEM32(out) = 0;
    RET(kDpErrNoInterface, 3);
}
void l_AddRef() { // (this)
    SFCOP_LOBBY_OR_FAIL(1);
    RET(lobby->AddRef(), 1);
}
void l_Release() { // (this)
    SFCOP_LOBBY_OR_FAIL(1);
    std::uint32_t left = lobby->Release();
    if (left == 0) {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        g_lobbies.erase(A32(0));
        hle_com_free(A32(0));
    }
    RET(left, 1);
}
void l_Connect() { // (this, dwFlags, lplpDP, pUnk)
    SFCOP_LOBBY_OR_FAIL(4);
    if (A32(2) != 0) {
        MEM32(A32(2)) = 0;
    }
    RET(lobby->Connect(), 4);
}
// (this, guidSP, guidDataType, lpData, dwDataSize, lpAddress, lpdwAddressSize); the GUIDs are
// REFGUIDs, so addresses.
void l_CreateAddress() {
    SFCOP_LOBBY_OR_FAIL(7);
    if (A32(1) == 0 || A32(2) == 0 || (A32(3) == 0 && A32(4) != 0)) {
        RET(kDpErrInvalidParams, 7);
    }
    std::vector<std::uint8_t> data = bytesAt(A32(3), A32(4));
    std::vector<std::uint8_t> address;
    HResult hr = lobby->CreateAddress(guidAt(A32(1)), guidAt(A32(2)), data.data(), data.size(),
                                      &address);
    if (hr != kDpOk) {
        RET(hr, 7);
    }
    RET(answerBytes(address, A32(5), A32(6)), 7);
}
// (this, lpEnumAddressCallback, lpAddress, dwAddressSize, lpContext); the callback is
// (guidDataType, dwDataSize, lpData, lpContext).
void l_EnumAddress() {
    SFCOP_LOBBY_OR_FAIL(5);
    std::uint32_t fn = A32(1), va = A32(2), bytes = A32(3), context = A32(4);
    if (fn == 0 || va == 0 || bytes > kMaxAddressBytes) {
        RET(kDpErrInvalidParams, 5);
    }
    std::vector<std::uint8_t> address = bytesAt(va, bytes);
    auto callback = [fn, context](const AddressElement &e) {
        std::uint32_t mark = hle_scratch_mark();
        std::uint32_t type = scratchGuid(e.type);
        std::uint32_t size = static_cast<std::uint32_t>(e.data.size());
        std::uint32_t data = size != 0 ? hle_scratch_copy(e.data.data(), size) : 0;
        std::uint32_t go = callGuest(fn, {type, size, data, context});
        hle_scratch_release(mark);
        return go != 0;
    };
    RET(lobby->EnumAddress(address.data(), address.size(), callback), 5);
}
void l_GetConnectionSettings() { // (this, dwAppID, lpData, lpdwDataSize)
    SFCOP_LOBBY_OR_FAIL(4);
    RET(lobby->GetConnectionSettings(), 4);
}
void l_RunApplication() { // (this, dwFlags, lpdwAppID, lpConn, hReceiveEvent)
    SFCOP_LOBBY_OR_FAIL(5);
    RET(lobby->RunApplication(), 5);
}
void l_SetConnectionSettings() { // (this, dwFlags, dwAppID, lpConn)
    SFCOP_LOBBY_OR_FAIL(4);
    RET(lobby->SetConnectionSettings(), 4);
}
void l_CreateCompoundAddress() { // (this, lpElements, dwElementCount, lpAddress, lpdwAddressSize)
    SFCOP_LOBBY_OR_FAIL(5);
    std::uint32_t elements = A32(1), count = A32(2);
    if (elements == 0 || count == 0 || count > kMaxAddressElements) {
        RET(kDpErrInvalidParams, 5);
    }
    std::vector<AddressElement> list;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t e = elements + kElementBytes * i;
        std::uint32_t size = MEM32(e + 16);
        if (size > kMaxAddressBytes || (size != 0 && MEM32(e + 20) == 0)) {
            RET(kDpErrInvalidParams, 5);
        }
        list.push_back(AddressElement{guidAt(e), bytesAt(MEM32(e + 20), size)});
    }
    std::vector<std::uint8_t> address;
    HResult hr = lobby->CreateCompoundAddress(list, &address);
    if (hr != kDpOk) {
        RET(hr, 5);
    }
    RET(answerBytes(address, A32(3), A32(4)), 5);
}
void l_ConnectEx() { // (this, dwFlags, riid, lplpDP, pUnk)
    SFCOP_LOBBY_OR_FAIL(5);
    if (A32(3) != 0) {
        MEM32(A32(3)) = 0;
    }
    RET(lobby->ConnectEx(), 5);
}
void l_WaitForConnectionSettings() { // (this, dwFlags)
    SFCOP_LOBBY_OR_FAIL(2);
    RET(lobby->WaitForConnectionSettings(), 2);
}

// The methods the game never calls.
void l_EnumAddressTypes() {
    RET(kDpErrUnsupported, 5);
}
void l_EnumLocalApplications() {
    RET(kDpErrUnsupported, 4);
}
void l_ReceiveLobbyMessage() {
    RET(kDpErrUnsupported, 6);
}
void l_SendLobbyMessage() {
    RET(kDpErrUnsupported, 5);
}
void l_SetLobbyMessageEvent() {
    RET(kDpErrUnsupported, 4);
}
void l_RegisterApplication() {
    RET(kDpErrUnsupported, 3);
}
void l_UnregisterApplication() {
    RET(kDpErrUnsupported, 3);
}

// ---- tables ------------------------------------------------------------------------------

// Each interface's methods in slot order (src/hle/com/IDirectPlay4A.h, IDirectPlayLobby3A.h).
#define SFCOP_PLAY_METHODS(X)                                                                  \
    X("IDirectPlay4A::QueryInterface", p_QueryInterface)                                       \
    X("IDirectPlay4A::AddRef", p_AddRef)                                                       \
    X("IDirectPlay4A::Release", p_Release)                                                     \
    X("IDirectPlay4A::AddPlayerToGroup", p_AddPlayerToGroup)                                   \
    X("IDirectPlay4A::Close", p_Close)                                                         \
    X("IDirectPlay4A::CreateGroup", p_CreateGroup)                                             \
    X("IDirectPlay4A::CreatePlayer", p_CreatePlayer)                                           \
    X("IDirectPlay4A::DeletePlayerFromGroup", p_DeletePlayerFromGroup)                         \
    X("IDirectPlay4A::DestroyGroup", p_DestroyGroup)                                           \
    X("IDirectPlay4A::DestroyPlayer", p_DestroyPlayer)                                         \
    X("IDirectPlay4A::EnumGroupPlayers", p_EnumGroupPlayers)                                   \
    X("IDirectPlay4A::EnumGroups", p_EnumGroups)                                               \
    X("IDirectPlay4A::EnumPlayers", p_EnumPlayers)                                             \
    X("IDirectPlay4A::EnumSessions", p_EnumSessions)                                           \
    X("IDirectPlay4A::GetCaps", p_GetCaps)                                                     \
    X("IDirectPlay4A::GetGroupData", p_GetGroupData)                                           \
    X("IDirectPlay4A::GetGroupName", p_GetGroupName)                                           \
    X("IDirectPlay4A::GetMessageCount", p_GetMessageCount)                                     \
    X("IDirectPlay4A::GetPlayerAddress", p_GetPlayerAddress)                                   \
    X("IDirectPlay4A::GetPlayerCaps", p_GetPlayerCaps)                                         \
    X("IDirectPlay4A::GetPlayerData", p_GetPlayerData)                                         \
    X("IDirectPlay4A::GetPlayerName", p_GetPlayerName)                                         \
    X("IDirectPlay4A::GetSessionDesc", p_GetSessionDesc)                                       \
    X("IDirectPlay4A::Initialize", p_Initialize)                                               \
    X("IDirectPlay4A::Open", p_Open)                                                           \
    X("IDirectPlay4A::Receive", p_Receive)                                                     \
    X("IDirectPlay4A::Send", p_Send)                                                           \
    X("IDirectPlay4A::SetGroupData", p_SetGroupData)                                           \
    X("IDirectPlay4A::SetGroupName", p_SetGroupName)                                           \
    X("IDirectPlay4A::SetPlayerData", p_SetPlayerData)                                         \
    X("IDirectPlay4A::SetPlayerName", p_SetPlayerName)                                         \
    X("IDirectPlay4A::SetSessionDesc", p_SetSessionDesc)                                       \
    X("IDirectPlay4A::AddGroupToGroup", p_AddGroupToGroup)                                     \
    X("IDirectPlay4A::CreateGroupInGroup", p_CreateGroupInGroup)                               \
    X("IDirectPlay4A::DeleteGroupFromGroup", p_DeleteGroupFromGroup)                           \
    X("IDirectPlay4A::EnumConnections", p_EnumConnections)                                     \
    X("IDirectPlay4A::EnumGroupsInGroup", p_EnumGroupsInGroup)                                 \
    X("IDirectPlay4A::GetGroupConnectionSettings", p_GetGroupConnectionSettings)               \
    X("IDirectPlay4A::InitializeConnection", p_InitializeConnection)                           \
    X("IDirectPlay4A::SecureOpen", p_SecureOpen)                                               \
    X("IDirectPlay4A::SendChatMessage", p_SendChatMessage)                                     \
    X("IDirectPlay4A::SetGroupConnectionSettings", p_SetGroupConnectionSettings)               \
    X("IDirectPlay4A::StartSession", p_StartSession)                                           \
    X("IDirectPlay4A::GetGroupFlags", p_GetGroupFlags)                                         \
    X("IDirectPlay4A::GetGroupParent", p_GetGroupParent)                                       \
    X("IDirectPlay4A::GetPlayerAccount", p_GetPlayerAccount)                                   \
    X("IDirectPlay4A::GetPlayerFlags", p_GetPlayerFlags)                                       \
    X("IDirectPlay4A::GetGroupOwner", p_GetGroupOwner)                                         \
    X("IDirectPlay4A::SetGroupOwner", p_SetGroupOwner)                                         \
    X("IDirectPlay4A::SendEx", p_SendEx)                                                       \
    X("IDirectPlay4A::GetMessageQueue", p_GetMessageQueue)                                     \
    X("IDirectPlay4A::CancelMessage", p_CancelMessage)                                         \
    X("IDirectPlay4A::CancelPriority", p_CancelPriority)
#define SFCOP_LOBBY_METHODS(X)                                                                 \
    X("IDirectPlayLobby3A::QueryInterface", l_QueryInterface)                                  \
    X("IDirectPlayLobby3A::AddRef", l_AddRef)                                                  \
    X("IDirectPlayLobby3A::Release", l_Release)                                                \
    X("IDirectPlayLobby3A::Connect", l_Connect)                                                \
    X("IDirectPlayLobby3A::CreateAddress", l_CreateAddress)                                    \
    X("IDirectPlayLobby3A::EnumAddress", l_EnumAddress)                                        \
    X("IDirectPlayLobby3A::EnumAddressTypes", l_EnumAddressTypes)                              \
    X("IDirectPlayLobby3A::EnumLocalApplications", l_EnumLocalApplications)                    \
    X("IDirectPlayLobby3A::GetConnectionSettings", l_GetConnectionSettings)                    \
    X("IDirectPlayLobby3A::ReceiveLobbyMessage", l_ReceiveLobbyMessage)                        \
    X("IDirectPlayLobby3A::RunApplication", l_RunApplication)                                  \
    X("IDirectPlayLobby3A::SendLobbyMessage", l_SendLobbyMessage)                              \
    X("IDirectPlayLobby3A::SetConnectionSettings", l_SetConnectionSettings)                    \
    X("IDirectPlayLobby3A::SetLobbyMessageEvent", l_SetLobbyMessageEvent)                      \
    X("IDirectPlayLobby3A::CreateCompoundAddress", l_CreateCompoundAddress)                    \
    X("IDirectPlayLobby3A::ConnectEx", l_ConnectEx)                                            \
    X("IDirectPlayLobby3A::RegisterApplication", l_RegisterApplication)                        \
    X("IDirectPlayLobby3A::UnregisterApplication", l_UnregisterApplication)                    \
    X("IDirectPlayLobby3A::WaitForConnectionSettings", l_WaitForConnectionSettings)

#define SFCOP_SHIM(name, fn) {name, fn},

const win32hle_shim kPlayMethods[] = {SFCOP_PLAY_METHODS(SFCOP_SHIM){nullptr, nullptr}};
const win32hle_shim kLobbyMethods[] = {SFCOP_LOBBY_METHODS(SFCOP_SHIM){nullptr, nullptr}};

// Every method name is in sfcop_hle_dplay[] as well, so these vtables resolve to this file's
// shims.
void buildVtables() {
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    if (g_vtPlay != 0) {
        return;
    }
    g_vtPlay = hle_com_vtable(kPlayMethods);
    g_vtLobby = hle_com_vtable(kLobbyMethods);
}

} // namespace
} // namespace sfcop::hle::dplay

using namespace sfcop::hle::dplay;

extern "C" const win32hle_shim sfcop_hle_dplay[] = {
    SFCOP_PLAY_METHODS(SFCOP_SHIM) SFCOP_LOBBY_METHODS(SFCOP_SHIM){nullptr, nullptr}};
