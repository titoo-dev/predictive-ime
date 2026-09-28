# predictive-ime

Predictive French/English input method for [fcitx5](https://fcitx-im.org/).
A small fcitx5 engine queries a local n-gram daemon for completion,
autocorrection, next-word prediction and an emoji picker. Offline, no telemetry.

![predictive-ime in action: next-word prediction, intra-word completion and the emoji picker](assets/demo.gif)

> Typing `je vais au travail aujourd'`, accepting the `aujourd'hui` completion, then **Super+;** `coeur` → ❤️. The candidate bar shows at most five ranked suggestions.

The core runs on any stock fcitx5 (using its default candidate bar). The
optional Qt Quick candidate bar (`qmlpanel`) needs a patched fcitx5 —
see [docs/patched-fcitx5.md](docs/patched-fcitx5.md).

## Install (Linux / fcitx5)

**1. Dependencies**

- Arch: `pacman -S --needed base-devel cmake extra-cmake-modules fcitx5 nlohmann-json qt6-base qt6-declarative curl`
- Fedora: `dnf install gcc-c++ cmake extra-cmake-modules pkgconf-pkg-config fcitx5-devel nlohmann-json-devel qt6-qtbase-devel qt6-qtdeclarative-devel libcurl-devel`
- Debian/Ubuntu: `apt install build-essential cmake extra-cmake-modules pkg-config nlohmann-json3-dev qt6-base-dev qt6-declarative-dev libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev libcurl4-openssl-dev`
- openSUSE: `zypper install gcc-c++ cmake extra-cmake-modules pkg-config fcitx5-devel nlohmann_json-devel qt6-base-devel qt6-declarative-devel libcurl-devel`

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

## Install (Windows 10 1803+ / 11)

Windows gets a native **TSF text service** (`predict-tsf.dll`) in place of the
fcitx5 engine, plus the same `predictord` daemon. Both frontends share the
input logic in [`core/`](core/) — see
[docs/specs/2026-09-11-windows-tsf-port-design.md](docs/specs/2026-09-11-windows-tsf-port-design.md).

**Easiest: the installer**

```powershell
.\dist\predictive-ime-0.1.0-x64.exe
```

It installs the text service under Program Files (a TSF DLL is loaded into
every application, so it must live where an unprivileged process cannot
rewrite it), registers it, grants `ALL APPLICATION PACKAGES` read access so
Store apps can load it, sets up the model and the daemon's logon task, and
adds **Predict** to your input methods so it shows up in **Win+Space** right
away. Build it yourself with `iscc packaging\windows\predictive-ime.iss`
(Inno Setup 6.6+ gives the wizard a dark mode that follows Windows).

**Or from source**

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools `
  --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Meta.Zstandard Git.Git

.\scripts\build-windows.ps1          # predictord.exe + predict-tsf.dll
.\scripts\setup-windows.ps1          # model, logon task, IME registration
Start-ScheduledTask -TaskName ime-predictord
```

The Build Tools ship their own CMake and Ninja; vcpkg (`nlohmann-json`,
`curl`) is bootstrapped by the build script. Run `setup-windows.ps1` from an
**elevated** shell to have it register the text service; otherwise it prints
the one `regsvr32` command to run yourself.

Then pick **Predict** with **Win+Space** and start typing.

**What it looks like.** The candidate bar is drawn with Direct2D/DirectWrite
to match Windows 11 menus: rounded corners, shadow and border from DWM, your
**accent color**, light/dark theme and **high contrast** followed live, color
emoji, and text that stays sharp at any scaling, including in apps that don't
handle DPI. The same three layouts as `qmlpanel`: word chips (the one Space
will apply is outlined in the accent color), the emoji grid, and a numbered
list for reformulation. The highlight slides between candidates; animations
respect *Settings › Accessibility › Animation effects*. On Windows 10 the bar
keeps square corners. For emoji on Windows, use **Win+;** — Windows keeps that
shortcut for its own emoji panel, so the Linux **Super+;** picker does not
apply there, and `:` types a plain colon on both systems.

A **Predict** icon sits next to the language indicator on the taskbar: click it
to pause or resume prediction in every app at once (the choice persists across
reboots); right-click for settings and help.

**Keyboard layout.** A text service has no layout of its own: unless told
otherwise, Windows hands Predict **US QWERTY**, whatever you picked (shortcuts
included: Ctrl+Z on AZERTY arrived as Ctrl+W). Registration therefore declares
your layout for each profile — TSF only accepts a layout *of the profile's
language*: French + AZERTY works, English + AZERTY cannot. Setup only offers
Predict under languages where it will type like you (Predict FR predicts
English too). The taskbar icon's tooltip and menu show the layout actually in
use.

The bar stays anchored to the word being typed: when an app has no layout yet
(Firefox and Chrome often answer `TS_E_NOLAYOUT` on a word's first letter) it
keeps its last position instead of jumping, and it follows the text when the
window moves or scrolls.

To preview the bar without installing anything:

```powershell
cmake --build build-win-x64 --config Release --target candidate-preview
$env:IME_PANEL_THEME = 'dark'   # or light; default: the Windows theme
.\build-win-x64\win\tools\Release\candidate-preview.exe $env:TEMP
```

It captures every layout (plus the taskbar icon) as PNGs.

Regression tests (also run by CI): `ctest --test-dir build-win-x64 -C Release`.
`win-layout-installed` checks the IME *installed on your machine* — Predict
must stay the active input method and type with your own layout; it is
skipped where Predict is not installed.

**Check the daemon on its own**

```powershell
.\scripts\probe-daemon.ps1 -Context je -Prefix v
# -> {"candidates":["vous","vais","veux","voudrais","voulais"], ...}
.\scripts\try-daemon.ps1             # testeur interactif dans le terminal
```

`probe-daemon.ps1` is the Windows stand-in for `nc -U`: same line-delimited
JSON over the same **AF_UNIX** socket (native since Windows 10 1803) that the
text service uses. `try-daemon.ps1` is a terminal REPL that shows suggestions
as you type — handy to judge the model without switching input method.

**Where things live.** `%LOCALAPPDATA%\ime-predictord\` holds the model, the
socket, the daemon log and the learned-word journals; `%APPDATA%\ime-predictord\`
holds the editable settings (`config.json`, `dict.txt`, `snippets.tsv`) — the
same split as XDG data vs. config on Linux.
`.\scripts\setup-windows.ps1 -Uninstall` unregisters the IME and removes the
logon task, leaving both directories intact.

**Known limits on Windows.** The Qt preferences app and the Wayland `qmlpanel`
are not ported — the candidate bar is drawn by the text service itself, and
settings are edited in `config.json` (the taskbar icon's *Réglages…* opens it). The neural predictor is off, as on Linux.

## Configuration

Settings live in `~/.config/ime-predictord/` (hot-reloaded): `config.json`,
`snippets.tsv`, `dict.txt`. The `ime-preferences` app edits `config.json`.

**Language.** `lang` in `config.json` picks the suggestion language: `fr` / `en`
(deterministic, the other language is strictly excluded), `auto` (context
vote), `off`. **Ctrl+Shift+L** opens a compact language switcher in the
candidate bar — [Français|English|Auto|Libre] chips with the current choice
highlighted; arrows/Tab navigate, `1-4`/Enter/Space apply, Escape cancels.
The engine rewrites `lang` in place (formatting preserved), the daemon
hot-reloads it, and the bar restarts in the new language. English
contractions (`don't`, `i'm`, `you're`…) are first-class vocabulary: completed
from `don`, restored from `dont`/`im`/`cant`, and `i`/`i'…` are always
capitalised to `I`/`I'…`.

**Grammatical agreement.** The daemon boosts candidates that agree in number
and gender with the governing determiner found in the surrounding sentence
(`les petits chat…` → `chats`), using the Lefff morphological lexicon
(`morph.tsv`). `agreeBoost` in `config.json` (default `2.0`) tunes the strength
(higher = more aggressive agreement). The engine feeds the full sentence via the
toolkit's *surrounding text*; apps that don't expose it degrade to the words the
IME itself committed.

**Recency cache.** Words already present in the text before the cursor (the
document you are writing, seen through the toolkit's *surrounding text*) are
boosted — human text repeats itself (proper nouns, topic vocabulary), so a word
you already used is likely to come back. `recencyBoost` in `config.json`
(default `1.3`) tunes the strength; `1.0` disables it. The word immediately
before the cursor is never boosted (no `the the`).

**Learned words.** Words you commit are learned and ranked **on the model's own
scale** (no overriding floor): a trusted learned word is treated as having an
effective frequency of at least a baseline, multiplied by its usage confidence —
so a rarely-learned word surfaces above ordinary words but never above a
massively more frequent one (`j'ai`), while a heavily-used one climbs past it.
`learnedBoost` in `config.json` (default `1.0`) scales how aggressive learned
suggestions are; `learnedFloor` (default `150000`) is the minimum effective
frequency a trusted learned word is treated as having (raise either if your
learned words feel too weak on your corpus).

**Bare elision proclitics.** Typing `j'` proposes `j'ai`/`j'aime` ahead of the
bare proclitic `j'` (rarely the intended final word). `proclisisDemote`
(default `6.0`) divides the score of a bare proclitic form (`j'`, `c'`, `qu'`,
`d'`, `n'`, `s'`, `t'`, `m'`, `l'`) when you typed exactly that proclitic.

**Emoji picker.** Always on, opened with **Super+;** at any point: empty
buffer, mid-word (the word in progress is committed as typed), or with the
next-word bar open; pressing it again closes the picker. It works **even when
the predictive input method is not the active one** (the addon watches the
shortcut before any input method, switches to `predict` and opens the picker),
so no Ctrl+Space first. The switch is a **loan**: as soon as the picker closes
(emoji inserted, Escape, shortcut pressed again, focus lost) your previous
input method is restored, so picking an emoji never leaves text prediction
turned on. It is a Material 3 surface with its own search field,
so the query stays in the picker and is never typed into your document. Type a
CLDR keyword (`coeur`, `soleil`, `fire`…) to filter the grid, or pick straight
from your recently-used favourites. Up to 96 results, **24 per page**: arrows
move cell by cell and by row, spilling over to the next/previous page at the
edges, PgDn/PgUp jump a page, Home/End go to either end, and the current page
shows as `2/4` in the search field. Enter inserts, Escape closes without
inserting anything. A typed `:` is a plain character everywhere (`10:30`).

**French typography (opt-in).** `frenchSpacing` (default `false`) inserts a
narrow no-break space (U+202F) before `;` `:` `!` `?` and the closing guillemet
`»`, and after the opening guillemet `«` — absorbing a regular space you already
typed. `autoCapitalize` (default `false`) capitalises the first letter at the
start of a field and after a sentence end (`. ! ?`), detected from the
surrounding text; it only touches the first letter (acronyms stay intact).

**Speculative bar in terminals.** The next-word bar shown *between* words has no
preedit to anchor it to the cursor, so in some terminals it trails behind the
caret. `nextWordBarExclude` (default `[]`) is a list of program substrings
(case-insensitive, matched against the client's program/app-id) for which that
speculative bar is suppressed — the inline completion bar (anchored to the
preedit while you type) is kept. Example: `"nextWordBarExclude": ["ghostty"]`.
Set `nextWordBar` to `false` to disable the speculative bar everywhere.

## Rebuild the model

`./build-model.sh <output-dir>` rebuilds it from the pinned open corpora.

## License

Code: MIT ([LICENSE](LICENSE)). Model: CC BY-SA 4.0, derived from open corpora —
see [NOTICE-DATASETS.md](NOTICE-DATASETS.md).

Design notes, algorithm and benchmarks: [docs/internals.md](docs/internals.md).
Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).
