# Multi-stage Dockerfile for building OBS PingPong Loop Filter
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
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
    libxcb-shape0-dev \
    libxcb-composite0-dev \
    libxcb-image0-dev \
    libxcb-util-dev \
    libxcb1-dev \
    libxcomposite-dev \
    libxinerama-dev \
    libv4l-dev \
    libvlc-dev \
    libfreetype6-dev \
    libfontconfig-dev \
    libjack-jackd2-dev \
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
    qtbase5-dev \
    qtbase5-private-dev \
    libqt5svg5-dev \
    qtwayland5 \
    wget \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp

# Download and build OBS Studio (we need libobs headers and libraries)
RUN git clone --recursive --depth 1 --branch 31.1.1 https://github.com/obsproject/obs-studio.git && \
    cd obs-studio && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_BROWSER=OFF \
        -DENABLE_VST=OFF \
        -DENABLE_SCRIPTING=OFF \
        -DENABLE_PIPEWIRE=OFF \
        -DENABLE_WAYLAND=OFF \
        -DENABLE_AJA=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DLINUX_PORTABLE=OFF \
        -DENABLE_UNIT_TESTS=OFF \
        -DENABLE_FRONTEND_API=OFF \
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
FROM ubuntu:22.04 AS runtime

RUN mkdir -p /output

# Copy the built plugin
COPY --from=builder /plugin/build/*.so /output/

WORKDIR /output
CMD ["ls", "-la"]