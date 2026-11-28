#!/bin/sh
# Install the Lamoka1/2 3D-render pre-commit hook.
# Run once from anywhere inside the repo:  tools/install-hooks.sh

set -e
repo=$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)
cd "$repo"

chmod +x .githooks/pre-commit tools/render_3d.py
git config core.hooksPath .githooks
echo "hooks installed: core.hooksPath=$(git config core.hooksPath)"

# GitKraken (Flatpak) cannot see kicad-cli inside its sandbox; the hook
# calls flatpak-spawn --host, which needs this talk permission.
if flatpak info com.axosoft.GitKraken >/dev/null 2>&1; then
    if ! flatpak override --user --show com.axosoft.GitKraken 2>/dev/null \
            | grep -q 'org.freedesktop.Flatpak'; then
        flatpak override --user --talk-name=org.freedesktop.Flatpak com.axosoft.GitKraken
        echo "granted GitKraken host-command access (flatpak override --user)."
        echo "restart GitKraken once for it to take effect."
    fi
fi
