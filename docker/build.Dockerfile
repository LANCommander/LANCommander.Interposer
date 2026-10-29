# Linux build environment for LANCommander Interposer: x64 and x86 (multilib),
# dependencies through vcpkg.
#
# Debian 12 matches the LANCommander game server base image, so the shared
# object built here runs on its glibc (and on anything newer).
#
#   docker build -f docker/build.Dockerfile -t lancommander-interposer-build docker
#   docker run --rm -v "$PWD:/src" -v interposer-vcpkg-cache:/root/.cache/vcpkg lancommander-interposer-build \
#       sh -c "cmake --preset linux-x64 && cmake --build --preset linux-x64-release && out/linux-x64/bin/Release/LANCommander.Interposer.Tests"
FROM debian:12

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-multilib gcc-multilib \
        cmake ninja-build pkg-config \
        git curl ca-certificates zip unzip tar \
    && rm -rf /var/lib/apt/lists/*

# Pinned, so a rebuild of the image resolves the same dependency versions. The
# same commit LANCommander.Comms builds with.
ARG VCPKG_COMMIT=817892b0ecbc6af994dd6df999c73faf795adbf2
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /src
