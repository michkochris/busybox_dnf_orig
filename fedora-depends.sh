#!/usr/bin/env bash
# fedora-depends.sh - Install BusyBox build requirements on Fedora

set -e

echo "Updating DNF cache..."
sudo dnf makecache

echo "Installing build dependencies..."
# Core build tools
sudo dnf install -y \
    binutils \
    bison \
    flex \
    gcc \
    gcc-c++ \
    gawk \
    make \
    texinfo \
    xz \
    libtool \
    kernel-headers \
    kernel-devel \
    glibc-devel \
    glibc-static \
    ncurses-devel \
    openssl-devel \
    elfutils-libelf-devel \
    libselinux-devel \
    patch \
    zstd

echo "--------------------------------------------------"
echo "Build dependencies installed successfully!"
echo "Note: If you plan on cross-compiling, you may need additional packages."
echo "--------------------------------------------------"
