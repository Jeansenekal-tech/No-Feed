#!/usr/bin/env bash
# Builds the macOS installer package (VST3 + AU + standalone app, each optional).
# Usage: build-pkg.sh <version> <artefacts/Release dir> <output dir>
set -euo pipefail

VERSION="$1"
ART="$2"
OUT="$3"
ID="com.uniqueav.nofeed"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$OUT"

make_component () {   # <name> <bundle path> <install location>
    local name="$1" bundle="$2" location="$3"
    mkdir -p "$work/root-$name"
    cp -R "$bundle" "$work/root-$name/"

    # ad-hoc signature so Apple Silicon will load the binaries (not a Developer ID signature)
    codesign --force --deep --sign - "$work/root-$name/$(basename "$bundle")"

    # always install to the given location, never "relocate" to a copy found elsewhere
    pkgbuild --analyze --root "$work/root-$name" "$work/$name.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$work/$name.plist"

    pkgbuild --root "$work/root-$name" \
             --component-plist "$work/$name.plist" \
             --identifier "$ID.$name" \
             --version "$VERSION" \
             --install-location "$location" \
             "$work/$name.pkg"
}

make_component vst3 "$ART/VST3/NoFeed.vst3"            "/Library/Audio/Plug-Ins/VST3"
make_component au   "$ART/AU/NoFeed.component"         "/Library/Audio/Plug-Ins/Components"
make_component app  "$ART/Standalone/NoFeed.app"       "/Applications"

cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>NoFeed $VERSION</title>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 plugin" description="Installs NoFeed.vst3 to /Library/Audio/Plug-Ins/VST3">
        <pkg-ref id="$ID.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit plugin" description="Installs NoFeed.component (Logic, GarageBand, ...) to /Library/Audio/Plug-Ins/Components">
        <pkg-ref id="$ID.au"/>
    </choice>
    <choice id="app" title="Standalone app" description="Installs NoFeed.app to /Applications">
        <pkg-ref id="$ID.app"/>
    </choice>
    <pkg-ref id="$ID.vst3" version="$VERSION" onConclusion="none">vst3.pkg</pkg-ref>
    <pkg-ref id="$ID.au"   version="$VERSION" onConclusion="none">au.pkg</pkg-ref>
    <pkg-ref id="$ID.app"  version="$VERSION" onConclusion="none">app.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --distribution "$work/distribution.xml" \
             --package-path "$work" \
             "$OUT/NoFeed-$VERSION-macOS.pkg"

echo "Built $OUT/NoFeed-$VERSION-macOS.pkg"
