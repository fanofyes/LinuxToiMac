#!/bin/sh
# Builds VScreen.app next to this script. Needs only the Xcode command line tools.
set -eu
cd "$(dirname "$0")"

APP=build/VScreen.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"

swiftc -O -target x86_64-apple-macos11.0 -o "$APP/Contents/MacOS/VScreen" VScreen.swift

cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>VScreen</string>
	<key>CFBundleIdentifier</key><string>local.vscreen</string>
	<key>CFBundleExecutable</key><string>VScreen</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
EOF

codesign --force --sign - "$APP"
echo "built $APP"
