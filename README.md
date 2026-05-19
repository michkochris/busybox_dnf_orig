# busybox_dnf

> [!IMPORTANT]
> **This project is currently under active development.** 
> Expect frequent breaking changes and incomplete documentation.

What is busybox_dnf:

  `busybox_dnf` is a high-level package management frontend for RPM-based
  systems. It is a lightweight interface to `rpm` which handles repository
  management and dependency resolution. It provides minimalist replacements
  for the features found in DNF (Dandified YUM).

  Built with size-optimization and system recovery in mind, `busybox_dnf`
  integrates directly into the BusyBox Kbuild system and utilizes internal
  applets for critical operations. This makes it a robust choice for embedded
  systems, rescue disks, and minimalist Fedora/RHEL-based environments where
  a full `dnf` installation is too heavy or when the system manager is broken.

----------------

Features:

  * Professional UI: Familiar output format with repository sync status and
    real-time progress tracking via a terminal-aware progress bar.
  * Dependency Resolution: Automatically resolves recursive dependencies for
    RPM packages, including support for Virtual Packages (via `Provides:`).
  * System Rescue Toolkit: Specialized commands like `reinstall`, `verify`,
    and `rescue-install` for repairing corrupted systems.
  * Disk Usage Reporting: Accurate calculation of disk space used or freed
    after operations, tracking individual package installed-sizes.
  * Manual Extraction: `rescue-install` bypasses broken `rpm` instances by
    using internal `cpio` and `rpm2cpio` logic to extract payloads directly to `/`.
  * RPM-Style Versioning: Robust comparison logic supporting epochs,
    revisions, and RPM-specific sorting rules.
  * Lightweight Defaults: Optimized for resource-constrained environments,
    focusing on core dependencies.

----------------
Repository Configuration:

  `busybox_dnf` requires valid repository configuration files to function. It
  looks for `.repo` files in `/etc/yum.repos.d/`. 

  * **Environment Support**: If you are running `busybox_dnf` on a non-RPM
    distribution, you **must** manually create
    `/etc/yum.repos.d/` and provide at least one valid repository file.
  * **Repository Files**: Each `.repo` file should follow the standard YUM/DNF
    format (with `[id]`, `baseurl=` or `metalink=`, and `enabled=1`).
  * **Architecture**: The applet automatically detects your system architecture
    and version, but these can be overridden via environment variables if
    needed for cross-environment recovery.

----------------
Using busybox_dnf:
```text
  Usage: dnf [-y] COMMAND [PACKAGE...]

  High-level package management frontend

  Options:
      -y, --assumeyes     Answer yes for all questions

  Commands:
      check-update        Check for available package updates
      update              Update the system
      install             Install new packages
      remove              Remove packages
      reinstall           Reinstall packages (restores files)
      rescue-install      Install packages bypassing rpm (uses internal cpio to /)
      verify              Verify package sanity (check status, deps, and files)
      search              Search for a package (alphabetically sorted)
      info                Display details about a package
```
  Examples:
```bash
./busybox dnf check-update
./busybox dnf install vim
./busybox dnf verify kernel
./busybox dnf rescue-install glibc
```
----------------

Rescue Workflow:

  `busybox_dnf` is designed for system recovery on RPM-based distributions.
  When critical libraries or the package manager itself is corrupted:

  1. Use `verify` to identify broken dependencies, missing files, or
     incorrect permissions.
  2. Use `reinstall` to restore files via standard `rpm` paths.
  3. Use `rescue-install` as a failsafe to extract critical packages
     directly to the root filesystem using BusyBox internal applets,
     bypassing the RPM database if necessary.

----------------

Build Instructions:

  To integrate `busybox_dnf` into your BusyBox build:

  0. Optionally wget busybox source and install dependencies
```bash
# Install host requirements (Fedora)
chmod +x busybox_dnf/fedora-depends.sh
./busybox_dnf/fedora-depends.sh

# Get source
wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
tar -xvjf busybox-1.37.0.tar.bz2
cd busybox-1.37.0
```
  1. Clone this repository into the root of your BusyBox source tree:
```bash
git clone https://github.com/michkochris/busybox_dnf_orig busybox_dnf/
```
  2. Automated Build:
     The included script will integrate the applet, configure, and compile:
```bash
./busybox_dnf/busybox_build.sh
```
  3. Manual Integration:
```bash
patch -p0 < busybox_dnf/busybox_dnf.patch
make menuconfig  # Enable 'dnf' under 'Applets' -> 'Busybox DNF'
make
```
----------------

License:

  This project is licensed under the GNU General Public License, version 2
  (GPLv2), matching the license of the BusyBox project.

----------------

Contact:

  For feedback, bug reports, or inquiries:
  michkochris@gmail.com | runepkg@gmail.com
