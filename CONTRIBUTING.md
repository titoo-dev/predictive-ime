# Contributing

Thanks for your interest in predictive-ime! This is a predictive fcitx5 input
method (FR/EN). Code is MIT; the model is built from open corpora (see
[`NOTICE-DATASETS.md`](NOTICE-DATASETS.md)).

## Layout

| Dir | What | Build deps |
|---|---|---|
| `engine/` | fcitx5 `predict` engine addon (C++20) | Fcitx5Core, nlohmann_json, ECM |
| `daemon/` | `predictord` n-gram daemon + Python model builders | nlohmann_json (C++17); python3 (builders) |
| `ui/` | `qmlpanel` Qt Quick candidate bar | Qt6, wayland, **patched fcitx5** ([`docs/patched-fcitx5.md`](docs/patched-fcitx5.md)) |
| `ui/preferences/` | `ime-preferences` Qt app | Qt6 |

The engine talks to the daemon over a Unix socket (JSON protocol, documented in
[`docs/internals.md`](docs/internals.md)). Each component builds standalone or
via the top-level `CMakeLists.txt` superbuild.

## Build & test

```sh
# Portable core (any stock fcitx5):
cmake -B build -DBUILD_UI=OFF && cmake --build build -j

# Daemon behaviour tests (27 cases) and model evaluation:
python3 daemon/test_predict.py "$(command -v predictord || echo build/daemon/predictord)"
python3 daemon/eval_model.py <predictord> <words.tsv> <held-out-sentences>...

# Headless end-to-end / UI snapshot tests:
./test-e2e.sh        # sway + fcitx5 + wtype
./test-ui.sh         # grim screenshots + animation assertions
```

CI (`.github/workflows/build.yml`) builds the core against the stock fcitx5 of
Arch, Fedora, Debian/Ubuntu and openSUSE on every push — keep it green.

## Conventions

- Match the surrounding code (style, comment density, naming). The codebase is
  documented in French; new code may be in French or English — be consistent
  within a file.
- Keep components decoupled: the engine/daemon protocol is the contract; don't
  leak model internals into the engine.
- Behaviour changes to prediction/correction should come with a test in
  `daemon/test_predict.py` and, where relevant, an `eval_model.py` number.

## The fcitx5 patch

`qmlpanel`'s caret positioning needs `ui/waylandim-public.patch`. The goal is to
get it **upstream** into fcitx5 so no fork is needed — see
[`docs/upstream-fcitx5-patch.md`](docs/upstream-fcitx5-patch.md). Help on that PR
is especially welcome.

## Pull requests

Branch from `main`, keep changes focused, describe what you verified. By
contributing you agree your code is licensed under the repository's MIT license.
