#!/bin/sh
# Builds everything (and runs the tests), then stages release/Noiseflip and zips it.
set -e
sh build_mingw.sh
rm -rf release && mkdir -p release/Noiseflip
cp noiseflip.exe release/Noiseflip/
cp -r ui release/Noiseflip/ui
cp README.md release/Noiseflip/
( cd release && zip -qr ../Noiseflip-release.zip Noiseflip )
echo "Made Noiseflip-release.zip"
