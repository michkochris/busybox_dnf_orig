# busybox_dnf

A high-performance, lightweight package management frontend for BusyBox.

`busybox_dnf` provides a minimalist yet powerful interface to `rpm`, handling repository management, metadata parsing, and complex dependency resolution. It is designed specifically for embedded systems, rescue environments, and resource-constrained Fedora/RHEL-based distributions.

---

## Key Features

*   **Greedy Dependency Resolution**: A high-performance resolver that uses batch-processing to resolve complex, nested dependency trees (tested up to 700+ packages) in seconds.
*   **Full Group Support**: Implements `groupinstall` by parsing repository `comps.xml` files, allowing for the installation of entire environments like "Xfce" or "base-x" with a single command.
*   **Advanced Capability Matching**: Robust support for virtual packages and capabilities. It intelligently matches against `Provides:`, architecture-specific symbols, and standard system paths (e.g., `/usr/bin/sh`).
*   **Atomic Transactions**: Large batches of packages are downloaded, staged, and passed to `rpm` in a single transaction for maximum consistency and speed.
*   **Embedded Optimized (Auto-Cleanup)**: Automatically purges downloaded RPM files immediately after installation, ensuring the system storage footprint remains lean.
*   **System Rescue Toolkit**: Specialized commands for recovery:
    *   `verify`: Check package sanity, existing files, working symlinks and check dependencies.
    *   `reinstall`: Restore package files through standard RPM paths.
    *   `md5check`: Verify package integrity by checking every installed file against its recorded checksum (MD5/SHA).
    *   `rescue-install`: Failsafe extraction using BusyBox internal `cpio` logic, bypassing the RPM database to repair core system libraries.
*   **Smart Metadata Caching**: Efficient state machine for fetching and caching repository metadata (`metalink`, `repomd.xml`, `primary.xml`, `comps.xml`) with a 6-hour refresh cycle.

---

## Usage

```text
Usage: dnf [-yv] COMMAND [PACKAGE...]

High-level package management frontend

Options:
    -y, --assumeyes     Answer yes for all questions
    -v, --verbose       Verbose output for debugging

Commands:
    update              Update repository metadata cache
    upgrade             Upgrade the system
    check-update        Check for available package updates
    install             Install new packages
    remove              Remove packages
    reinstall           Reinstall packages (restores files)
    groupinstall        Install all packages in a group (e.g. "Xfce")
    rescue-install      Install packages bypassing rpm (uses internal cpio to /)
    verify              Verify package sanity (check status, deps, and files)
    md5check            Verify package integrity (checks file hashes)
    search              Search for a package
    info                Display details about a package
```

### Examples

```bash
# Update metadata and upgrade all packages
./busybox dnf upgrade

# Install a full desktop environment
./busybox dnf groupinstall "Xfce Desktop"

# Search for a specific package
./busybox dnf search network-manager

# Verify system sanity
./busybox dnf verify glibc

# perform smart inquiry via: (capabilities)
./busybox dnf reinstall "libmagic.so.1"
./busybox dnf verify "libmagic.so.1"
./busybox dnf md5check "libz.so.1"
```

---

## Repository Configuration

`busybox_dnf` utilizes standard `.repo` files located in `/etc/yum.repos.d/`.

*   **Format**: Supports standard `[id]`, `baseurl=`, `metalink=`, and `mirrorlist=` entries.
*   **Variable Substitution**: Automatically handles `$releasever` and `$basearch` variables within repo files.
*   **Detection**: Automatically detects host architecture (e.g., x86_64, aarch64) and distribution version.

---

## Build Instructions

### 1. Host Requirements (Fedora/RHEL)
```bash
chmod +x busybox_dnf/fedora-depends.sh
./busybox_dnf/fedora-depends.sh
```

# 2. Get source
wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
tar -xvjf busybox-1.37.0.tar.bz2
cd busybox-1.37.0

### 3. Integration
Clone this repository into the root of your BusyBox source tree:
```bash
git clone https://github.com/michkochris/busybox_dnf_orig busybox_dnf/
```

### 4. Build
Use the automated integration script:
```bash
./busybox_dnf/busybox_build.sh
```

### 5. Manual Integration:
```bash
patch -p0 < busybox_dnf/busybox_dnf.patch
make menuconfig  # Enable 'dnf' under 'Applets' -> 'Busybox DNF'
make
```
----------------

## License

This project is licensed under the **GNU General Public License, version 2 (GPLv2)**, matching the license of the BusyBox project.

----------------

## Contact

For feedback, bug reports, or inquiries:
michkochris@gmail.com | runepkg@gmail.com
