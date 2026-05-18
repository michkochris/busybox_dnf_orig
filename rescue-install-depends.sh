#!/usr/bin/env bash
# rescue-install-depends.sh - Uses the dnf applet to install build deps (recovery testing)

# Ensure the applet is built
if [ ! -f "./busybox" ]; then
    echo "Error: ./busybox not found. Please build it first."
    exit 1
fi

./busybox dnf update

# List of packages needed for BusyBox development
./busybox dnf rescue-install \
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
    glibc-devel \
    glibc-static \
    ncurses-devel \
    openssl-devel \
    elfutils-libelf-devel
