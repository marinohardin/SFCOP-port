/*
 * The Winsock module's C entry points, for the guest shims in WinsockGuest.c (src/hle/README.md,
 * "Implementing a module": the lift's headers are C, so the shims are C and call the module
 * through these). One function per WSOCK32 entry, named after it, taking the Winsock structures
 * in the host-side form below; the shims read and write the guest's own layout field by field.
 *
 * Every function behaves as the Winsock 1.1 call of the same name: the same return values, the
 * same error left for WSAGetLastError. They work on one module instance for the whole process,
 * created on first use on the host Transport (Winsock.h: guestWinsock()). Any thread may call
 * them; a call may block as the Winsock call would.
 */
#ifndef SFCOP_HLE_WINSOCK_WINSOCKAPI_H
#define SFCOP_HLE_WINSOCK_WINSOCKAPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sockaddr_in, with the port and the address in host byte order (192.0.2.1 is 0xC0000201). */
typedef struct WinsockSockAddr {
    int16_t family; /* 2: AF_INET */
    uint16_t port;
    uint32_t address;
} WinsockSockAddr;

/* fd_set: a count and up to FD_SETSIZE (64) guest SOCKETs. */
typedef struct WinsockFdSet {
    uint32_t count;
    uint32_t sockets[64];
} WinsockFdSet;

/* timeval: Winsock's fields are 32-bit longs. */
typedef struct WinsockTimeVal {
    int32_t seconds;
    int32_t microseconds;
} WinsockTimeVal;

/* WSADATA, without lpVendorInfo (always NULL in the guest's copy). */
typedef struct WinsockData {
    uint16_t version;
    uint16_t highVersion;
    char description[257];
    char systemStatus[129];
    uint16_t maxSockets;
    uint16_t maxUdpDatagram;
} WinsockData;

/* What gethostbyname found: the name and up to 16 IPv4 addresses, in host byte order. */
typedef struct WinsockHostEntry {
    char name[256];
    uint32_t count;
    uint32_t addresses[16];
} WinsockHostEntry;

int32_t sfcop_winsock_WSAStartup(uint16_t versionRequested, WinsockData *data);
int32_t sfcop_winsock_WSACleanup(void);
int32_t sfcop_winsock_WSAGetLastError(void);
void sfcop_winsock_WSASetLastError(int32_t error);

uint32_t sfcop_winsock_socket(int32_t family, int32_t type, int32_t protocol);
int32_t sfcop_winsock_closesocket(uint32_t s);
int32_t sfcop_winsock_bind(uint32_t s, const WinsockSockAddr *name, int32_t nameLength);
int32_t sfcop_winsock_connect(uint32_t s, const WinsockSockAddr *name, int32_t nameLength);
int32_t sfcop_winsock_listen(uint32_t s, int32_t backlog);
uint32_t sfcop_winsock_accept(uint32_t s, WinsockSockAddr *address, int32_t *addressLength);
int32_t sfcop_winsock_send(uint32_t s, const void *buffer, int32_t length, int32_t flags);
int32_t sfcop_winsock_recv(uint32_t s, void *buffer, int32_t length, int32_t flags);
int32_t sfcop_winsock_sendto(uint32_t s, const void *buffer, int32_t length, int32_t flags,
                             const WinsockSockAddr *to, int32_t toLength);
int32_t sfcop_winsock_recvfrom(uint32_t s, void *buffer, int32_t length, int32_t flags,
                               WinsockSockAddr *from, int32_t *fromLength);
int32_t sfcop_winsock_select(int32_t nfds, WinsockFdSet *readfds, WinsockFdSet *writefds,
                             WinsockFdSet *exceptfds, const WinsockTimeVal *timeout);
int32_t sfcop_winsock_fd_is_set(uint32_t s, const WinsockFdSet *set); /* __WSAFDIsSet */
int32_t sfcop_winsock_shutdown(uint32_t s, int32_t how);
int32_t sfcop_winsock_getpeername(uint32_t s, WinsockSockAddr *name, int32_t *nameLength);
int32_t sfcop_winsock_getsockname(uint32_t s, WinsockSockAddr *name, int32_t *nameLength);
int32_t sfcop_winsock_ioctlsocket(uint32_t s, int32_t command, uint32_t *argument);
/* Every option the game sets is 4 bytes; `value` is those bytes as a little-endian word. */
int32_t sfcop_winsock_setsockopt(uint32_t s, int32_t level, int32_t name, const uint32_t *value,
                                 int32_t length);
int32_t sfcop_winsock_getsockopt(uint32_t s, int32_t level, int32_t name, uint32_t *value,
                                 int32_t *length);
/* 1 and the entry, or 0 with the error set; a NULL or empty name is this machine's. */
int32_t sfcop_winsock_gethostbyname(const char *name, WinsockHostEntry *entry);
int32_t sfcop_winsock_gethostname(char *name, int32_t length);

/* The byte-order and address functions, on the guest's little-endian view: an in_addr passed
 * by value holds the first octet in its low byte. */
uint16_t sfcop_winsock_htons(uint16_t value);
uint16_t sfcop_winsock_ntohs(uint16_t value);
uint32_t sfcop_winsock_htonl(uint32_t value);
uint32_t sfcop_winsock_ntohl(uint32_t value);
uint32_t sfcop_winsock_inet_addr(const char *text);
void sfcop_winsock_inet_ntoa(uint32_t address, char out[16]);

#ifdef __cplusplus
}
#endif

#endif /* SFCOP_HLE_WINSOCK_WINSOCKAPI_H */
