# Running qmlpanel on a patched fcitx5

`qmlpanel` (the caret-positioned QML candidate bar) needs the raw
`zwp_input_method_v2` object owned by fcitx5's `waylandim` frontend. Stock
fcitx5 does **not** expose it to external addons (verified on fcitx5-devel
5.1.7 / 5.1.13 / 5.1.17 / 5.1.19: no waylandim public header, no
`ZwpInputMethodV2` wrapper, no `rawPointer`). The fix is a 4-line patch that
adds `getInputMethodV2Raw` and `INSTALL`s the waylandim public module:
[`../ui/waylandim-public.patch`](../ui/waylandim-public.patch).

This is the **interim** path until the patch is accepted upstream (tracking:
see the repo issues). Once upstream, a new-enough stock fcitx5 will carry the
API and qmlpanel becomes a plain addon — no rebuild of fcitx5 needed.

## The sustainable path: upstream

The patch is intentionally minimal and generic (a raw accessor + installing the
already-existing public header). Help get it merged into fcitx5 rather than
maintaining a fork. PR-ready description: [`upstream-fcitx5-patch.md`](upstream-fcitx5-patch.md).

## Nix (automatic)

The flake builds the patched fcitx5 for you (`fcitx5-patched`) and the
`nixosModules.default` overlays it system-wide. Nothing to do beyond importing
the module. This is the reference, always-working setup.

## Standalone (non-Nix), advanced

You rebuild fcitx5 from source **at the same version as your distro's** (addon
ABI must match), apply the patch, install it, then build qmlpanel against it.

```sh
# 1. Match your distro's fcitx5 version (addons are version-locked).
FCITX_VER="$(pkg-config --modversion Fcitx5Core)"   # e.g. 5.1.13

# 2. Get the matching source + this repo's patch.
git clone --branch "$FCITX_VER" --depth 1 https://github.com/fcitx/fcitx5
cd fcitx5
patch -p1 < /path/to/predictive-ime/ui/waylandim-public.patch

# 3. Build & install. Pick ONE of:
#    (a) a private prefix (does NOT touch the system fcitx5):
cmake -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local/fcitx5-patched"
#    (b) over the system (only if you manage fcitx5 yourself, e.g. an AUR
#        -git package or a held package — otherwise a distro update reverts it):
#    cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)" && cmake --install build
cd ..

# 4. Build qmlpanel against the patched fcitx5 (point CMake at its prefix).
cmake -B build-ui -S /path/to/predictive-ime \
  -DBUILD_ENGINE=OFF -DBUILD_DAEMON=OFF -DBUILD_PREFERENCES=OFF -DBUILD_UI=ON \
  -DCMAKE_PREFIX_PATH="$HOME/.local/fcitx5-patched" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/fcitx5-patched"
cmake --build build-ui -j"$(nproc)" && cmake --install build-ui

# 5. Run that fcitx5 as your session IME with the QML UI:
#    "$HOME/.local/fcitx5-patched/bin/fcitx5" --ui qmlpanel
```

**Caveats**

- fcitx5 is one-per-session: run *either* the system fcitx5 *or* the patched
  one, not both. With prefix (a), launch the patched binary from your session
  autostart and make sure the system fcitx5 isn't also started.
- A private-prefix fcitx5 must find its addons: set
  `FCITX_ADDON_DIRS="$HOME/.local/fcitx5-patched/lib/fcitx5:/usr/lib/fcitx5"`
  (adjust libdir per distro) so it loads both the patched modules and your
  distro's input methods.
- Theme colours for the bar default sanely; override via
  `~/.config/fcitx5/qmlpanel/colors.json`.

If this is too heavy for your setup, use the core with classicui — it is fully
functional; qmlpanel is only a cosmetic upgrade.
