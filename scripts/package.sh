#!/bin/sh
# Builds the distributable RAR support component: ncrar-helper.app inside a DMG.
set -o pipefail
set -o xtrace
set -e

CUR_DIR=$( cd "$( dirname "$0" )" >/dev/null 2>&1 && pwd )
ROOT_DIR="${CUR_DIR}/.."
VERSION="${VERSION:-1.0}"
ARCH=$( uname -m )
APP_NAME="ncrar-helper"
APP="${ROOT_DIR}/dist/${APP_NAME}.app"
DMG="${ROOT_DIR}/dist/${APP_NAME}-${VERSION}-macos-${ARCH}.dmg"

cd "${ROOT_DIR}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

rm -rf "${ROOT_DIR}/dist"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"

cp build/ncrar-helper "${APP}/Contents/MacOS/${APP_NAME}"
cp LICENSE LICENSE-MIT README.md "${APP}/Contents/Resources/"
cp docs/protocol.md "${APP}/Contents/Resources/protocol.md"

cat > "${APP}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>info.filesmanager.ncrar-helper</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>RAR Support</string>
    <key>CFBundleDisplayName</key>
    <string>RAR Support for Nimble Commander</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
PLIST

# An ad-hoc signature is enough for a bundle that is only ever executed by the file manager.
codesign --force --deep --sign - "${APP}" 2>/dev/null || true

STAGE="${ROOT_DIR}/dist/stage"
mkdir -p "${STAGE}"
cp -R "${APP}" "${STAGE}/"
ln -s /Applications "${STAGE}/Applications"

hdiutil create -volname "RAR Support for Nimble Commander" \
    -srcfolder "${STAGE}" -fs HFS+ -format UDZO -ov "${DMG}" >/dev/null

rm -rf "${STAGE}"
( cd "${ROOT_DIR}/dist" && shasum -a 256 "$( basename "${DMG}" )" > "$( basename "${DMG}" ).sha256" )
echo "Built ${DMG}"
