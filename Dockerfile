# syntax=docker/dockerfile:1
#
# Multi-stage production build for the Symphony Media Bridge (SMB).
#
#   Stage 1 (builder): reproduces the docker/ubuntu-jammy build recipe
#                      (clang/llvm 12 + libc++, openssl 3.4, libsrtp 2.6,
#                      libmicrohttpd, opus) and compiles a Release `smb` binary.
#   Stage 2 (runtime): a slim ubuntu:jammy image carrying only the `smb`
#                      binary and the shared libraries it needs at run time.
#
# Build:   docker build -t smb:latest .
# Run:     see docker-compose.yml or docker/CONTAINER.md
#
# ---------------------------------------------------------------------------
# Stage 1: builder
# ---------------------------------------------------------------------------
FROM ubuntu:jammy AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update --fix-missing && apt-get -y upgrade \
    && apt-get -y install git wget cmake xz-utils libz-dev build-essential \
    && rm -rf /var/lib/apt/lists/*

# openssl 3.4.0 (installed to /usr/local/lib64)
RUN cd /tmp && wget -q https://github.com/openssl/openssl/releases/download/openssl-3.4.0/openssl-3.4.0.tar.gz \
    && tar xfz openssl-3.4.0.tar.gz && rm openssl-3.4.0.tar.gz \
    && cd openssl-3.4.0 && ./config && make -j"$(nproc)" && make install_sw \
    && rm -rf /tmp/openssl-3.4.0

# cmake 3.30 (jammy's apt cmake is too old for the build)
RUN apt-get -y remove cmake || true \
    && cd /tmp && wget -q https://cmake.org/files/v3.30/cmake-3.30.0.tar.gz && tar xfz cmake-3.30.0.tar.gz \
    && cd cmake-3.30.0 && ./bootstrap --parallel="$(nproc)" && make -j"$(nproc)" && make install \
    && rm -rf /tmp/cmake-3.30.0 /tmp/cmake-3.30.0.tar.gz

# libsrtp 2.6.0 (statically linked into smb; no runtime .so needed)
RUN cd /tmp && git clone --depth 1 --branch v2.6.0 https://github.com/cisco/libsrtp \
    && cd libsrtp \
    && PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig ./configure --enable-openssl \
    && make -j"$(nproc)" && make install \
    && rm -rf /tmp/libsrtp

# clang/llvm 12 + libc++ (SMB only compiles with clang + libc++)
RUN cd /tmp \
    && wget -q -O clang.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-12.0.1/clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz \
    && tar xf clang.tar.xz \
    && cd clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* \
    && cp -rn * /usr/local \
    && ln -sf /usr/local/bin/lld /usr/local/bin/ld \
    && cd /tmp && rm -rf clang+llvm-12.0.1-x86_64-linux-gnu-ubuntu-* clang.tar.xz

# libmicrohttpd 0.9.73
RUN cd /tmp && wget -q https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-0.9.73.tar.gz \
    && tar xfz libmicrohttpd-0.9.73.tar.gz && rm libmicrohttpd-0.9.73.tar.gz \
    && cd libmicrohttpd-0.9.73 && ./configure --disable-https && make -j"$(nproc)" && make install \
    && rm -rf /tmp/libmicrohttpd-0.9.73

# opus 1.3.1
RUN cd /tmp && wget -q https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz \
    && tar xfz opus-1.3.1.tar.gz && rm opus-1.3.1.tar.gz \
    && cd opus-1.3.1 && ./configure && make -j"$(nproc)" && make install \
    && rm -rf /tmp/opus-1.3.1

RUN ldconfig

# --- compile SMB (out-of-source Release build) ---
WORKDIR /src
COPY . /src
# .git is required for version/CheckGit.cmake to stamp git_version.h
RUN git config --global --add safe.directory /src

RUN cmake -S /src -B /src/build \
        -DENABLE_LEGACY_API=ON \
        -DENABLE_LIBATOMIC=ON \
        -D_CMAKE_TOOLCHAIN_PREFIX=llvm- \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=/usr/local/bin/clang \
        -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ \
        -G "Unix Makefiles" \
    && cmake --build /src/build --target smb -j"$(nproc)"

# Gather the exact runtime shared libraries (mirrors docker/ubuntu-jammy/buildscript.sh)
RUN mkdir -p /out/libs \
    && cp /src/build/smb /out/smb \
    && cp /usr/lib/x86_64-linux-gnu/libatomic.so.1 /out/libs/ \
    && cp /usr/local/lib/libc++.so.1            /out/libs/ \
    && cp /usr/local/lib/libc++abi.so.1         /out/libs/ \
    && cp /usr/local/lib64/libssl.so.3          /out/libs/ \
    && cp /usr/local/lib64/libcrypto.so.3       /out/libs/ \
    && cp /usr/local/lib/libmicrohttpd.so.12    /out/libs/ \
    && cp /usr/local/lib/libopus.so.0           /out/libs/

# ---------------------------------------------------------------------------
# Stage 2: runtime
# ---------------------------------------------------------------------------
FROM ubuntu:jammy AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# curl: HEALTHCHECK probe. ca-certificates: handy if the deployment fetches
# anything. Kept minimal.
RUN apt-get -y update --fix-missing \
    && apt-get -y install --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# Runtime shared libraries
COPY --from=builder /out/libs/ /usr/local/lib/
RUN ldconfig

# Application binary + default config
COPY --from=builder /out/smb /opt/smb/smb
COPY docker/runtime-config.json /etc/smb/config.json

# Real-time thread scheduling (SCHED_FIFO): SMB requests it at startup. To
# enable it, start the container with `--cap-add=SYS_NICE` (see CONTAINER.md).
# The capability is NOT baked onto the binary with setcap on purpose — a file
# whose permitted caps fall outside the container's bounding set fails to exec
# at all, so that would make the image refuse to start without the cap. Without
# SYS_NICE, SMB logs a warning and runs at normal priority.

# Non-root runtime user
RUN useradd --system --no-create-home --shell /usr/sbin/nologin smb
USER smb

# HTTP API. Media/ICE ports are UDP and depend on the config; see CONTAINER.md.
EXPOSE 8080/tcp
EXPOSE 10000/udp
EXPOSE 10500/udp

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["curl", "-fsS", "-o", "/dev/null", "http://127.0.0.1:8080/stats"]

# exec form so SIGINT/SIGTERM reach smb directly (it shuts down cleanly on SIGINT)
ENTRYPOINT ["/opt/smb/smb"]
CMD ["/etc/smb/config.json"]
