#!/usr/bin/env bash
# NoFeed installer for Linux.
#
#   ./install.sh              install for the current user  (~/.vst3, ~/.local/bin)
#   sudo ./install.sh --system  install for all users       (/usr/lib/vst3, /usr/local/bin)
#   ./install.sh --uninstall  remove it again (add --system for a system-wide install)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode=user
action=install
for arg in "$@"; do
    case "$arg" in
        --system)    mode=system ;;
        --uninstall) action=uninstall ;;
        -h|--help)   sed -n '2,7p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [ "$mode" = system ]; then
    vst3_dir=/usr/lib/vst3
    bin_dir=/usr/local/bin
    apps_dir=/usr/share/applications
    if [ "$(id -u)" -ne 0 ]; then echo "System-wide install needs root: sudo $0 $*" >&2; exit 1; fi
else
    vst3_dir="$HOME/.vst3"
    bin_dir="$HOME/.local/bin"
    apps_dir="$HOME/.local/share/applications"
fi

if [ "$action" = uninstall ]; then
    rm -rf "$vst3_dir/NoFeed.vst3" "$bin_dir/NoFeed" "$apps_dir/nofeed.desktop"
    echo "NoFeed removed."
    exit 0
fi

mkdir -p "$vst3_dir" "$bin_dir" "$apps_dir"
rm -rf "$vst3_dir/NoFeed.vst3"
cp -R "$here/NoFeed.vst3" "$vst3_dir/"
install -m 755 "$here/NoFeed" "$bin_dir/NoFeed"

cat > "$apps_dir/nofeed.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=NoFeed
Comment=Feedback suppressor, de-noise and de-ess
Exec=$bin_dir/NoFeed
Categories=AudioVideo;Audio;
Terminal=false
DESKTOP

echo "NoFeed installed:"
echo "  VST3 plugin:    $vst3_dir/NoFeed.vst3"
echo "  Standalone app: $bin_dir/NoFeed"
echo "Rescan plugins in your DAW to pick it up."
