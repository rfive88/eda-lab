FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Minimal tools needed to run the dependency installer itself
RUN apt-get update && apt-get install -y --no-install-recommends \
    git curl ca-certificates sudo build-essential \
    && rm -rf /var/lib/apt/lists/*

# Copy just the dependency installer + its support files from the pinned submodule
COPY third_party/openroad/etc/DependencyInstaller.sh /tmp/DependencyInstaller.sh
RUN chmod +x /tmp/DependencyInstaller.sh && /tmp/DependencyInstaller.sh -all

# Dev tooling on top of OpenROAD's own deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build clang clangd gdb lldb \
    python3 python3-pip python3-venv \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --break-system-packages pybind11 typer

WORKDIR /workspace
