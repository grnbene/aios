#!/bin/bash
# Rebuild the main checkout's web tools and record the commit they were built from (build/BUILD_SHA), which scripts/wt-build.py
# compares forks against. Run from anywhere: scripts/wt-main-build.sh
set -e
cd "$(dirname "$0")/.."
sha=$(git rev-parse HEAD)
git diff --quiet HEAD || echo "note: uncommitted changes in the main checkout are in the build but not in BUILD_SHA"
rm -f build/BUILD_SHA
(cd build && make -j3 aios-cli aios-web-pane-drive aios-web-layout-drive)
echo "$sha" > build/BUILD_SHA
echo "main build is at $sha"
