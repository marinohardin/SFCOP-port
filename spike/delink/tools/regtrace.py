#!/usr/bin/env python3
"""Compares the registry keys and values two boots touched, from WINEDEBUG=+reg logs.

The Mac's screen may be locked, so a screenshot is not always possible; the registry trace
shows how far start-up got. Both logs come from boot.sh runs (labels differ, so the exe name
is replaced by <exe>). Handles and buffer addresses are dropped; the order of first use is
kept per log, and the comparison is of the sets, since threads interleave differently. Wine
names the display adapter's video key with a GUID it makes up per session (lowercase, unlike
the uppercase COM GUIDs), so lowercase GUIDs become <session-guid>.

Usage: regtrace.py <wine.log A> <wine.log B> <label A> <label B>
"""
import re
import sys

SESSION_GUID = re.compile(r'\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}')
PAT = re.compile(r'trace:reg:(NtOpenKeyEx|NtCreateKey|RegQueryValueEx[AW]) \((?:0x)?[0-9A-Fa-f]+,(L?"[^"]*"|\(null\))')


def names(path, label):
    seen = []
    for line in open(path, encoding='latin-1'):
        m = PAT.search(line)
        if m:
            item = '%s %s' % (m.group(1).replace('RegQueryValueExA', 'RegQueryValueEx').replace('RegQueryValueExW', 'RegQueryValueEx'),
                              SESSION_GUID.sub('<session-guid>', m.group(2).replace(label, '<exe>')))
            if item not in seen:
                seen.append(item)
    return seen


def main(a, b, la, lb):
    na, nb = names(a, la), names(b, lb)
    sa, sb = set(na), set(nb)
    print('%s: %d distinct keys/values, last first-use: %s' % (la, len(na), na[-1]))
    print('%s: %d distinct keys/values, last first-use: %s' % (lb, len(nb), nb[-1]))
    for x in sorted(sa - sb):
        print('only in %s: %s' % (la, x))
    for x in sorted(sb - sa):
        print('only in %s: %s' % (lb, x))
    return 0 if sa == sb else 1


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
