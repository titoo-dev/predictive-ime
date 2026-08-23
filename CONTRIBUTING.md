# Contributing

Thanks for your interest in predictive-ime! This is a predictive fcitx5 input
method (FR/EN). Code is MIT; the model is built from open corpora (see
[`NOTICE-DATASETS.md`](NOTICE-DATASETS.md)).

## Layout

| Dir | What | Build deps |
|---|---|---|
| `engine/` | fcitx5 `predict` engine addon (C++20) | Fcitx5Core, nlohmann_json, ECM |
| `daemon/` | `predictord` n-gram daemon + Python model builders | nlohmann_json (C++17); python3 (builders) |
| `preferences/` | `ime-preferences` Qt app | Qt6 |

The caret-positioned Qt Quick candidate bar lives in its own project:
[Opale](https://github.com/titoo-dev/opale).

The engine talks to the daemon over a Unix socket (JSON protocol, documented in
[`docs/internals.md`](docs/internals.md)). Each component builds standalone or
via the top-level `CMakeLists.txt` superbuild.

## Build & test

```sh
# Portable core (any stock fcitx5):
cmake -B build && cmake --build build -j

# Daemon behaviour tests (27 cases) and model evaluation:
python3 daemon/test_predict.py "$(command -v predictord || echo build/daemon/predictord)"
python3 daemon/eval_model.py <predictord> <words.tsv> <held-out-sentences>...

# Headless end-to-end tests:
./test-e2e.sh        # sway + fcitx5 + wtype
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

## Pull requests

Branch from `main`, keep changes focused, describe what you verified. By
contributing you agree your code is licensed under the repository's MIT license.
