#!/bin/sh
# Builds "LMD Visualizer.app" in tools/visualizer/build (needs Xcode's command line tools).
# The preview inside it is the firmware's own scene code (src/render), compiled for the Mac.
set -e
cd "$(dirname "$0")"
ROOT=../..
APP="build/LMD Visualizer.app"
OBJ=build/obj
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$OBJ"
for src in demo.cpp "$ROOT"/src/render/*.cpp; do
    xcrun clang++ -std=c++17 -O2 -I. -I"$ROOT/include" -I"$ROOT/src" -c "$src" -o "$OBJ/$(basename "$src" .cpp).o"
done
xcrun swiftc -O -swift-version 5 -import-objc-header DemoBridge.h LMDVisualizer.swift "$OBJ"/*.o -lc++ \
    -o "$APP/Contents/MacOS/LMD Visualizer"
cp Info.plist "$APP/Contents/Info.plist"
cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
codesign --force --sign - "$APP" 2>/dev/null
echo "Built tools/visualizer/$APP"
