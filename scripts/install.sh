#!/bin/sh
# Installs the RAR support component into the location Nimble Commander-Fork looks at.
set -e

DEST="${HOME}/Library/Application Support/Nimble Commander-Fork"
BIN="${DEST}/ncrar-helper"

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "${BIN}"
    echo "Removed ${BIN}"
    exit 0
fi

HERE=$( cd "$( dirname "$0" )" && pwd )
mkdir -p "${DEST}"
cp "${HERE}/ncrar-helper" "${BIN}"
chmod +x "${BIN}"
echo "Installed ${BIN}"
echo "Nimble Commander-Fork will use it the next time a RAR archive is opened."
echo "To remove it: $0 --uninstall"
