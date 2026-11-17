#!/bin/sh
# Downloads and unpacks the UnRAR sources that the helper is built against.
#
# The tarball is pinned by SHA256: the sources are executed as part of the build (and in CI, which
# then has a token with write access), so a tampered download must fail here instead of being
# compiled. When bumping UNRAR_VERSION, update UNRAR_SHA256 as well - the release workflow tells
# you the new hash through the failed checksum, verify it against rarlab.com before pasting it.
set -o pipefail
set -o xtrace
set -e

CUR_DIR=$( cd "$( dirname "$0" )" >/dev/null 2>&1 && pwd )
VENDOR_DIR="${CUR_DIR}/../vendor"
UNRAR_VERSION=7.3.2
UNRAR_SHA256=f61a1160d6e2ebbda2ad353c8e82f62ab4312f97fde23c6c2c982d25e0014f99

mkdir -p "${VENDOR_DIR}"
cd "${VENDOR_DIR}"

wget -N "https://www.rarlab.com/rar/unrarsrc-${UNRAR_VERSION}.tar.gz"
echo "${UNRAR_SHA256}  unrarsrc-${UNRAR_VERSION}.tar.gz" | shasum -a 256 -c -

rm -rf unrar
tar -xzf "unrarsrc-${UNRAR_VERSION}.tar.gz"

# One small local patch on top of the pristine sources: UnRAR opens volumes without O_NONBLOCK, so
# a FIFO (or a blocking device) placed at a volume name would hang the helper. The patch is
# re-checked whenever UNRAR_VERSION changes - if it stops applying, fail loudly instead of building
# an unprotected binary.
patch -p1 -d unrar < "${CUR_DIR}/patches/unrar-nonblock.patch"

echo "UnRAR ${UNRAR_VERSION} sources are in ${VENDOR_DIR}/unrar"
echo "Reminder: distribution must keep the UnRAR license text (see ../LICENSE)."
