#!/usr/bin/env python3
"""Copies stdin to stdout with the lift's input folders replaced, for a log line shown on GitHub.

    ... | python3 packaging/scrub.py

SFCOP_INSTALL_DIR becomes <install> and SFCOP_PCRECOMP_DIR <pcrecomp>, each as given, as its real
path, and the install's parent folder as <install-parent>; longer forms are replaced first.
"""

import os
import sys


def replacements(environ):
    pairs = []
    for name, label in (("SFCOP_INSTALL_DIR", "<install>"), ("SFCOP_PCRECOMP_DIR", "<pcrecomp>")):
        value = environ.get(name, "").rstrip("/")
        if not value:
            continue
        for form in {value, os.path.realpath(value)}:
            pairs.append((form, label))
        if name == "SFCOP_INSTALL_DIR":
            parent = os.path.dirname(value)
            for form in {parent, os.path.realpath(parent)}:
                if form not in ("", "/"):
                    pairs.append((form, "<install-parent>"))
    return sorted(pairs, key=lambda pair: -len(pair[0]))


def scrub(text, environ):
    for value, label in replacements(environ):
        text = text.replace(value, label)
    return text


if __name__ == "__main__":
    sys.stdout.write(scrub(sys.stdin.read(), os.environ))
