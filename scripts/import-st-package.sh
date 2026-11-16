#!/usr/bin/env bash
# Extract the SLA0111 parts (app/, lib/) of ST's STSW-IMG054 zip into vendor/.
# Usage: scripts/import-st-package.sh ~/Downloads/vl53l9-nucleo-n657-1.0.0.zip
set -euo pipefail
ZIP=${1:?usage: $0 path/to/stsw-img054.zip}
PKG=vl53l9-nucleo-n657-fmw-1.1.1
cd "$(dirname "$0")/.."

unzip -q -o "$ZIP" "$PKG/app/*" "$PKG/lib/*" -d vendor
echo "Imported app/ and lib/ into vendor/$PKG"
