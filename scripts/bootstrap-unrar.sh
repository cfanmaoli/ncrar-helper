#!/bin/sh
# Downloads and unpacks the UnRAR sources that the helper is built against.
set -o pipefail
set -o xtrace
set -e

CUR_DIR=$( cd "$( dirname "$0" )" >/dev/null 2>&1 && pwd )
VENDOR_DIR="${CUR_DIR}/../vendor"
UNRAR_VERSION=7.3.2

mkdir -p "${VENDOR_DIR}"
cd "${VENDOR_DIR}"

wget -N "https://www.rarlab.com/rar/unrarsrc-${UNRAR_VERSION}.tar.gz"
rm -rf unrar
tar -xzf "unrarsrc-${UNRAR_VERSION}.tar.gz"

echo "UnRAR ${UNRAR_VERSION} sources are in ${VENDOR_DIR}/unrar"
echo "Reminder: distribution must keep the UnRAR license text (see ../LICENSE)."
