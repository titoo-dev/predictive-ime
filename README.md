# predictive-ime

A predictive **French/English** input method for [fcitx5](https://fcitx-im.org/):
a thin fcitx5 engine that delegates to a fast local **n-gram daemon** (Kneser-Ney,
µs-latency lookups), with completion, AZERTY-aware autocorrection, next-word
prediction, an emoji picker, and learning from your own typing — all offline,
no network, no telemetry. Quality is in the ballpark of a deployed Gboard
n-gram model (see [`docs/internals.md`](docs/internals.md) for the benchmark).

> **Status:** the portable core (engine + daemon + preferences) is CI-verified
> to build against the **stock fcitx5** of Arch, Fedora, Debian/Ubuntu and
> openSUSE. The optional caret-positioned **QML candidate bar** (`qmlpanel`)
> needs a small fcitx5 patch — see [qmlpanel](#optional-qml-candidate-bar).

## Components

| Component | What it is | Needs |
|---|---|---|
| `predict` | fcitx5 engine addon — buffers input, queries the daemon, shows candidates | stock fcitx5 |
| `predictord` | n-gram prediction daemon (Unix socket + JSON) | — |
| `ime-preferences` | small Qt app to tune language/behaviour (`config.json`) | Qt 6 |
| `qmlpanel` | Qt Quick candidate bar positioned at the caret by the compositor | **patched** fcitx5 (Wayland) |
| model | `words.tsv` + n-grams + emoji, built from pinned open corpora | — |

The engine works with fcitx5's standard **classicui** bar out of the box;
`qmlpanel` is a cosmetic upgrade, not a requirement.

## Install (core)

### 1. Dependencies

| Distro | Command |
|---|---|
| **Arch** | `sudo pacman -S --needed base-devel cmake extra-cmake-modules fcitx5 nlohmann-json qt6-base qt6-declarative` |
| **Fedora** | `sudo dnf install gcc-c++ cmake extra-cmake-modules pkgconf-pkg-config fcitx5-devel nlohmann-json-devel qt6-qtbase-devel qt6-qtdeclarative-devel` |
| **Debian/Ubuntu** | `sudo apt install build-essential cmake extra-cmake-modules pkg-config nlohmann-json3-dev qt6-base-dev qt6-declarative-dev libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev` |
| **openSUSE** | `sudo zypper install gcc-c++ cmake extra-cmake-modules pkg-config fcitx5-devel nlohmann_json-devel qt6-base-devel qt6-declarative-devel` |

### 2. Build & install

```sh
cmake -B build -DBUILD_UI=OFF -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

This installs the `predict` engine addon, the `predictord` daemon, the
`ime-preferences` app, and a `ime-predictord` systemd **user** service.

### 3. Get the model

The model is distributed as a versioned release artifact (recommended), or you
can rebuild it from the pinned corpora:

```sh
# Option A — download the prebuilt model (replace vN with the latest release):
#   https://github.com/titoo-dev/predictive-ime/releases
sudo mkdir -p /usr/share/ime-predictord
curl -fsSL .../ime-model-vN.tar.zst | sudo tar -C /usr/share/ime-predictord --zstd -xf -

# Option B — rebuild it yourself (downloads the open corpora, ~minutes):
sudo ./build-model.sh /usr/share/ime-predictord
```

### 4. Start the daemon & enable the input method

```sh
systemctl --user daemon-reload
systemctl --user enable --now ime-predictord.service
```

Then add **Predict** as an input method (e.g. via `fcitx5-configtool`, or add a
`predict` item to your fcitx5 group) and restart fcitx5. Toggle it like any
fcitx5 input method.

## Optional: QML candidate bar

`qmlpanel` renders the caret-positioned, themeable candidate bar. It needs the
raw Wayland input-method object, which stock fcitx5 does **not** expose to
external addons — so it requires a fcitx5 carrying a small public-API patch
([`ui/waylandim-public.patch`](ui/waylandim-public.patch), 4 lines + a header
install). Until that lands upstream, build qmlpanel against a patched fcitx5
(the Nix flake does this automatically; a standalone recipe is in
[`docs/patched-fcitx5.md`](docs/patched-fcitx5.md)) and run `fcitx5 --ui qmlpanel`.
Without it, the core uses fcitx5's standard classicui bar.

## Configuration

Runtime config lives in `~/.config/ime-predictord/` (hot-reloaded, no restart):
`config.json` (language, autocorrection switches…), `snippets.tsv`, `dict.txt`
(personal words). The `ime-preferences` app edits `config.json` for you; it also
has a CLI: `ime-preferences --set lang=fr`. Details in
[`docs/internals.md`](docs/internals.md).

## Nix / NixOS

```sh
nix build github:titoo-dev/predictive-ime#predictord   # etc.
```

The flake exposes packages, a `nixosModules.default` (wires the addon into
fcitx5 and runs the daemon), and builds the patched fcitx5 for qmlpanel.

## License

Source code: **MIT** ([`LICENSE`](LICENSE)). The prediction **model** is derived
from third-party corpora (mostly CC BY) and is distributed under **CC BY-SA 4.0**
with attribution — see [`NOTICE-DATASETS.md`](NOTICE-DATASETS.md).

Deep design notes, the model/algorithm, interaction reference and benchmarks:
[`docs/internals.md`](docs/internals.md).
