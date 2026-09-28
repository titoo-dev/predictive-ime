# Design — Windows port: TSF text service + native `predictord`

Date: 2026-09-11 · Status: **implemented** (phases 0-4)

## What actually shipped

| Phase | Result |
|---|---|
| 0 | VS 2022 Build Tools + Win11 SDK, vcpkg (`nlohmann-json`, `curl`), zstd. CMake/Ninja come with the Build Tools — no separate install needed. |
| 1 | `predictord.exe` — byte-identical candidates to Linux on the same model. `core/os_compat.h` holds every divergence. |
| 2 | `core/` extracted: `text.h`, `config.h`, `daemon_client.h`, `state.h`, `frontend.h`, `engine_core.{h,cpp}`. `engine/predict.cpp` is now a ~440-line fcitx5 adapter. The engine test harness passes. |
| 3 | `win/tsf/` — `predict-tsf.dll`, 522 KB, system DLLs only (`/MT`). Registers, instantiates via COM, appears in the Windows input-method list. |
| 4 | `packaging/windows/predictive-ime.iss` → `dist/predictive-ime-0.1.0-x64.exe` (2.6 MB). |

### Corrections the work forced

- **W1 was under-stated.** The daemon was a *console* application, so it died
  with `STATUS_CONTROL_C_EXIT` whenever its launching console closed. Fixed by
  linking the WINDOWS subsystem (no console at all) with
  `/ENTRY:mainCRTStartup`, logging to
  `%LOCALAPPDATA%\ime-predictord\predictord.log`.
- **`freopen_s` opens `_SH_DENYRW`** — the log was unreadable while the daemon
  ran. `freopen` shares.
- **Composition text is document text.** Under fcitx5 the preedit is display
  only; under TSF it lives in the document. So reading "text before the cursor"
  had to anchor at the *composition start*, not the caret — otherwise the word
  being typed would pollute its own prediction context. Likewise, clearing the
  preedit must delete the composition, not merely stop drawing it.
- **Session state cannot live in `EngineCore`.** It is rebuilt on every
  keystroke; `lastReformMode_` reset itself each time. Caught by the engine
  test, moved to a `SessionPrefs` the adapter owns.
- **`WIN32_LEAN_AND_MEAN` is a project-level define**, not a per-file one: the
  first bare `windows.h` pulls the old `winsock.h` and every `winsock2.h`
  declaration then collides.

### Still unverified

The DLL registers, instantiates and is listed as an input method, and the
daemon answers correctly — but **composing text inside a real application was
not exercised**: selecting the IME is a `Win+Space` gesture that cannot be
driven reliably from a script. First run is the real test.

## Original plan

## Context

predictive-ime today is Linux-only in every layer:

| Layer | Today | Portable? |
|---|---|---|
| `engine/predict.cpp` (2052 l.) | fcitx5 addon — `InputContext`, `CandidateList`, `InputPanel`, `EventDispatcher` | **No** — fcitx5 has no Windows frontend |
| `daemon/predictord.cpp` (2598 l.) | AF_UNIX + `poll()` + XDG paths + `SIGPIPE` | **Mostly** — ~25 POSIX call sites, all in `main()` + 3 helpers |
| `ui/` (qmlpanel) | Qt Quick over a **patched** fcitx5 `zwp_input_method_v2` | **No** — Wayland-only by construction |
| `ui/preferences` | Qt widgets over `config.json` | Portable, but not on the v1 path |
| model (`words.tsv`, `bigrams.tsv`, …) | plain TSV, loaded with `std::ifstream` | **Yes** — byte-identical |

The prediction *brain* and the wire protocol are already frontend-agnostic:
one line of JSON per message over a stream socket
(`{"context":[…],"prefix":"…"} → {"candidates":[…],…}`). That seam is what
makes a Windows port tractable: the daemon is ported, not rewritten, and the
protocol is untouched.

## Goal

A native Windows 11 install with no WSL, no Qt, no admin at runtime:

