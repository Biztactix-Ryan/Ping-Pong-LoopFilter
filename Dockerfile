# Multi-stage Dockerfile for building OBS PingPong Loop Filter
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Build argument for OBS version (default to 31.1.1)
ARG OBS_VERSION=31.1.1

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    ninja-build \
    pkg-config \
    libcurl4-openssl-dev \
    libavcodec-dev \
    libavdevice-dev \
    libavfilter-dev \
    libavformat-dev \
    libavutil-dev \
    libswresample-dev \
    libswscale-dev \
    libx264-dev \
    libx11-dev \
    libxcb-xinerama0-dev \
    libxcb-shm0-dev \
    libxcb-randr0-dev \
    libxcb-xfixes0-dev \
    libxcb-composite0-dev \
    libxcb1-dev \
    libxcomposite-dev \
    libxinerama-dev \
    libv4l-dev \
    libfreetype6-dev \
    libfontconfig-dev \
    libpulse-dev \
    libasound2-dev \
    libgl1-mesa-dev \
    libjansson-dev \
    libluajit-5.1-dev \
    libspeexdsp-dev \
    libwayland-dev \
    libpci-dev \
    swig \
    python3-dev \
    libqt6svg6-dev \
    qt6-base-dev \
    qt6-base-private-dev \
    qt6-wayland \
    libpipewire-0.3-dev \
    libxss-dev \
    libgbm-dev \
    libdrm-dev \
    libudev-dev \
    librist-dev \
    libsrt-openssl-dev \
    libwebsocketpp-dev \
    libasio-dev \
    libmbedtls-dev \
    && rm -rf /var/lib/apt/lists/*

# Install newer CMake (Ubuntu 24.04 has 3.28+)
RUN apt-get update && apt-get install -y cmake && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp

# Download and build OBS Studio (we need libobs headers and libraries)
RUN git clone --recursive --depth 1 --branch ${OBS_VERSION} https://github.com/obsproject/obs-studio.git && \
    cd obs-studio && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_BROWSER=OFF \
        -DENABLE_VST=OFF \
        -DENABLE_SCRIPTING=OFF \
        -DENABLE_AJA=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DLINUX_PORTABLE=OFF \
        -DENABLE_UNIT_TESTS=OFF \
        -DENABLE_UI=OFF \
        -G Ninja && \
    ninja && \
    ninja install

# Copy plugin source
COPY . /plugin
WORKDIR /plugin

# Build the plugin
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -G Ninja && \
    cmake --build build

# Output stage - extract just the built plugin
FROM ubuntu:24.04 AS runtime

RUN mkdir -p /output

# Copy the built plugin
COPY --from=builder /plugin/build/*.so /output/

WORKDIR /output
CMD ["ls", "-la"]