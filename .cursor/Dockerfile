FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# System toolchain + raylib desktop (GLFW/X11) build dependencies, OpenGL/Mesa
# headers, the Mesa software rasterizer (llvmpipe) for headless GPU-less
# rendering, and Xvfb + ffmpeg so examples can run and be captured without a
# physical display. Package list mirrors the raylib Linux CI workflow plus the
# extras needed to actually run examples headlessly.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        pkg-config \
        sudo \
        libglfw3 \
        libglfw3-dev \
        libx11-dev \
        libxcursor-dev \
        libxrandr-dev \
        libxinerama-dev \
        libxi-dev \
        libxext-dev \
        libxfixes-dev \
        libwayland-dev \
        libxkbcommon-dev \
        libgl1-mesa-dev \
        libglu1-mesa-dev \
        libgl1-mesa-dri \
        mesa-utils \
        libasound2-dev \
        xvfb \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# Ensure the default "ubuntu" user exists (present on recent noble images) and
# has passwordless sudo for convenience during development.
RUN if ! id ubuntu >/dev/null 2>&1; then useradd -m -s /bin/bash ubuntu; fi \
    && echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu

# Headless rendering defaults inherited by every shell in the container:
# render on the virtual X display started by the environment `start` command,
# and force Mesa's software rasterizer since the VM has no GPU.
ENV DISPLAY=:99
ENV LIBGL_ALWAYS_SOFTWARE=1

USER ubuntu