```
 Windows app  (Word · Chrome · VS Code · Terminal · Notepad)
      ↕  TSF  (in-process COM, ITfThreadMgr)
 predict-tsf.dll        ← NEW: replaces the fcitx5 layer of engine/predict.cpp
      ↕  AF_UNIX stream socket, line-delimited JSON  (protocol UNCHANGED)
 predictord.exe         ← ported daemon, started by a logon task
      ↳ %LOCALAPPDATA%\ime-predictord\model\{words,bigrams,trigrams,emoji,morph}.tsv
```

Windows 10 1803+ / 11 support `AF_UNIX` `SOCK_STREAM` natively (`afunix.h`),
so the transport stays the same and the socket file inherits NTFS ACLs —
better isolation than a loopback TCP port, which any local process could reach.

## Premortem — "it's 6 months later and the Windows port failed"

| # | Failure mode | Preventive decision |
|---|---|---|
| W1 | **The TSF DLL freezes typing.** It is loaded *into every app's process*. Today the engine does a blocking `connect`+`read` on the keyboard thread with a 150 ms budget; 150 ms of frozen keyboard inside Word is not acceptable, and a wedged daemon makes it permanent. | IPC moves to a **worker thread** from the start. The key handler never blocks: it answers from the last known candidate set and repaints when the reply lands (hidden message window + `PostMessage`). This is a behaviour change vs. Linux and is the single most important design decision here. |
| W2 | **Logic forks and rots.** Copy `predict.cpp` into a `win/` tree and the two frontends drift within weeks. | Extract a **frontend-agnostic core** (`core/`) behind an abstract `Frontend` interface. fcitx5 and TSF become thin adapters. Linux keeps compiling in CI on 4 distros. |
| W3 | **Refactor silently breaks Linux.** `test-e2e.sh` / `test_predict_engine.cpp` are **not run in CI** — `build.yml` only compiles. | Make the W2 extraction strictly mechanical (move code, zero behaviour change), and **add `ctest` to `build.yml`** before touching anything. |
| W4 | **32-bit apps get nothing.** A TSF service is loaded in-process; an x64 DLL cannot load into a 32-bit app (still common: older editors, some installers). | Build and register **both x64 and x86** DLLs from the same sources. Cheap if planned from day one, painful if retrofitted. |
| W5 | **Store / sandboxed apps get nothing.** AppContainer processes cannot load a DLL without the right ACL. | Grant `ALL APPLICATION PACKAGES` read+execute on the install dir and register `GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT`. |
| W6 | **`surroundingText` degrades badly.** Agreement, recency, auto-capitalise and Backspace-revert all read the text before the caret. TSF exposes it via `ITfRange` — but only in TSF-aware apps. | Same graceful degradation as Linux: when the range read fails, fall back to the words the IME itself committed. Already the engine's documented behaviour. |
| W7 | **Model download is a dead end.** The release artifact is `.tar.zst`; Windows `tar` handles `tar` but not `zstd`. | `winget install Meta.Zstandard` (verified available), or ship an installer that pipes through a bundled `zstd.exe`. |

## Phase 0 — toolchain (verified winget IDs)

Nothing is installed on this machine: no MSVC, no CMake, no real Python (the
`python.exe` on PATH is the Microsoft Store alias stub), WSL has only
`docker-desktop`.

```
winget install Microsoft.VisualStudio.2022.BuildTools   # + "Desktop development with C++" workload, Windows 11 SDK
winget install Kitware.CMake Ninja-build.Ninja Git.Git Meta.Zstandard
git clone https://github.com/microsoft/vcpkg && .\vcpkg\bootstrap-vcpkg.bat
vcpkg install nlohmann-json:x64-windows curl:x64-windows nlohmann-json:x86-windows curl:x86-windows
```

The Windows SDK is what supplies `msctf.h` / `ctffunc.h` (TSF) — the DLL needs
no third-party dependency at all. ~3–6 GB of downloads; this is the slow step.

## Phase 1 — port `predictord` to Windows

One new header, `daemon/win_compat.h`, plus a short diff. The whole POSIX
surface, by line:

