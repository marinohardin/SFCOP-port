#!/usr/bin/env python3
"""Drives a running sfcop-directory and sfcop-auth over real loopback TCP, as the original client
would on its way to a logon (src/net/DIRECTORY_API.md).

Used by container-test-inner.sh inside the test container only (server/deploy/test-container.sh).
Every value is invented. Exit status 0 when every exchange answered as documented.
"""
import hashlib
import socket
import sys

HOST = "127.0.0.1"


def exchange(port, data, server_first=False):
    with socket.create_connection((HOST, port), timeout=5) as s:
        first = s.recv(4096) if server_first else b""
        if data:
            s.sendall(data)
        out = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                return first, out
            out += chunk


def wstr(text):
    raw = text.encode("utf-16-le")
    return len(text).to_bytes(2, "little") + raw


def getdir(path, flags):
    body = bytes([5]) + (2).to_bytes(2, "little") + (0x67).to_bytes(2, "little")
    body += flags.to_bytes(4, "little") + b"\0\0" + wstr(path) + b"\0\0"
    return (len(body) + 4).to_bytes(4, "little") + body


def md5(text):
    return hashlib.md5(text.encode()).hexdigest()


def main():
    _, fw = exchange(15101, getdir("/TitanServers/Firewall", 0x06020837))
    assert fw[4:12] == bytes.fromhex("0502000300000080"), fw.hex()
    assert "TitanFirewallDetector".encode("utf-16-le") in fw, fw.hex()
    _, camp = exchange(15101, getdir("/StarFleetCommand2/Game/Release", 0x06020E77))
    assert b"class tCampaignInfo" in camp, camp.hex()
    _, det = exchange(15102, bytes.fromhex("0f00050300010 0fc0801204e000000".replace(" ", "")))
    assert det == bytes.fromhex("090005030002000000"), det.hex()

    _, ver = exchange(80, b"GET /motd/vercheck.asp?productid=808&versionuniqueid=2.5.5.2&distid=270"
                          b" HTTP/1.1\r\nHost: patch.example.test\r\n\r\n")
    assert ver.endswith(b"\r\n\r\n\\newver\\0"), ver

    email, nick, password = "testcaptain@example.test", "TestCaptain", "throwaway"
    user = nick + "@" + email
    client = "abcdefghijklmnopqrstuvwxyz012345"
    with socket.create_connection((HOST, 29900), timeout=5) as s:
        hello = s.recv(4096).decode()
        server = hello.split("\\challenge\\")[1].split("\\")[0]
        s.sendall(("\\newuser\\\\email\\%s\\nick\\%s\\password\\%s\\id\\1\\final\\"
                   % (email, nick, password)).encode())
        assert s.recv(4096).startswith(b"\\nur\\"), "new account"
        h = md5(password)
        s.sendall(("\\login\\\\challenge\\%s\\user\\%s\\response\\%s\\port\\6500\\id\\1\\final\\"
                   % (client, user, md5(h + " " * 48 + user + client + server + h))).encode())
        reply = s.recv(4096).decode()
        assert "\\proof\\" + md5(h + " " * 48 + user + server + client + h) + "\\" in reply, reply
    _, vr = exchange(29901, ("\\valid\\\\email\\%s\\final\\" % email).encode())
    assert vr == b"\\vr\\1\\final\\", vr
    print("dynaverse probe: ok")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError) as e:
        print("dynaverse probe: FAIL: %s" % e, file=sys.stderr)
        sys.exit(1)
