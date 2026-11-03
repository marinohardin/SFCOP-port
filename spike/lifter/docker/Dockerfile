# Sandbox for running pcrecomp's Python tools (third-party code) on the game exe.
# The tools run with --network none and only work/lifter mounted; see ../README.md.
FROM python:3.13-slim
RUN pip install --no-cache-dir capstone==5.0.7 pefile==2024.8.26 unicorn==2.1.4
WORKDIR /w
