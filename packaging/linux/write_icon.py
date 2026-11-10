#!/usr/bin/env python3
"""Writes the Linux package's icon: the 512x512 PNG inside packaging/macos/SFCOP.icns (its `ic09`
entry), so both platforms show the same icon; when the .icns has none, the placeholder is drawn
again by packaging/macos/make_icon.py.

    python3 packaging/linux/write_icon.py OUT.png
"""

import os
import struct
import sys

MACOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "macos")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_from_icns(data):
    """The 512x512 PNG entry of an .icns file, or None."""
    if data[:4] != b"icns":
        return None
    offset = 8
    while offset + 8 <= len(data):
        kind, length = data[offset:offset + 4], struct.unpack(">I", data[offset + 4:offset + 8])[0]
        if length < 8:
            return None
        body = data[offset + 8:offset + length]
        if kind == b"ic09" and body.startswith(PNG_SIGNATURE):
            return body
        offset += length
    return None


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    with open(os.path.join(MACOS, "SFCOP.icns"), "rb") as stream:
        image = png_from_icns(stream.read())
    if image is None:
        sys.path.insert(0, MACOS)
        import make_icon  # noqa: E402

        image = make_icon.png()
    with open(argv[1], "wb") as stream:
        stream.write(image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
