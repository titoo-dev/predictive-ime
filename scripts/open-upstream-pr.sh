#!/usr/bin/env bash
# Open the upstream fcitx5 PR that adds the waylandim public API, so qmlpanel
# (and any external UI addon) needs no fcitx5 fork. This is the sustainable fix
# for the patched-fcitx5 problem (see docs/patched-fcitx5.md).
#
# Run it when YOU are ready to open the PR under your GitHub account. Requires
# an authenticated `gh` with repo+workflow scope. It forks fcitx/fcitx5, applies
# ui/waylandim-public.patch on a fresh branch off upstream HEAD, pushes, and
# opens the PR with scripts/upstream-pr-body.md as the body.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH="$ROOT/ui/waylandim-public.patch"
BODY="$ROOT/scripts/upstream-pr-body.md"
BRANCH="waylandim-public-api"
TITLE="waylandim: expose raw zwp_input_method_v2 to external UI addons"

command -v gh >/dev/null || { echo "gh (GitHub CLI) required"; exit 1; }
me="$(gh api user -q .login)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
echo ">> forking + cloning fcitx/fcitx5 (gh handles fork propagation)"
gh repo fork fcitx/fcitx5 --clone >/dev/null
cd fcitx5
git fetch --quiet upstream
base="$(gh api repos/fcitx/fcitx5 -q .default_branch)"
git checkout -q -b "$BRANCH" "upstream/$base"

patch -p1 < "$PATCH"
git commit -aqm "$TITLE

Add WaylandIMModule::getInputMethodV2Raw() and INSTALL the WaylandIM public
module + header so external UI addons can create a caret-positioned
zwp_input_popup_surface_v2. Purely additive; existing consumers unaffected."
git push -u origin "$BRANCH"

gh pr create --repo fcitx/fcitx5 \
  --base "$base" --head "$me:$BRANCH" \
  --title "$TITLE" --body-file "$BODY"
echo ">> done."
