#!/bin/bash
# clean_up.sh - Cleanup script for busybox_dnf

echo "Cleaning up busybox_dnf temporary files..."

# Remove build artifacts
rm -f *.o *.a
rm -f built-in.o
rm -f .*.cmd
rm -f busybox_dnf.patch

# Remove local metadata if any (usually in /var/cache/dnf, but safe to clear local strays)
rm -f *.xml *.xml.gz *.xml.xz *.xml.zst

echo "Cleanup complete."
