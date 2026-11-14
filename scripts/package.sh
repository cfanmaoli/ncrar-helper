#!/bin/sh
# Builds the distributable RAR support component: the helper binary plus its license and docs.
set -o pipefail
set -o xtrace
set -e

CUR_DIR=$( cd "$( dirname "$0" )" >/dev/null 2>&1 && pwd )
ROOT_DIR="${CUR_DIR}/.."
VERSION="${VERSION:-1.0}"
ARCH=$( uname -m )
NAME="ncrar-helper-${VERSION}-macos-${ARCH}"
DIST="${ROOT_DIR}/dist"

cd "${ROOT_DIR}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

rm -rf "${DIST}"
mkdir -p "${DIST}/${NAME}"
cp build/ncrar-helper "${DIST}/${NAME}/"
cp LICENSE README.md "${DIST}/${NAME}/"
cp scripts/install.sh "${DIST}/${NAME}/"
cp docs/protocol.md "${DIST}/${NAME}/"

# An ad-hoc signature keeps macOS from complaining about an unsigned command line tool.
codesign --force --sign - "${DIST}/${NAME}/ncrar-helper" 2>/dev/null || true

cd "${DIST}"
zip -q -r "${NAME}.zip" "${NAME}"
shasum -a 256 "${NAME}.zip" > "${NAME}.zip.sha256"
echo "Built ${DIST}/${NAME}.zip"
