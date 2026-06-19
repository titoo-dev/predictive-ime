# predictive-ime

Predictive French/English input method for [fcitx5](https://fcitx-im.org/).
A small fcitx5 engine queries a local n-gram daemon for completion,
autocorrection, next-word prediction and an emoji picker. Offline, no telemetry.

The core runs on any stock fcitx5 (using its default candidate bar). The
optional Qt Quick candidate bar (`qmlpanel`) needs a patched fcitx5 —
see [docs/patched-fcitx5.md](docs/patched-fcitx5.md).

## Install

**1. Dependencies**

- Arch: `pacman -S --needed base-devel cmake extra-cmake-modules fcitx5 nlohmann-json qt6-base qt6-declarative`
- Fedora: `dnf install gcc-c++ cmake extra-cmake-modules pkgconf-pkg-config fcitx5-devel nlohmann-json-devel qt6-qtbase-devel qt6-qtdeclarative-devel`
- Debian/Ubuntu: `apt install build-essential cmake extra-cmake-modules pkg-config nlohmann-json3-dev qt6-base-dev qt6-declarative-dev libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev`
- openSUSE: `zypper install gcc-c++ cmake extra-cmake-modules pkg-config fcitx5-devel nlohmann_json-devel qt6-base-devel qt6-declarative-devel`

**2. Build and install**

```sh
cmake -B build -DBUILD_UI=OFF
cmake --build build -j
sudo cmake --install build
```

**3. Get the model**

```sh
sudo mkdir -p /usr/share/ime-predictord
curl -fsSL https://github.com/titoo-dev/predictive-ime/releases/download/model-v1/ime-model-model-v1.tar.zst \
  | zstd -d | sudo tar -C /usr/share/ime-predictord -xf -
```

**4. Enable**

```sh
systemctl --user enable --now ime-predictord.service
```

Add `Predict` as an input method (e.g. with `fcitx5-configtool`) and restart fcitx5.

## Configuration

Settings live in `~/.config/ime-predictord/` (hot-reloaded): `config.json`,
`snippets.tsv`, `dict.txt`. The `ime-preferences` app edits `config.json`.

**Grammatical agreement.** The daemon boosts candidates that agree in number
and gender with the governing determiner found in the surrounding sentence
(`les petits chat…` → `chats`), using the Lefff morphological lexicon
(`morph.tsv`). `agreeBoost` in `config.json` (default `2.0`) tunes the strength
(higher = more aggressive agreement). The engine feeds the full sentence via the
toolkit's *surrounding text*; apps that don't expose it degrade to the words the
IME itself committed.

## Rebuild the model

`./build-model.sh <output-dir>` rebuilds it from the pinned open corpora.

## License

Code: MIT ([LICENSE](LICENSE)). Model: CC BY-SA 4.0, derived from open corpora —
see [NOTICE-DATASETS.md](NOTICE-DATASETS.md).

Design notes, algorithm and benchmarks: [docs/internals.md](docs/internals.md).
Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).
