#!/usr/bin/env python3
"""Drives a running sfcop-relay over real loopback UDP, as two clients and a stranger would.

Used by container-test-inner.sh inside the test container only (server/deploy/test-container.sh). It reads the ticket key from the
environment file the way the lobby does, and never prints it.
"""
import hmac
import hashlib
import socket
import struct
import sys
import time
import urllib.request

RELAY = ("127.0.0.1", 47710)
SESSION = 0x5346434F50000001


def key():
    for line in open("/etc/sfcop/sfcop.env"):
        if line.startswith("SFCOP_TICKET_KEY="):
            return bytes.fromhex(line.strip().split("=", 1)[1])
    raise SystemExit("no ticket key")


def ticket(peer, expires=None):
    expires = int(time.time()) + 600 if expires is None else expires
    body = struct.pack("<QIIB3x", SESSION, peer, expires, 1 if peer == 1 else 2)
    return body + hmac.new(key(), body, hashlib.sha256).digest()[:16]


def bind_datagram(peer, txn=1):
    return struct.pack("<4sBBHI", b"SFRV", 1, 1, 0, txn) + ticket(peer) + b"\x00\x00"


def envelope(src, dst, payload, session=SESSION):
    header = struct.pack("<4sBBBBQIIIHH", b"SFNE", 1, 1, 0, 0, session, src, dst, 7, 0, 1)
    return header + payload


def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.5)
    return s


def recv(s):
    try:
        return s.recvfrom(2048)[0]
    except socket.timeout:
        return None


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        sys.exit(1)


a, b, stranger = sock(), sock(), sock()

for s, peer in ((a, 1), (b, 2)):
    s.sendto(bind_datagram(peer), RELAY)
    reply = recv(s)
    check(reply is not None and reply[5] == 2 and reply[12:16] == b"\0\0\0\0", f"peer {peer} bound (BindReply ok)")
    port = struct.unpack("<H", reply[20:22])[0]
    check(port == s.getsockname()[1], f"peer {peer}: the reflexive port is the one the relay saw")

forged = bytearray(bind_datagram(1, 9))
forged[12 + 20] ^= 1  # first byte of the mac
stranger.sendto(bytes(forged), RELAY)
reply = recv(stranger)
check(reply is not None and reply[5] == 8 and struct.unpack("<I", reply[12:16])[0] == 1, "a forged ticket gets kBadTicket")

b.sendto(struct.pack("<4sBBHIQII", b"SFRV", 1, 3, 0, 5, SESSION, 2, 1), RELAY)
got_b, got_a = recv(b), recv(a)
check(got_b is not None and got_b[5] == 4 and got_a is not None and got_a[5] == 4, "Connect: both peers get PeerCandidates")
check(got_a[24:28] == got_b[24:28] != b"\0\0\0\0", "both carry the same nonzero punch id")
check(recv(a) == got_a and recv(b) == got_b, "the second copy follows 100 ms later, identical")

payload = bytes(range(256)) * 4
a.sendto(envelope(1, 2, payload), RELAY)
r = recv(b)
check(r == envelope(1, 2, payload), "A -> B: the relayed envelope equals the sent bytes (1056 bytes)")
b.sendto(envelope(2, 1, b"x" * 1168), RELAY)
r = recv(a)
check(r == envelope(2, 1, b"x" * 1168), "B -> A: a 1200-byte envelope arrives unchanged")

stranger.sendto(envelope(1, 2, b"spoof"), RELAY)
check(recv(b) is None, "a third address using peer 1's ids is not forwarded")
a.sendto(envelope(1, 2, b"x" * 1169), RELAY)
check(recv(b) is None, "a 1201-byte datagram is dropped")
a.sendto(envelope(1, 2, b"hi", session=SESSION + 1), RELAY)
check(recv(b) is None, "an envelope for a session with no bindings is dropped")

# Throughput and the cap: 20000 x 200-byte envelopes from A as fast as the loop allows.
b.settimeout(0.05)
n = 20000
frame = envelope(1, 2, b"y" * 168)
t0 = time.time()
for _ in range(n):
    a.sendto(frame, RELAY)
sent_seconds = time.time() - t0
time.sleep(0.3)
received = 0
while recv(b) is not None:
    received += 1
print(f"measured: sent {n} x {len(frame)} B in {sent_seconds:.3f} s ({n / sent_seconds:.0f}/s); "
      f"the relay forwarded {received}")
cap = 1000 + int(1000 * sent_seconds)  # one-second burst plus the refill while sending
check(received <= cap + 5, f"forwarding stayed within the per-binding cap ({received} <= {cap})")
check(received >= 900, "and at least the burst was forwarded")

# Idle sessions: removal through the loopback endpoint stops forwarding at once.
req = urllib.request.Request(f"http://127.0.0.1:47711/v1/bindings/{SESSION:016x}", method="DELETE")
check(urllib.request.urlopen(req).status == 204, "DELETE /v1/bindings/<session> answers 204")
time.sleep(0.2)
b.settimeout(0.5)
a.sendto(envelope(1, 2, b"after"), RELAY)
check(recv(b) is None, "after the lobby removed the session nothing is forwarded")
health = urllib.request.urlopen("http://127.0.0.1:47711/v1/health").read().decode()
print("health:", health)
check('"sessions":0' in health, "health counts no bindings after the removal")
print("peers: all checks passed")