| `predictord.cpp` | Linux | Windows |
|---|---|---|
| 1978 | `signal(SIGPIPE, SIG_IGN)` | no-op (no SIGPIPE) |
| 1983 | `find_last_of('/')` | `find_last_of("/\\")` |
| 1992–2007 | `$XDG_DATA_HOME` / `$XDG_CONFIG_HOME` / `$HOME` | `%LOCALAPPDATA%\ime-predictord` (user.log, user.tri.log, veto.log, socket) and `%APPDATA%\ime-predictord` (config.json, dict.txt, snippets.tsv) |
| 1998–1999 | `mkdir(p, 0755)` | `CreateDirectoryW` |
| 786 | `::stat` → `st_mtime` (hot-reload) | `_stat64` — same field, works as-is |
| 2052–2054, 2061, 2211, 2497 | wake **pipe** + `fcntl(O_NONBLOCK)` | **loopback socketpair** — `WSAPoll` cannot watch a pipe handle, only sockets |
| 2217–2218 | `/tmp/ime-predictord.sock`, `unlink` | `%LOCALAPPDATA%\ime-predictord\predictord.sock`, `_unlink` |
| 2219, 2492 | `SOCK_NONBLOCK` / `fcntl` | `ioctlsocket(FIONBIO)` |
| 2484 | `poll()` | `WSAPoll()` (identical `pollfd`) |
| 2552, 2497 | `read(fd,…)` | `recv(fd,…)` |
| 2577 | `send(…, MSG_NOSIGNAL)` | `send(…, 0)` |
| 2590 | `close()` | `closesocket()` |
| top of `main` | — | `WSAStartup(MAKEWORD(2,2), …)` |

Build flags: `/utf-8` is **mandatory** — the sources are UTF-8 with French
accents and the model code walks UTF-8 bytes by hand; MSVC otherwise reads
them as the system codepage. Plus `_CRT_SECURE_NO_WARNINGS` for `strncpy`.

`reformulate_http.cpp` needs no change: libcurl builds on Windows (Schannel
backend via vcpkg). `WITH_NEURAL` (llama.cpp) stays **OFF** for v1.

Autostart: a hidden **Task Scheduler task at logon** (not a Windows service —
the daemon must run in the user session to see `%APPDATA%` and write the
learned-words journals).

**Exit criterion:** `predictord.exe model\words.tsv` answers a hand-sent
`{"context":["je"],"prefix":"v"}` with the same candidates as Linux.

## Phase 2 — extract the frontend-agnostic core

`engine/predict.cpp` is 2052 lines, of which ~412 touch `fcitx` / `ic->`.
Split into:

- **`core/`** (no fcitx, no win32, ~1200 l.): `EngineCfg` + hot-reload,
  `config.json` `lang` rewrite-in-place, UTF-8 helpers, case handling
  (`applyCase`, `capFirst`), French thin-space + auto-capitalise rules,
  the daemon IPC client, and the **input state machine** — driven through an
  abstract `Frontend`:

  ```cpp
  struct Frontend {                     // 6 verbs is the whole coupling
    virtual void commitText(const std::string&)                  = 0;
    virtual void setPreedit(const std::string&, size_t cursor)    = 0;
    virtual void setCandidates(const std::vector<Candidate>&, int sel) = 0;
    virtual void setAux(const std::string&)                      = 0;
    virtual bool textBeforeCursor(std::string& out)              = 0;  // surroundingText
    virtual void deleteBeforeCursor(unsigned n)                  = 0;
  };
  ```

- **`engine/`** — fcitx5 adapter, behaviour unchanged, still the Linux build.
- **`win/tsf/`** — the TSF adapter (Phase 3).

Strictly mechanical: move code, rename nothing, change no behaviour. Guarded
by the 4-distro CI compile *plus* `ctest` added to `build.yml` first (W3).

## Phase 3 — the TSF text service (`predict-tsf.dll`)

The bulk of the work: ~2000–2500 lines of C++/COM.

