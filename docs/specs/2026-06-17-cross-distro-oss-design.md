# Design — Open-sourcing predictive-ime for all Linux distros

Date: 2026-06-17 · Status: accepted, in implementation (branch `feat/cross-distro-oss`)

## Context

predictive-ime is a predictive FR/EN input method: a thin fcitx5 engine addon
(`predict`) that delegates to an external n-gram daemon (`predictord`) over a
Unix socket, a Qt Quick caret-positioned candidate bar (`qmlpanel`), a small
preferences app (`ime-preferences`), and a Kneser-Ney model built from pinned
corpora. Today it is consumed only as a **Nix flake** (NixOS module + packages).
All install wiring — addon registration, the systemd user service, the fcitx5
profile, Qt plugin/QML paths, a patched fcitx5 — lives in `flake.nix`.

## Goal

Make predictive-ime **open-source and installable on any Linux distribution**,
prioritizing distribution flexibility. Reuse the existing GitHub repo.

## Premortem — "it's 12 months later and the cross-distro release failed"

| # | Failure mode | Preventive decision |
|---|---|---|
| F1 | **Patched-fcitx5 wall.** The QML bar requires patching fcitx5; nobody on Arch/Fedora/Debian recompiles their fcitx5 → either everyone gets classicui or adoption dies. | The patch's runtime addition (`getInputMethodV2Raw`) is a **1-line shim** over the already-published `getInputMethodV2`. Spike running qmlpanel on **stock fcitx5** by vendoring `waylandim_public.h` + deriving the raw pointer. Upstream the patch in parallel. Runtime fallback to classicui if the API is absent. → fork stops being a hard requirement. |
| F2 | **Model isn't shippable.** 66 MB built from hundreds of MB of network corpora; distro build sandboxes forbid network; URLs rot. | Build the model **once in CI** (`build-model.sh`, pinned SHA-256) → versioned tarball in **GitHub Releases**. Packages bundle/fetch the artifact, never re-download corpora at build time. |
| F3 | **Data licensing → distro rejection.** Model derives from CC BY / Unicode data; no clear license = not redistributable. | `LICENSE` (MIT, code) + `NOTICE-DATASETS.md` (per-corpus attribution; generated model = **CC BY-SA 4.0**). NOTICE travels inside the model artifact. |
| F4 | **Nix-shaped assumptions everywhere.** All wiring in `flake.nix`; baked Qt paths. | Standalone top-level **CMake superbuild** + `cmake --install`; auto-detect Qt paths (baked flags optional); ship systemd unit + `.desktop`. Flake becomes a thin wrapper. |
| F5 | **fcitx5 ABI drift.** Addons compiled against one fcitx5 don't load on another. | No universal prebuilt engine/UI binaries — build per distro from source. Only the **daemon** (no fcitx5 dep) and the **model** (data) are distro-agnostic artifacts. |
| F6 | **"All distros at once" = dispersion;** Flatpak is a trap for an IME *engine* (sandbox can't inject into host fcitx5). | Few high-leverage channels: **source build + docs** (universal) + keep the **Nix flake**. Distro-native packages (AUR/COPR/OBS) and Flatpak deferred. |
| F7 | **NixOS/Hyprland/DMS couplings in UX** (DMS colors, SUPER+ALT+I, Maple Mono, stowed autostart hack). | Graceful fallbacks: default palette without DMS, font fallback, preferences as a normal `.desktop` app, standard fcitx5 autostart, documented `systemctl --user enable`. |

## Decisions (chosen by maintainer)

- **qmlpanel everywhere from v1** (take on F1) — via the stock-fcitx5 spike, not a forced fork.
- **Channels:** source + CMake + docs, and keep the Nix flake. (AUR door left open in a later phase; not implemented now.)
- **Model:** prebuilt, versioned release artifact (deterministic CI build).
- **Code license:** MIT. Model artifact: CC BY-SA 4.0 (per F3).

## Architecture (target)

```
CMakeLists.txt          # top-level superbuild (toggles + install glue)
engine/                 # fcitx5 'predict' addon  (Fcitx5Core, nlohmann_json)
daemon/                 # predictord (nlohmann_json) + Python model builders
ui/                     # qmlpanel addon (Fcitx5, Qt6, wayland) + vendored header
ui/preferences/         # ime-preferences Qt app + .desktop
packaging/              # ime-predictord.service.in (systemd user unit)
build-model.sh          # standalone model build (pinned corpora)
flake.nix               # thin wrapper over the CMake build
.github/workflows/      # model release + multi-distro build matrix
LICENSE, NOTICE-DATASETS.md, README.md, CONTRIBUTING.md
```

Components are independently buildable; each owns one purpose and a clear
interface (engine↔daemon = documented JSON socket protocol; addons↔fcitx5 =
published-function API).

## The qmlpanel / patched-fcitx5 plan (central)

`ui/qmlui.cpp` calls `call<IWaylandIMModule::getInputMethodV2Raw>(ic)`.
`getInputMethodV2Raw(ic)` is literally `wayland::rawPointer(getInputMethodV2(ic))`;
`getInputMethodV2` is published by **stock** waylandim. The stock blockers are
only that fcitx5 doesn't *install* `waylandim_public.h` / the `WaylandIM` CMake
module.

Plan: a CMake option `QMLPANEL_REQUIRE_PATCHED_FCITX5`.
- **OFF (target default):** vendor `waylandim_public.h` (declaring `getInputMethodV2`
  only), call it + `rawPointer` in qmlui.cpp, drop the `WaylandIM` find_package
  component. Builds & runs on stock fcitx5.
- **ON (current default, flake path):** use the patched `getInputMethodV2Raw`.
- Runtime: if the published function is unavailable, degrade to classicui.

Default flips to OFF once the stock path is implemented and CI-verified
(P0). Until then the portable **core** (engine+daemon+preferences) builds on
stock fcitx5 with `-DBUILD_UI=OFF`.

## Model distribution

`build-model.sh` reproduces the Nix `ime-model` derivation exactly (same URLs,
same pinned SHA-256, same pipeline) with plain bash/python. CI runs it on tags,
checksums the output, attaches `ime-model-vN.tar.zst` + `NOTICE` to the Release,
and runs `eval_model.py` as a regression gate. Daemon loads the model from a
standard data dir; install docs fetch the pinned artifact.

## Phasing

- **P0** Spike qmlpanel on stock fcitx5; decide patch path; flip option default. *(CI-verified)*
- **P1** Standalone CMake + install targets + licensing + spec. *(this commit)*
- **P2** Model CI build + versioned Release artifact + eval gate.
- **P3** Multi-distro CI matrix (Arch/Fedora/Ubuntu/openSUSE) + graceful fallbacks.
- **P4** General-audience README, flake-as-wrapper, OSS hygiene; later: upstream the patch.

## Open questions

- MIT vs Apache-2.0 (patent grant) — defaulted to MIT, revisit before first tag.
- Whether to add AUR (PKGBUILD) once the source build is proven in CI.
- Non-systemd init support (runit/OpenRC/s6) — docs-only / best-effort for now.
