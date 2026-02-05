# syntax=docker/dockerfile:1

FROM rust:1.78-bookworm AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        ninja-build \
        pkg-config \
        git \
        curl \
        unzip \
        zip \
        tar \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        libglib2.0-dev \
        libgirepository1.0-dev \
        libgdk-pixbuf2.0-dev \
        libcairo2-dev \
        libpango1.0-dev \
        libgraphene-1.0-dev \
        libgtk-4-dev \
        libatk1.0-dev \
        librdmacm-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh --disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /src
COPY . .

RUN /src/.devcontainer/scripts/common/libfabric/install.sh

RUN cargo build -p gst-mxl-rs --release --manifest-path /src/mxl/rust/Cargo.toml
RUN cargo build -p multiviewer-mf --release

FROM debian:trixie-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-tools \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/target/release/multiviewer-mf /app/multiviewer-mf
COPY --from=builder /src/mxl/rust/target/release/libgstmxl.so /app/
COPY --from=builder /src/mxl/rust/target/release/build/mxl-sys-*/out/build/lib/libmxl.so* /app/
COPY --from=builder /src/mxl/rust/target/release/build/mxl-sys-*/out/build/lib/internal/*.so* /app/
COPY config/multiviewer.json /app/config/multiviewer.json

ENV GST_PLUGIN_PATH=/app
ENV LD_LIBRARY_PATH=/app

ENTRYPOINT ["/app/multiviewer-mf"]
CMD ["--config", "/app/config/multiviewer.json"]
