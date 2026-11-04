/*
 * The guest shims of the WSOCK32 module, in win32hle's shim form (src/hle/README.md,
 * "Implementing a module"). One shim per surface row: it reads the stdcall arguments from the
 * guest stack, copies the Winsock structures out of guest memory field by field into the forms
 * of WinsockApi.h, calls the module, writes the results back and pops exactly the row's
 * stack_bytes. Every entry is registered by name and by ordinal ("wsock32.dll#N"), since the
 * game imports WSOCK32 by ordinal.
 *
 * A call that can wait (accept, connect, recv, send, select, closesocket with linger,
 * gethostbyname) gives the machine up around the wait, with its arguments already read; the
 * structures are written back after it has the machine again. Data buffers are the guest's own
 * memory, filled in place, as Winsock fills them.
 *
 * Guest layouts (Winsock 1.1, 32-bit):
 *   sockaddr_in  16 bytes: u16 family, u16 port (network order), u32 address (network order),
 *                8 bytes zero
 *   fd_set       u32 fd_count, SOCKET fd_array[64]
 *   timeval      i32 tv_sec, i32 tv_usec
 *   WSADATA      400 bytes: u16 wVersion, u16 wHighVersion, char szDescription[257] at 4,
 *                char szSystemStatus[129] at 261, u16 iMaxSockets at 390, u16 iMaxUdpDg at 392,
 *                char *lpVendorInfo at 396
 *   hostent      char *h_name, char **h_aliases, i16 h_addrtype, i16 h_length,
 *                char **h_addr_list
 */
#include "hle/winsock/WinsockApi.h"
#include "win32hle.h"

#include <string.h>

enum { kSockAddrSize = 16, kFdSetMax = 64, kNoWrite = -1 };

static void sockaddr_read(uint32_t va, WinsockSockAddr *out) {
    out->family = (int16_t)MEM16(va);
    out->port = (uint16_t)((uint32_t)MEM8(va + 2) << 8 | MEM8(va + 3));
    out->address = (uint32_t)MEM8(va + 4) << 24 | (uint32_t)MEM8(va + 5) << 16 |
                   (uint32_t)MEM8(va + 6) << 8 | MEM8(va + 7);
}

static void sockaddr_write(uint32_t va, const WinsockSockAddr *in) {
    MEM16(va) = (uint16_t)in->family;
    MEM8(va + 2) = (uint8_t)(in->port >> 8);
    MEM8(va + 3) = (uint8_t)in->port;
    MEM8(va + 4) = (uint8_t)(in->address >> 24);
    MEM8(va + 5) = (uint8_t)(in->address >> 16);
    MEM8(va + 6) = (uint8_t)(in->address >> 8);
    MEM8(va + 7) = (uint8_t)in->address;
    for (uint32_t i = 8; i < kSockAddrSize; i++) MEM8(va + i) = 0;
}

/* An input sockaddr: NULL when the guest passed none. */
static const WinsockSockAddr *sockaddr_in_arg(uint32_t va, WinsockSockAddr *scratch) {
    if (!va) return NULL;
    sockaddr_read(va, scratch);
    return scratch;
}

/* An out-parameter pair (address, &length). The module writes `family` only when it writes the
 * address, so `kNoWrite` there means: leave the guest's copy alone. */
typedef struct {
    uint32_t address_va, length_va;
    WinsockSockAddr address;
    int32_t length;
} out_addr;

static void out_addr_begin(out_addr *o, uint32_t address_va, uint32_t length_va) {
    o->address_va = address_va;
    o->length_va = length_va;
    memset(&o->address, 0, sizeof o->address);
    o->address.family = kNoWrite;
    o->length = length_va ? (int32_t)MEM32(length_va) : 0;
}

static void out_addr_end(const out_addr *o) {
    if (o->address.family == kNoWrite) return;
    if (o->address_va) sockaddr_write(o->address_va, &o->address);
    if (o->length_va) MEM32(o->length_va) = (uint32_t)o->length;
}

static void fdset_read(uint32_t va, WinsockFdSet *out) {
    uint32_t count = MEM32(va);
    out->count = count;
    if (count > kFdSetMax) count = kFdSetMax;
    for (uint32_t i = 0; i < count; i++) out->sockets[i] = MEM32(va + 4 + 4 * i);
}

static void fdset_write(uint32_t va, const WinsockFdSet *in) {
    MEM32(va) = in->count;
    for (uint32_t i = 0; i < in->count && i < kFdSetMax; i++) MEM32(va + 4 + 4 * i) = in->sockets[i];
}

