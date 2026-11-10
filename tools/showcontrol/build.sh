#!/bin/sh
# Builds "Show Control.app" in tools/showcontrol/build (needs Xcode's command line tools).
# The display preview inside it is the firmware's own scene code (src/render), compiled for the Mac.
# Exo 2 and Sora are copied in from ~/Library/Fonts; Matter lights run from matter/ (npm install there first).
set -e
cd "$(dirname "$0")"
ROOT=../..
APP="build/Show Control.app"
OBJ=build/obj
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Fonts" "$OBJ"
for src in demo.cpp "$ROOT"/src/render/*.cpp; do
    xcrun clang++ -std=c++17 -O2 -I. -I"$ROOT/include" -I"$ROOT/src" -c "$src" -o "$OBJ/$(basename "$src" .cpp).o"
done
xcrun swiftc -O -swift-version 5 -import-objc-header DemoBridge.h Audio.swift Drums.swift DrumModel.swift Display.swift Lamps.swift Fixtures.swift Rig.swift Show.swift Web.swift Relay.swift Style.swift Views.swift main.swift \
    "$OBJ"/*.o -lc++ -o "$APP/Contents/MacOS/Show Control"
cp Info.plist "$APP/Contents/Info.plist"
# The Matter helper runs from Application Support: macOS holds back anything the app starts from
# reading the Documents folder.
HELPER="$HOME/Library/Application Support/Show Control/matter"
mkdir -p "$HELPER"
rsync -a --delete --exclude .matter matter/ "$HELPER/"
/usr/libexec/PlistBuddy -c "Add :MatterHelper string $HELPER" "$APP/Contents/Info.plist"
cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
mkdir -p "$APP/Contents/Resources/web"
cp web/index.html "$APP/Contents/Resources/web/"
sips -z 180 180 AppIcon.png --out "$APP/Contents/Resources/web/icon.png" >/dev/null
for f in Exo2.0-SemiBold.otf Sora-Regular.ttf Sora-Light.ttf; do
    if [ -f "$HOME/Library/Fonts/$f" ]; then cp "$HOME/Library/Fonts/$f" "$APP/Contents/Resources/Fonts/"; else echo "Missing font $f"; fi
done
codesign --force --sign - "$APP" 2>/dev/null
echo "Built tools/showcontrol/$APP"
