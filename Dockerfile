# Reproducible Linux build/test environment for Aurora OS (x86-64 guest).
# Build:  docker build -t aurora-dev .
# Use:    docker run --rm -it -v "$PWD":/src -w /src aurora-dev make && make smoke
# NOTE: this Dockerfile was written for the Linux reference environment but
# has NOT been executed on the development machine (no Docker available
# there); see STATUS.md.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates python3 make clang lld gcc libc6-dev \
        qemu-system-x86 ovmf git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
CMD ["make", "help"]
