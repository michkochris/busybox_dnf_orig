#!/bin/bash
# busybox_build.sh - Automated BusyBox build with DNF support

set -e

# Detect if we are inside the busybox_dnf directory or at the root
if [ -f "Makefile" ] && [ -d "busybox_dnf" ]; then
    ROOT_DIR="."
elif [ -f "../Makefile" ] && [ -d "../busybox_dnf" ]; then
    ROOT_DIR=".."
else
    echo "Error: Could not find BusyBox root directory."
    echo "Please run this script from the BusyBox root or from the 'busybox_dnf' subdirectory."
    exit 1
fi

cd "$ROOT_DIR"

echo "Starting BusyBox build process in $(pwd)..."

# 0. Integrate into build system if not already done
if ! grep -q "busybox_dnf/Config.in" Config.in; then
    echo "Integrating DNF into Config.in..."
    if grep -q "sysklogd/Config.in" Config.in; then
        sed -i '/sysklogd\/Config.in/a source busybox_dnf/Config.in' Config.in
    else
        echo "source busybox_dnf/Config.in" >> Config.in
    fi
fi

if ! grep -q "busybox_dnf/" Makefile; then
    echo "Integrating DNF into Makefile..."
    # Using a more robust sed pattern to insert busybox_dnf/ into the libs-y list
    sed -i '/libs-y[[:space:]]*:=/a \\t\tbusybox_dnf/ \\' Makefile
fi

# 1. Generate default configuration
echo "Generating default configuration..."
make defconfig

# 2. Enable DNF applet
echo "Configuring DNF applet..."
sed -i 's/^# CONFIG_DNF is not set/CONFIG_DNF=y/' .config
if ! grep -q "^CONFIG_DNF=y" .config; then
    echo "CONFIG_DNF=y" >> .config
fi

# 3. Ensure dependencies are met
sed -i 's/^# CONFIG_WGET is not set/CONFIG_WGET=y/' .config
sed -i 's/^# CONFIG_RPM is not set/CONFIG_RPM=y/' .config
sed -i 's/^# CONFIG_RPM2CPIO is not set/CONFIG_RPM2CPIO=y/' .config
sed -i 's/^# CONFIG_CPIO is not set/CONFIG_CPIO=y/' .config
sed -i 's/^# CONFIG_GZIP is not set/CONFIG_GZIP=y/' .config

# Disable features that might fail to compile due to kernel header mismatches
echo "Disabling problematic features..."
sed -i 's/CONFIG_FEATURE_COMPRESS_USAGE=y/# CONFIG_FEATURE_COMPRESS_USAGE is not set/' .config
sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config

# 4. Finalize configuration
make silentoldconfig

# 5. Compile
echo "Compiling BusyBox..."
if make -j$(nproc); then
    chmod a+x busybox || true
    echo "--------------------------------------------------"
    echo "Build successful!"
    echo "The 'busybox' binary has been created in the current directory."
    echo "You can test the new applet using: ./busybox dnf"
    echo "--------------------------------------------------"
else
    echo "--------------------------------------------------"
    echo "Error: Build failed!"
    echo "--------------------------------------------------"
    exit 1
fi
