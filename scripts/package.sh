#!/bin/bash
# Builds the distributable RAR support component: ncrar-helper.app inside a DMG.
set -o pipefail
set -o xtrace
set -e

CUR_DIR=$( cd "$( dirname "$0" )" >/dev/null 2>&1 && pwd )
ROOT_DIR="${CUR_DIR}/.."
VERSION="${VERSION:-$( cat "${ROOT_DIR}/VERSION" )}"
# Fixed rather than uname -m: the build below pins CMAKE_OSX_ARCHITECTURES to arm64, so the DMG
# must be named after what is inside it even when packaged on an Intel host.
ARCH=arm64
APP_NAME="ncrar-helper"
APP="${ROOT_DIR}/dist/${APP_NAME}.app"
DMG="${ROOT_DIR}/dist/${APP_NAME}-${VERSION}-macos-${ARCH}.dmg"

cd "${ROOT_DIR}"
# The file manager only ships arm64 builds, so the component is pinned to the same architecture
# instead of silently following whatever machine happens to package it.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
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
    <string>${VERSION}</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
PLIST

# An ad-hoc signature is what the file manager's check accepts. A failure here used to be ignored,
# which produced a DMG that the application would silently refuse to use - fail the build instead.
codesign --force --sign - "${APP}/Contents/MacOS/${APP_NAME}"
codesign --force --sign - "${APP}"
codesign --verify --deep --strict "${APP}"

STAGE="${ROOT_DIR}/dist/stage"
mkdir -p "${STAGE}"
cp -R "${APP}" "${STAGE}/"
ln -s /Applications "${STAGE}/Applications"

hdiutil create -volname "RAR Support for Nimble Commander" \
    -srcfolder "${STAGE}" -fs HFS+ -format UDZO -ov "${DMG}" >/dev/null

rm -rf "${STAGE}"
( cd "${ROOT_DIR}/dist" && shasum -a 256 "$( basename "${DMG}" )" > "$( basename "${DMG}" ).sha256" )
echo "Built ${DMG}"
