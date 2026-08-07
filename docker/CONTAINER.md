# Running Symphony Media Bridge in a container

This directory's per-distro folders (`ubuntu-jammy`, `el8`, …) are **build/CI**
images. For a deployable **runtime** image use the multi-stage `Dockerfile` in
the repository root, which compiles a Release `smb` and ships only the binary
plus the shared libraries it needs.

## Build

```bash
docker build -t smb:latest .
```

The build reproduces the `docker/ubuntu-jammy` toolchain (clang/llvm 12 +
libc++, OpenSSL 3.4, libsrtp 2.6, libmicrohttpd, opus). It is a from-source
build and takes a while on first run. `.git` is part of the build context
because `version/CheckGit.cmake` stamps the git hash into the binary.

## Run

### Recommended: host networking

SMB is a WebRTC **SFU**. It advertises its own reachable IP in ICE candidates
and moves media over UDP. Host networking avoids Docker NAT entirely and gives
the best media performance:

```bash
docker run -d --name smb \
  --network host \
  --cap-add SYS_NICE \
  -v "$PWD/docker/runtime-config.json:/etc/smb/config.json:ro" \
  smb:latest
```

or simply:

```bash
docker compose up -d --build
```

### Bridged (published ports)

If you cannot use host networking, the default config sets
`ice.sharedPorts = 1`, so all media is collapsed onto the single ICE UDP port
and only a handful of ports need publishing:

```bash
docker run -d --name smb \
  --cap-add SYS_NICE \
  -p 8080:8080/tcp \
  -p 10000:10000/udp \
  -p 10500:10500/udp \
  -v "$PWD/docker/runtime-config.json:/etc/smb/config.json:ro" \
  smb:latest
```

In this mode you **must** set `ice.publicIpv4` (see below) — behind the Docker
bridge SMB otherwise advertises an unreachable container-internal address.

## Configuration

The image bakes in `docker/runtime-config.json` at `/etc/smb/config.json`.
Override it by mounting your own file at that path.

| Key | Purpose |
| --- | --- |
| `address` | HTTP API **bind** address. Must be `0.0.0.0` in a container — the code default is `127.0.0.1`, which is loopback-only and unreachable through a port map. |
| `port` | HTTP API port (TCP, default 8080) |
| `ice.singlePort` | Shared ICE/media UDP port (default 10000) |
| `ice.sharedPorts` | `1` = all media on the single port (keeps the port surface small) |
| `ice.publicIpv4` | **Set this** to the IP clients use to reach the host, unless on AWS with `ice.useAwsInfo` |
| `recording.singlePort` | Recording UDP port (default 10500) |
| `logStdOut` | `true` so logs go to the container's stdout |

On AWS EC2 you can instead set `"ice.useAwsInfo": true` to auto-discover the
public IP from instance metadata.

## Notes

- **CAP_SYS_NICE (real-time priority)** — SMB requests `SCHED_FIFO` thread
  priority at startup. Without permission it logs
  `Failed to set thread priority to real-time ... Not permitted` and runs fine
  at normal priority; it is a performance optimization, not a requirement.
  To grant it you need **all** of:
    1. a **rootful** container runtime (Docker, or rootful podman) — under
       *rootless* podman the host kernel denies `SCHED_FIFO` regardless of
       caps, because container-root is an unprivileged user in a user
       namespace (verify with `docker info | grep rootless`);
    2. `--cap-add=SYS_NICE` (bypasses the `RLIMIT_RTPRIO` limit, which defaults
       to 0 in containers); on some setups also pass `--ulimit rtprio=99`;
    3. the process running as root in the container (`--user 0`), since a
       non-root process does not get the capability into its effective set
       without ambient caps.
  The capability is intentionally **not** baked onto the binary with `setcap`:
  a file whose permitted caps fall outside the container's bounding set fails to
  `exec` at all, which would make the image refuse to start unless the cap were
  always present.
- **Non-root by default** — the process runs as the unprivileged `smb` user
  (uid 999). This is the secure default; enabling real-time priority (above)
  means giving that up.
- **Signals** — `smb` is PID 1 via the exec-form entrypoint and shuts down
  cleanly on `SIGINT`/`SIGTERM` (`docker stop`).
- **Health** — `HEALTHCHECK` polls `GET /stats` on the API port. Note: podman
  builds an OCI image by default and prints a warning that it ignores
  `HEALTHCHECK`; it works under Docker, or build with `--format docker`.