static void wsadata_write(uint32_t va, const WinsockData *in) {
    for (uint32_t i = 0; i < 400; i++) MEM8(va + i) = 0;
    MEM16(va) = in->version;
    MEM16(va + 2) = in->highVersion;
    for (uint32_t i = 0; i < sizeof in->description && in->description[i]; i++)
        MEM8(va + 4 + i) = (uint8_t)in->description[i];
    for (uint32_t i = 0; i < sizeof in->systemStatus && in->systemStatus[i]; i++)
        MEM8(va + 261 + i) = (uint8_t)in->systemStatus[i];
    MEM16(va + 390) = in->maxSockets;
    MEM16(va + 392) = in->maxUdpDatagram;
    MEM32(va + 396) = 0;
}

/* ---- startup and errors ---- */

static void w_WSAStartup(void) { /* (wVersionRequested, lpWSAData) */
    uint32_t data_va = A32(1);
    WinsockData data;
    memset(&data, 0, sizeof data);
    int32_t r = sfcop_winsock_WSAStartup((uint16_t)A32(0), data_va ? &data : NULL);
    if (data_va && data.version) wsadata_write(data_va, &data);
    RET(r, 2);
}

static void w_WSACleanup(void) {
    int32_t r;
    hle_block_begin();
    r = sfcop_winsock_WSACleanup();
    hle_block_end();
    RET(r, 0);
}

static void w_WSAGetLastError(void) { RET(sfcop_winsock_WSAGetLastError(), 0); }

/* ---- sockets ---- */

static void w_socket(void) { /* (af, type, protocol) */
    RET(sfcop_winsock_socket((int32_t)A32(0), (int32_t)A32(1), (int32_t)A32(2)), 3);
}

static void w_closesocket(void) { /* (s) */
    uint32_t s = A32(0);
    int32_t r;
    hle_block_begin();
    r = sfcop_winsock_closesocket(s);
    hle_block_end();
    RET(r, 1);
}

static void w_bind(void) { /* (s, name, namelen) */
    WinsockSockAddr name;
    RET(sfcop_winsock_bind(A32(0), sockaddr_in_arg(A32(1), &name), (int32_t)A32(2)), 3);
}

static void w_connect(void) { /* (s, name, namelen) */
    WinsockSockAddr scratch;
    uint32_t s = A32(0);
    const WinsockSockAddr *name = sockaddr_in_arg(A32(1), &scratch);
    int32_t length = (int32_t)A32(2), r;
    hle_block_begin();
    r = sfcop_winsock_connect(s, name, length);
    hle_block_end();
    RET(r, 3);
}

static void w_listen(void) { /* (s, backlog) */
    RET(sfcop_winsock_listen(A32(0), (int32_t)A32(1)), 2);
}

static void w_accept(void) { /* (s, addr, addrlen) */
    out_addr o;
    uint32_t s = A32(0), r;
    out_addr_begin(&o, A32(1), A32(2));
    hle_block_begin();
    r = sfcop_winsock_accept(s, o.address_va ? &o.address : NULL, o.length_va ? &o.length : NULL);
    hle_block_end();
    out_addr_end(&o);
    RET(r, 3);
}

static void w_send(void) { /* (s, buf, len, flags) */
    uint32_t s = A32(0);
    const void *buffer = APTR(1);
    int32_t length = (int32_t)A32(2), flags = (int32_t)A32(3), r;
    hle_block_begin();
    r = sfcop_winsock_send(s, buffer, length, flags);
    hle_block_end();
    RET(r, 4);
}

static void w_recv(void) { /* (s, buf, len, flags) */
    uint32_t s = A32(0);
    void *buffer = APTR(1);
    int32_t length = (int32_t)A32(2), flags = (int32_t)A32(3), r;
    hle_block_begin();
    r = sfcop_winsock_recv(s, buffer, length, flags);
    hle_block_end();
    RET(r, 4);
}

static void w_sendto(void) { /* (s, buf, len, flags, to, tolen) */
    WinsockSockAddr scratch;
    uint32_t s = A32(0);
    const void *buffer = APTR(1);
    int32_t length = (int32_t)A32(2), flags = (int32_t)A32(3), to_length = (int32_t)A32(5), r;
    const WinsockSockAddr *to = sockaddr_in_arg(A32(4), &scratch);
    hle_block_begin();
    r = sfcop_winsock_sendto(s, buffer, length, flags, to, to_length);
    hle_block_end();
    RET(r, 6);
}

static void w_recvfrom(void) { /* (s, buf, len, flags, from, fromlen) */
    out_addr o;
    uint32_t s = A32(0);
    void *buffer = APTR(1);
    int32_t length = (int32_t)A32(2), flags = (int32_t)A32(3), r;
    out_addr_begin(&o, A32(4), A32(5));
    hle_block_begin();
    r = sfcop_winsock_recvfrom(s, buffer, length, flags, o.address_va ? &o.address : NULL,
                               o.length_va ? &o.length : NULL);
    hle_block_end();
    out_addr_end(&o);
    RET(r, 6);
}

