#!/bin/bash
# Copy build outputs to a staging directory for release packaging.
# Called by Buildroot after images are built.
# $BINARIES_DIR = buildroot/output/images/

set -e

STAGING="${BINARIES_DIR}/3ds-release"
mkdir -p "${STAGING}"

cp "${BINARIES_DIR}/Image"       "${STAGING}/Image"
cp "${BINARIES_DIR}/rootfs.ext4" "${STAGING}/rootfs.ext2"

echo "Release assets ready in ${STAGING}/"
echo "  Image:      $(du -h "${STAGING}/Image"      | cut -f1)"
echo "  rootfs.ext2: $(du -h "${STAGING}/rootfs.ext2" | cut -f1)"
