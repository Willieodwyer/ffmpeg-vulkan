FROM ubuntu:24.04

# Install build packages, Update & upgrade with these 
RUN apt-get update && \
    apt-get upgrade -y && \    
    apt-get install -y \
    jq \
    unzip \
    curl \
    build-essential \
    cmake \
    gcc \
    g++ \
    git \
    lua5.4 \
    ccache \
    libc6-dev \
    gdb \
    vulkan-tools

# Install x64 deps 
RUN apt-get install -y \
    libavcodec-dev \
    libavformat-dev \
    libswscale-dev \
    libavutil-dev \
    libswresample-dev \
    libavdevice-dev \
    libavfilter-dev \
    libspeex-dev \
    libspeexdsp-dev \
    libjpeg-dev \
    libturbojpeg0-dev \
    libv4l-dev \
    libasound2-dev \
    zlib1g-dev \
    libjson-c-dev \
    liblzma-dev \
    libssl-dev \
    libspdlog-dev \
    libsqlite3-dev \
    libvulkan-dev

# This step is necessary as the package fails to install with an error while "trying to overwrite shared '/usr/bin/curl-config'"
RUN apt-get install -f -o Dpkg::Options::="--force-overwrite" libcurl4-openssl-dev

ENV PATH="/usr/lib/ccache:$PATH"
ENV ANV_VIDEO_DECODE=1
ENV RADV_PERFTEST=video_decode

# Command to run when the container starts
CMD ["/bin/bash"]