| File | Contents |
|---|---|
| `dllmain.cpp` | `DllMain`, `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer` / `DllUnregisterServer` |
| `TextService.cpp` | `ITfTextInputProcessorEx`, `ITfThreadMgrEventSink`, `ITfKeyEventSink`, `ITfThreadFocusSink`, `ITfCompositionSink`, `ITfDisplayAttributeProvider` |
| `EditSession.cpp` | `ITfEditSession` helpers: start/update/end composition, read the range before the caret (→ `textBeforeCursor`), delete N chars back (→ Backspace revert) |
| `CandidateWindow.cpp` | Layered topmost `HWND`, positioned with `ITfContextView::GetTextExt` — the Windows answer to `qmlpanel`. Own-drawn, DPI-aware, follows the caret |
| `AsyncClient.cpp` | Worker thread owning the socket; results posted to the TSF thread via a hidden message window (W1) — also how the two-phase `"pending":true` neural refresh arrives |
| `Register.cpp` | `ITfInputProcessorProfileMgr::RegisterProfile` + categories: `GUID_TFCAT_TIP_KEYBOARD`, `TIPCAP_UIELEMENTENABLED`, `TIPCAP_SECUREMODE`, `TIPCAP_IMMERSIVESUPPORT`, `TIPCAP_SYSTRAYSUPPORT`, `DISPLAYATTRIBUTEPROVIDER` |
| `KeyMap.cpp` | fcitx keysyms → Win32 `VK_*` (+ `ToUnicodeEx` for the character payload; AZERTY-aware, since the autocorrect noisy channel is keyboard-layout sensitive) |

Mapping of the verbs:

| fcitx5 | TSF |
|---|---|
| `ic->commitString(s)` | `ITfRange::SetText` in an edit session, then `EndComposition` |
| `inputPanel().setClientPreedit` + `updatePreedit` | `ITfComposition` + `TF_ATTR_INPUT` display attribute (the ghost-text underline) |
| `inputPanel().setCandidateList` | our own `CandidateWindow` (the system draws nothing unless the *app* implements `ITfUIElementSink` — so never rely on it) |
| `ic->surroundingText()` | `ITfContext::GetStart` + `ITfRange::ShiftStart` backwards, inside a read-only edit session |
| `deleteSurroundingBefore(n)` | range `ShiftStart(-n)` + `SetText("")` |
| `keyEvent(...)` | `ITfKeyEventSink::OnTestKeyDown` (claim) / `OnKeyDown` (act) |
| `EventDispatcher` + FD watch | worker thread + `PostMessage` to a hidden window |

Both `x64` and `x86` are built and registered (W4).

## Phase 4 — packaging

An Inno Setup installer that: copies `predict-tsf.dll` (x64+x86) and
`predictord.exe`, runs `regsvr32`, registers the profile under
`fr-FR`/`en-US`, creates the logon task, fetches + unpacks the model release
into `%LOCALAPPDATA%\ime-predictord\model\`, and sets the AppContainer ACL
(W5). Uninstall reverses all of it.

## Explicitly out of scope for v1

- `qmlpanel` (Wayland by construction) — replaced by `CandidateWindow`.
- `ime-preferences` (Qt) — v1 edits `config.json` by hand; a small Win32
  settings dialog is a follow-up.
- The neural predictor (`WITH_NEURAL`, llama.cpp) — `OFF`, as it is on Linux.
- Emoji picker and reformulation: they ride on the same protocol and should
  work for free, but they are verified last, not first.

## Effort

| Phase | Scope | Estimate |
|---|---|---|
| 0 | toolchain | ~1 h, mostly downloads |
| 1 | daemon port | ~300 l. of diff + shim — 1 session |
| 2 | core extraction | mechanical, ~2000 l. moved — 1 session |
| 3 | TSF service | ~2200 l. new C++/COM — 2–4 sessions |
| 4 | installer | ~1 session |

Phase 1 is independently useful and independently testable: at its end you
have a working Windows prediction daemon, which de-risks everything after it.
