#!/bin/sh

set -e

VERSION="0.22.1"
INPUT_ARCHIVE="$1"
OUTPUT_PKG="$2"
TMP_ROOT="/tmp/package.$$"
TMP_COMPONENT="/tmp/Vdagent.pkg"

command -v realpath >/dev/null 2>&1 || realpath() {
    [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
}

BASEDIR="$(dirname "$(realpath $0)")"

cp -a "$INPUT_ARCHIVE/Products" "$TMP_ROOT"

xattr -c "$TMP_ROOT/usr/local/bin/spice-vdagentd"

codesign --sign "Developer ID Application" \
         --force \
         --identifier com.redhat.spice.vdagentd \
         --preserve-metadata=entitlements,requirements,flags,runtime \
         --timestamp \
         "$TMP_ROOT/usr/local/bin/spice-vdagentd"

xattr -c "$TMP_ROOT/usr/local/bin/spice-vdagent"

codesign --sign "Developer ID Application" \
         --force \
         --identifier com.redhat.spice.vdagent \
         --preserve-metadata=entitlements,requirements,flags,runtime \
         --timestamp \
         "$TMP_ROOT/usr/local/bin/spice-vdagent"

pkgbuild --root "$TMP_ROOT" \
         --identifier com.redhat.spice.vdagent \
         --version "$VERSION" \
         --install-location / \
         --scripts "$BASEDIR/data/install_scripts" \
         "$TMP_COMPONENT"

productbuild --package "$TMP_COMPONENT" \
             --identifier com.redhat.spice.vdagent \
             --version "$VERSION" \
             --sign "Developer ID Installer" \
             "$OUTPUT_PKG"

rm "$TMP_COMPONENT"
rm -r "$TMP_ROOT"
