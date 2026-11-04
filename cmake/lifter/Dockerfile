# Sandbox for running pcrecomp's Python tools (third-party code) on the game exe: lift.sh runs
# them with --network none, with only the lift directory writable. Same image as the P0-4 spike's
# (spike/lifter/docker/Dockerfile), with the base pinned: lift.sh tags it sfcop-lifter-py:<the first
# 12 hex digits of this file's SHA-256>.
# python:3.13-slim (Python 3.13.18, Debian trixie), the multi-platform index digest of 2026-10-28.
FROM python:3.13-slim@sha256:ae14d4ce8771606477a81446e7ef429edd6762305fc50c84507c1041fa8285d1
RUN pip install --no-cache-dir capstone==5.0.7 pefile==2024.8.26 unicorn==2.1.4
WORKDIR /w