static void w_select(void) { /* (nfds, readfds, writefds, exceptfds, timeout) */
    uint32_t va[3] = { A32(1), A32(2), A32(3) }, tv = A32(4);
    WinsockFdSet sets[3];
    WinsockTimeVal timeout = { 0, 0 };
    int32_t nfds = (int32_t)A32(0), r;
    for (int k = 0; k < 3; k++) if (va[k]) fdset_read(va[k], &sets[k]);
    if (tv) timeout.seconds = (int32_t)MEM32(tv), timeout.microseconds = (int32_t)MEM32(tv + 4);
    int waits = !tv || timeout.seconds != 0 || timeout.microseconds != 0;
    if (waits) hle_block_begin();
    r = sfcop_winsock_select(nfds, va[0] ? &sets[0] : NULL, va[1] ? &sets[1] : NULL,
                             va[2] ? &sets[2] : NULL, tv ? &timeout : NULL);
    if (waits) hle_block_end();
    if (r >= 0)
        for (int k = 0; k < 3; k++) if (va[k]) fdset_write(va[k], &sets[k]);
    RET(r, 5);
}

static void w___WSAFDIsSet(void) { /* (s, set) */
    WinsockFdSet set;
    uint32_t va = A32(1);
    if (!va) RET(0, 2);
    fdset_read(va, &set);
    RET(sfcop_winsock_fd_is_set(A32(0), &set), 2);
}

static void w_shutdown(void) { /* (s, how) */
    RET(sfcop_winsock_shutdown(A32(0), (int32_t)A32(1)), 2);
}

static void w_getpeername(void) { /* (s, name, namelen) */
    out_addr o;
    int32_t r;
    out_addr_begin(&o, A32(1), A32(2));
    r = sfcop_winsock_getpeername(A32(0), o.address_va ? &o.address : NULL,
                                  o.length_va ? &o.length : NULL);
    out_addr_end(&o);
    RET(r, 3);
}

static void w_getsockname(void) { /* (s, name, namelen) */
    out_addr o;
    int32_t r;
    out_addr_begin(&o, A32(1), A32(2));
    r = sfcop_winsock_getsockname(A32(0), o.address_va ? &o.address : NULL,
                                  o.length_va ? &o.length : NULL);
    out_addr_end(&o);
    RET(r, 3);
}

static void w_ioctlsocket(void) { /* (s, cmd, argp) */
    uint32_t argp = A32(2), argument = argp ? MEM32(argp) : 0;
    int32_t r = sfcop_winsock_ioctlsocket(A32(0), (int32_t)A32(1), argp ? &argument : NULL);
    if (r == 0 && argp) MEM32(argp) = argument;
    RET(r, 3);
}

static void w_setsockopt(void) { /* (s, level, optname, optval, optlen) */
    uint32_t optval = A32(3), value = 0;
    int32_t length = (int32_t)A32(4);
    if (optval && length >= 4) value = MEM32(optval);
    RET(sfcop_winsock_setsockopt(A32(0), (int32_t)A32(1), (int32_t)A32(2),
                                 optval ? &value : NULL, length),
        5);
}

static void w_getsockopt(void) { /* (s, level, optname, optval, optlen) */
    uint32_t optval = A32(3), optlen = A32(4), value = 0;
    int32_t length = optlen ? (int32_t)MEM32(optlen) : 0;
    int32_t r = sfcop_winsock_getsockopt(A32(0), (int32_t)A32(1), (int32_t)A32(2),
                                         optval ? &value : NULL, optlen ? &length : NULL);
    if (r == 0) {
        MEM32(optval) = value;
        MEM32(optlen) = (uint32_t)length;
    }
    RET(r, 5);
}

/* ---- names ---- */

/* One hostent per thread, in guest memory, as Winsock keeps one per thread:
 *   +0 the hostent, +16 h_aliases (one NULL), +20 h_addr_list (16 pointers and a NULL),
 *   +88 the addresses (16 x 4 bytes, network order), +152 the name (256 bytes). */
enum { kHostentSize = 408, kHostentAliases = 16, kHostentList = 20, kHostentAddresses = 88,
       kHostentName = 152 };

