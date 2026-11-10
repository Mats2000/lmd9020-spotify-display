#!/bin/sh
# Builds "Show Control.app" in tools/showcontrol/build (needs Xcode's command line tools).
# The display preview inside it is the firmware's own scene code (src/render), compiled for the Mac.
# Exo 2 and Sora are copied in from ~/Library/Fonts; Matter lights run from matter/ (npm install there).
set -e
cd "$(dirname "$0")"
ROOT=../..
APP="build/Show Control.app"
OBJ=build/obj
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Fonts" "$OBJ"
for src in demo.cpp "$ROOT"/src/render/*.cpp; do
    xcrun clang++ -std=c++17 -O2 -I. -I"$ROOT/include" -I"$ROOT/src" -c "$src" -o "$OBJ/$(basename "$src" .cpp).o"
done
xcrun swiftc -O -swift-version 5 -import-objc-header DemoBridge.h Audio.swift Display.swift Lamps.swift Style.swift Views.swift main.swift \
    "$OBJ"/*.o -lc++ -o "$APP/Contents/MacOS/Show Control"
cp Info.plist "$APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Add :MatterHelper string $(pwd)/matter" "$APP/Contents/Info.plist"
cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
for f in Exo2.0-SemiBold.otf Sora-Regular.ttf Sora-Light.ttf; do
    if [ -f "$HOME/Library/Fonts/$f" ]; then cp "$HOME/Library/Fonts/$f" "$APP/Contents/Resources/Fonts/"; else echo "Missing font $f"; fi
done
codesign --force --sign - "$APP" 2>/dev/null
echo "Built tools/showcontrol/$APP"