static void w_gethostbyname(void) { /* (name) */
    static __thread uint32_t hostent_va;
    WinsockHostEntry entry;
    char name[256];
    const char *asked = NULL;
    int32_t found;
    if (A32(0)) {
        uint32_t va = A32(0), i = 0;
        for (; i < sizeof name - 1 && MEM8(va + i); i++) name[i] = (char)MEM8(va + i);
        name[i] = '\0';
        asked = name;
    }
    hle_block_begin();
    found = sfcop_winsock_gethostbyname(asked, &entry);
    hle_block_end();
    if (!found) RET(0, 1);
    if (!hostent_va) hostent_va = hle_va(hle_alloc(kHostentSize));
    if (!hostent_va) {
        sfcop_winsock_WSASetLastError(10055); /* WSAENOBUFS */
        RET(0, 1);
    }
    uint32_t h = hostent_va;
    for (uint32_t i = 0; i < kHostentSize; i++) MEM8(h + i) = 0;
    for (uint32_t i = 0; i < sizeof entry.name - 1 && entry.name[i]; i++)
        MEM8(h + kHostentName + i) = (uint8_t)entry.name[i];
    MEM32(h) = h + kHostentName;
    MEM32(h + 4) = h + kHostentAliases;
    MEM16(h + 8) = 2;  /* AF_INET */
    MEM16(h + 10) = 4; /* bytes per address */
    MEM32(h + 12) = h + kHostentList;
    for (uint32_t i = 0; i < entry.count && i < 16; i++) {
        uint32_t a = h + kHostentAddresses + 4 * i;
        MEM8(a) = (uint8_t)(entry.addresses[i] >> 24);
        MEM8(a + 1) = (uint8_t)(entry.addresses[i] >> 16);
        MEM8(a + 2) = (uint8_t)(entry.addresses[i] >> 8);
        MEM8(a + 3) = (uint8_t)entry.addresses[i];
        MEM32(h + kHostentList + 4 * i) = a;
    }
    RET(h, 1);
}

static void w_gethostname(void) { /* (name, namelen) */
    RET(sfcop_winsock_gethostname(ASTR(0), (int32_t)A32(1)), 2);
}

/* ---- byte order and addresses ---- */

static void w_htonl(void) { RET(sfcop_winsock_htonl(A32(0)), 1); }
static void w_htons(void) { RET(sfcop_winsock_htons((uint16_t)A32(0)), 1); }
static void w_ntohs(void) { RET(sfcop_winsock_ntohs((uint16_t)A32(0)), 1); }

static void w_inet_addr(void) { /* (cp) */
    char text[64];
    uint32_t va = A32(0), i = 0;
    if (!va) RET(0xFFFFFFFFu, 1);
    for (; i < sizeof text - 1 && MEM8(va + i); i++) text[i] = (char)MEM8(va + i);
    text[i] = '\0';
    RET(sfcop_winsock_inet_addr(text), 1);
}

static void w_inet_ntoa(void) { /* (in_addr by value) */
    static __thread uint32_t text_va; /* one per thread, as Winsock's */
    char text[16];
    sfcop_winsock_inet_ntoa(A32(0), text);
    if (!text_va) text_va = hle_va(hle_alloc(16));
    if (!text_va) RET(0, 1);
    for (uint32_t i = 0; i < 16; i++) MEM8(text_va + i) = (uint8_t)text[i];
    RET(text_va, 1);
}

#define WS_SHIM(ordinal, name, fn) { "wsock32.dll#" #ordinal, fn }, { name, fn }

const win32hle_shim sfcop_hle_winsock[] = {
    WS_SHIM(151, "__WSAFDIsSet", w___WSAFDIsSet),
    WS_SHIM(1, "accept", w_accept),
    WS_SHIM(2, "bind", w_bind),
    WS_SHIM(3, "closesocket", w_closesocket),
    WS_SHIM(4, "connect", w_connect),
    WS_SHIM(52, "gethostbyname", w_gethostbyname),
    WS_SHIM(57, "gethostname", w_gethostname),
    WS_SHIM(5, "getpeername", w_getpeername),
    WS_SHIM(6, "getsockname", w_getsockname),
    WS_SHIM(7, "getsockopt", w_getsockopt),
    WS_SHIM(8, "htonl", w_htonl),
    WS_SHIM(9, "htons", w_htons),
    WS_SHIM(10, "inet_addr", w_inet_addr),
    WS_SHIM(11, "inet_ntoa", w_inet_ntoa),
    WS_SHIM(12, "ioctlsocket", w_ioctlsocket),
    WS_SHIM(13, "listen", w_listen),
    WS_SHIM(15, "ntohs", w_ntohs),
    WS_SHIM(16, "recv", w_recv),
    WS_SHIM(17, "recvfrom", w_recvfrom),
    WS_SHIM(18, "select", w_select),
    WS_SHIM(19, "send", w_send),
    WS_SHIM(20, "sendto", w_sendto),
    WS_SHIM(21, "setsockopt", w_setsockopt),
    WS_SHIM(22, "shutdown", w_shutdown),
    WS_SHIM(23, "socket", w_socket),
    WS_SHIM(116, "WSACleanup", w_WSACleanup),
    WS_SHIM(111, "WSAGetLastError", w_WSAGetLastError),
    WS_SHIM(115, "WSAStartup", w_WSAStartup),
    { 0, 0 }
};
