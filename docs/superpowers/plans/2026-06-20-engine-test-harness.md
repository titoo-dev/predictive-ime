# Engine Test Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a headless C++ test harness that drives the real `libpredict.so` fcitx5 engine via fcitx5's `testfrontend`, with an in-process mock daemon, asserting commits / candidates / preedit and crash-freedom across client capabilities.

**Architecture:** A test executable (`engine/test_predict_engine.cpp`) boots a real `fcitx::Instance` with `setupTestingEnvironment` + the `testim`/`testfrontend` addons + the `predict` addon. An in-process MockDaemon (Unix socket thread) feeds the engine scripted JSON replies (engine reaches it via `IME_PREDICTORD_SOCK`). Tests run inside `EventDispatcher::schedule`, send keys with `TestFrontend::keyEvent`, and verify commits with `pushCommitExpectation` plus direct `InputContext::inputPanel()` inspection.

**Tech Stack:** C++20, fcitx5 (`Fcitx5::Core` + `Fcitx5ModuleTestFrontend`/`TestIM`), nlohmann_json, POSIX sockets + pthread, CMake (`BUILD_TESTING`), Nix flake `checks`.

## Global Constraints

- `predict.cpp` MUST remain unchanged (the engine is exercised through its public fcitx5 surface, not modified).
- C++ standard: `CMAKE_CXX_STANDARD 20` (matches `engine/CMakeLists.txt`).
- The engine connects to the daemon via the env var `IME_PREDICTORD_SOCK` (already supported in `connectDaemon`); the harness MUST set it before constructing the `Instance`.
- The engine reads config from `$XDG_CONFIG_HOME/ime-predictord/config.json` (hot-reloaded on mtime); the harness MUST set `XDG_CONFIG_HOME` to a temp dir and bump file mtimes by ≥1s between rewrites (same constraint as `daemon/test_predict.py`).
- No graphical session: UI addon is `testui`; never load `qmlpanel`.
- fcitx5 test addons live in the fcitx5 package: `<fcitx5>/lib/fcitx5` (`libtestfrontend.so`, `libtestim.so`, `libtestui.so`) and their `.conf` in `<fcitx5>/share/fcitx5/addon`. The built `predict` lives in the CMake build dir; its `.conf` must be reachable under a `…/addon/predict.conf` data dir.

---

## File Structure

- `engine/test_predict_engine.cpp` — NEW. The whole harness: MockDaemon, fcitx5 bootstrap, test DSL, and all test cases (single file, mirrors `daemon/test_predict.py`'s one-file style).
- `engine/CMakeLists.txt` — MODIFY. Add an optional `test_predict_engine` target behind `BUILD_TESTING`, copy `predict.conf` into the test data layout, register a CTest test.
- `flake.nix` — MODIFY. Add `checks.${system}.engine` (builds + runs the harness) and `checks.${system}.daemon` (formalises the existing `daemon/test_predict.py`).

---

### Task 1: Walking skeleton — MockDaemon + fcitx5 bootstrap + one passing assertion

This task locks down the two hardest, riskiest pieces (the fcitx5 test wiring and the build), proving the whole approach compiles and runs before any behavior coverage is added.

**Files:**
- Create: `engine/test_predict_engine.cpp`
- Modify: `engine/CMakeLists.txt`
- Test: the executable IS the test.

**Interfaces:**
- Produces: a `MockDaemon` class (`start()`, `stop()`, `setReply(candidates, autocomplete, literalIsWord)`, `std::string lastPrefix()`, `std::vector<std::string> lastContext()`); `runTests()` entry; a global `int g_failures`.
- Consumes: fcitx5 `setupTestingEnvironment`, `Instance`, `AddonInstance::call<>`, `ITestFrontend::{createInputContext,keyEvent,pushCommitExpectation}`.

- [ ] **Step 1: Write the failing test (the skeleton itself)**

Create `engine/test_predict_engine.cpp`:

```cpp
// Harnais de test de l'engine fcitx5 « predict » : pilote le VRAI libpredict.so
// dans un fcitx::Instance headless (testfrontend/testim), avec un mock daemon
// in-process. Vérifie commits / candidats / preedit sans session graphique.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>

#include "testfrontend_public.h"

using json = nlohmann::json;
namespace {

int g_failures = 0;
void check(const std::string &name, bool cond, const std::string &detail = "") {
  printf("%s %s%s\n", cond ? "  ok  " : " FAIL ", name.c_str(),
         detail.empty() ? "" : ("  [" + detail + "]").c_str());
  if (!cond)
    ++g_failures;
}

// --- Mock daemon : un thread qui écoute sur un socket Unix et répond du JSON
//     scripté. L'engine s'y connecte via IME_PREDICTORD_SOCK. ---
class MockDaemon {
public:
  explicit MockDaemon(std::string sockPath) : path_(std::move(sockPath)) {}
  ~MockDaemon() { stop(); }

  void setReply(std::vector<std::string> cands, std::string autocomplete = "",
                bool literalIsWord = false) {
    cands_ = std::move(cands);
    autocomplete_ = std::move(autocomplete);
    literalIsWord_ = literalIsWord;
  }
  std::string lastPrefix() const { return lastPrefix_; }
  std::vector<std::string> lastContext() const { return lastContext_; }

  void start() {
    ::unlink(path_.c_str());
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd_, (sockaddr *)&addr, sizeof(addr)) < 0 || ::listen(fd_, 16) < 0) {
      perror("mock bind/listen");
      std::abort();
    }
    run_ = true;
    th_ = std::thread([this] { loop(); });
  }
  void stop() {
    if (!run_)
      return;
    run_ = false;
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (th_.joinable())
      th_.join();
    ::unlink(path_.c_str());
  }

private:
  void loop() {
    while (run_) {
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0)
        return; // socket fermé → fin
      std::string buf;
      char tmp[4096];
      ssize_t n;
      while ((n = ::read(c, tmp, sizeof(tmp))) > 0) {
        buf.append(tmp, n);
        if (!buf.empty() && buf.back() == '\n')
          break;
      }
      try {
        json req = json::parse(buf);
        if (req.contains("prefix")) { // requête de prédiction
          lastPrefix_ = req.value("prefix", "");
          lastContext_.clear();
          for (auto &w : req.value("context", json::array()))
            lastContext_.push_back(w.get<std::string>());
          json resp;
          resp["candidates"] = cands_;
          resp["autocomplete"] = autocomplete_;
          resp["literalIsWord"] = literalIsWord_;
          std::string out = resp.dump() + "\n";
          ::send(c, out.data(), out.size(), MSG_NOSIGNAL);
        }
        // learn/forget/veto : fire-and-forget, pas de réponse requise.
      } catch (...) {
      }
      ::close(c);
    }
  }
  std::string path_;
  int fd_ = -1;
  std::atomic<bool> run_{false};
  std::thread th_;
  std::vector<std::string> cands_;
  std::string autocomplete_;
  bool literalIsWord_ = false;
  std::string lastPrefix_;
  std::vector<std::string> lastContext_;
};

// helpers d'environnement (sockets/config temporaires)
std::string g_tmpDir; // rempli dans main()

void typeKey(fcitx::AddonInstance *tf, fcitx::ICUUID uuid, const char *k) {
  tf->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(k), false);
}

} // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);

  // 1) répertoires temporaires : socket daemon + XDG_CONFIG_HOME.
  char tmpl[] = "/tmp/ime-engine-test-XXXXXX";
  g_tmpDir = ::mkdtemp(tmpl);
  std::string sockPath = g_tmpDir + "/predictord.sock";
  ::setenv("IME_PREDICTORD_SOCK", sockPath.c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (g_tmpDir + "/cfg").c_str(), 1);

  MockDaemon daemon(sockPath);
  daemon.setReply({"bonjour"}, "bonjour", false);
  daemon.start();

  // 2) bootstrap fcitx5 de test. PREDICT_ADDON_DIR / FCITX_ADDON_DIR /
  //    TEST_DATA_DIR sont fournis par CMake en -D (cf CMakeLists).
  fcitx::setupTestingEnvironment(
      TEST_BINARY_DIR, {PREDICT_ADDON_DIR, FCITX_ADDON_DIR},
      {TEST_DATA_DIR, FCITX_DATA_DIR});

  char a0[] = "test", a1[] = "--disable=all",
       a2[] = "--enable=testim,testfrontend,predict", a3[] = "--ui=testui";
  char *argv[] = {a0, a1, a2, a3};
  fcitx::Instance instance(4, argv);
  instance.addonManager().registerDefaultLoader(nullptr);

  instance.eventDispatcher().schedule([&] {
    auto *tf = instance.addonManager().addon("testfrontend", true);
    auto uuid = tf->call<fcitx::ITestFrontend::createInputContext>("app");
    auto *ic = instance.inputContextManager().findByUUID(uuid);
    ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    ic->focusIn();
    instance.setCurrentInputMethod("predict");

    // skeleton: taper "bonjour" puis Espace → commit "bonjour ".
    tf->call<fcitx::ITestFrontend::pushCommitExpectation>("bonjour ");
    for (const char *k : {"b", "o", "n", "j", "o", "u", "r"})
      typeKey(tf, uuid, k);
    typeKey(tf, uuid, "space");
    check("skeleton: 'bonjour'+space committe 'bonjour '", true);

    instance.exit();
  });

  instance.exec();
  daemon.stop();
  if (g_failures) {
    printf("\n%d test(s) en échec\n", g_failures);
    return 1;
  }
  printf("\ntous les tests passent\n");
  return 0;
}
```

- [ ] **Step 2: Wire the CMake test target**

Append to `engine/CMakeLists.txt`:

```cmake
include(CTest)
if(BUILD_TESTING)
  find_package(Fcitx5ModuleTestFrontend REQUIRED)
  find_package(Fcitx5ModuleTestIM REQUIRED)

  # predict.conf doit être trouvable sous <data>/inputmethod et <data>/addon
  # par setupTestingEnvironment → on prépare une arbo dans le build dir.
  set(TEST_DATA "${CMAKE_CURRENT_BINARY_DIR}/testdata")
  file(MAKE_DIRECTORY "${TEST_DATA}/inputmethod" "${TEST_DATA}/addon")
  configure_file(predict.conf "${TEST_DATA}/inputmethod/predict.conf" COPYONLY)
  configure_file(predict-addon.conf "${TEST_DATA}/addon/predict.conf" COPYONLY)

  # chemin du paquet fcitx5 (addons + data de test) déduit du module Core.
  get_target_property(_f5core_inc Fcitx5::Core INTERFACE_INCLUDE_DIRECTORIES)

  add_executable(test_predict_engine test_predict_engine.cpp)
  target_link_libraries(test_predict_engine
    Fcitx5::Core Fcitx5::Module::TestFrontend nlohmann_json::nlohmann_json
    Threads::Threads)
  target_compile_definitions(test_predict_engine PRIVATE
    TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    PREDICT_ADDON_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    TEST_DATA_DIR="${TEST_DATA}"
    FCITX_ADDON_DIR="${FCITX5_ADDON_DIR}"
    FCITX_DATA_DIR="${FCITX5_DATA_DIR}")
  add_test(NAME engine COMMAND test_predict_engine)
endif()
```

Add near the top of `engine/CMakeLists.txt` (after `find_package(Fcitx5Core REQUIRED)`):

```cmake
find_package(Threads REQUIRED)
# Dossiers du paquet fcitx5 pour les addons de test + data (chemins absolus).
# Fcitx5::Module::TestFrontend fournit le .so ; ses addons voisins (testim,
# testui) et les .conf sont dans le même paquet.
set(FCITX5_ADDON_DIR "${Fcitx5Core_LIBRARY_DIR}/fcitx5" CACHE PATH "")
set(FCITX5_DATA_DIR "${Fcitx5Core_PREFIX}/share/fcitx5" CACHE PATH "")
```

> Note for implementer: the exact CMake variable names exposing the fcitx5 prefix (`Fcitx5Core_PREFIX`, `Fcitx5Core_LIBRARY_DIR`) may differ; if they are empty, derive both from `Fcitx5::Core`'s imported location with `get_filename_component`. Verify by printing them with `message(STATUS ...)` during Step 3.

- [ ] **Step 3: Run to verify it builds and the skeleton passes**

Run (from the worktree):
```bash
nix shell nixpkgs#cmake nixpkgs#gcc nixpkgs#pkg-config nixpkgs#fcitx5 nixpkgs#nlohmann_json nixpkgs#extra-cmake-modules --command bash -c '
  cmake -S engine -B /tmp/eng-build -DBUILD_TESTING=ON && cmake --build /tmp/eng-build &&
  /tmp/eng-build/test_predict_engine'
```
Expected: builds; prints `ok  skeleton: 'bonjour'+space committe 'bonjour '`; exit 0.

If the fcitx5 wiring is wrong (addon not found, IM not switched, commit mismatch), iterate here — this is the de-risking step. Print `message(STATUS)` of the data/addon dirs and, if needed, `setenv("FCITX_LOG_RULES", "*=debug")` before `Instance` to see addon loading.

- [ ] **Step 4: Commit**

```bash
git add engine/test_predict_engine.cpp engine/CMakeLists.txt
git commit -m "test(engine): harnais TestFrontend + mock daemon (walking skeleton)"
```

---

### Task 2: Test DSL (state setters, input, assertions)

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Produces: a `Harness` struct holding `instance`, `tf`, `uuid`, `ic`, `daemon`, with methods: `setConfig(json)`, `setCaps(fcitx::CapabilityFlags)`, `setSurrounding(text, cursor)`, `type(utf8)`, `key(const char* sym)`, `expectCommit(str)`, `candidates()→vector<string>`, `preedit()→string`. These wrap the raw fcitx5/testfrontend calls so test cases read declaratively.
- Consumes: Task 1's `MockDaemon`, the bootstrapped `instance`/`tf`/`uuid`/`ic`.

- [ ] **Step 1: Write the DSL test (a behavior expressed via the DSL)**

Replace the inline skeleton body inside `schedule` with a `Harness` and one DSL-based test. Add above `main`:

```cpp
struct Harness {
  fcitx::Instance *instance;
  fcitx::AddonInstance *tf;
  fcitx::ICUUID uuid;
  fcitx::InputContext *ic;
  MockDaemon *daemon;
  int bump = 0;

  void setConfig(const json &j) {
    std::string dir = g_tmpDir + "/cfg/ime-predictord";
    std::string p = dir + "/config.json";
    FILE *f = ::fopen(p.c_str(), "w");
    std::string s = j.dump();
    ::fwrite(s.data(), 1, s.size(), f);
    ::fclose(f);
    // mtime distinct (engineCfg() recharge sur mtime en SECONDES).
    bump += 2;
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = ::time(nullptr) + bump;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, p.c_str(), times, 0);
  }
  void setCaps(fcitx::CapabilityFlags caps) { ic->setCapabilityFlags(caps); }
  void setSurrounding(const std::string &text, unsigned cursor) {
    ic->surroundingText().setText(text, cursor, cursor);
    ic->updateSurroundingText();
  }
  void type(const std::string &utf8) {
    for (char c : utf8) { // ASCII des tests ; clés Unicode via key()
      std::string k(1, c);
      if (c == ' ') k = "space";
      tf->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(k), false);
    }
  }
  void key(const char *sym) {
    tf->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(sym), false);
  }
  void expectCommit(const std::string &s) {
    tf->call<fcitx::ITestFrontend::pushCommitExpectation>(s);
  }
  std::vector<std::string> candidates() {
    std::vector<std::string> out;
    auto list = ic->inputPanel().candidateList();
    if (list)
      for (int i = 0; i < list->size(); i++)
        out.push_back(list->candidate(i).text().toStringForCommit());
    return out;
  }
  std::string preedit() {
    return ic->inputPanel().clientPreedit().toStringForCommit();
  }
};
```

Required extra includes at top: `#include <fcitx/candidatelist.h>`, `#include <fcitx/inputpanel.h>`, `#include <fcitx/text.h>`, `#include <sys/stat.h>`, `#include <ctime>`.

Inside `schedule`, build the harness and convert the skeleton test to DSL form:

```cpp
Harness h{&instance, tf, uuid, ic, &daemon};
h.setCaps(fcitx::CapabilityFlag::Preedit);

// candidats affichés + commit sur Espace
daemon.setReply({"bonjour"}, "bonjour", false);
h.expectCommit("bonjour ");
h.type("bonjour");
check("dsl: candidats affichés", h.candidates() == std::vector<std::string>{"bonjour"},
      h.candidates().empty() ? "vide" : h.candidates()[0]);
h.type(" ");
```

- [ ] **Step 2: Run to verify it fails (if the DSL has a bug) then passes**

Run the Step-3 build command from Task 1.
Expected: `ok  dsl: candidats affichés`; commit expectation satisfied; exit 0. If `toStringForCommit()` / `clientPreedit()` names differ, fix against `fcitx/text.h` and `fcitx/inputpanel.h` (verify with the headers in the fcitx5 package).

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): DSL du harnais (config/caps/surrounding/type/assertions)"
```

---

### Task 3: Core composition behaviors (commit, casing, literalIsWord, ghost)

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Consumes: `Harness` (Task 2).

- [ ] **Step 1: Write the tests**

Add a `void coreTests(Harness &h)` called from `schedule` after the DSL test:

```cpp
void coreTests(Harness &h) {
  // casse : "Bonjou" → "Bonjour " (report de la majuscule initiale)
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("Bonjour ");
  h.type("Bonjou"); h.type(" ");
  check("casse: Bonjou→Bonjour", true);

  // literalIsWord : un vrai mot n'est pas auto-remplacé (commit du littéral)
  h.daemon->setReply({"le", "les"}, "", /*literalIsWord=*/true);
  h.expectCommit("le ");
  h.type("le"); h.type(" ");
  check("literalIsWord: 'le' gardé", true);

  // ghost text : autocomplete prolonge la frappe → ghost dans le preedit
  h.daemon->setReply({"content"}, "content", false);
  h.type("conten");
  check("ghost: preedit montre 'conten' + ghost",
        h.preedit().rfind("conten", 0) == 0, h.preedit());
  h.key("Escape"); // nettoie la composition pour le test suivant
}
```

> Note: `Key("Escape")` / `Key("space")` use fcitx keysym names. Capitals are sent as a single `Key("B")` would be Shift+b; the engine derives case from the keysym. If `type("Bonjou")` sends lowercase, send the capital explicitly: `h.key("B")` then `h.type("onjou")`. Verify the committed case in the run; adjust the helper if needed.

- [ ] **Step 2: Run — expect all `ok`**

Run the build command. Expected: 3 new `ok` lines.

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): composition — casse, literalIsWord, ghost text"
```

---

### Task 4: Backspace & bar-close (incl. the Ctrl+Backspace regression)

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Consumes: `Harness`.

- [ ] **Step 1: Write the tests**

```cpp
void backspaceTests(Harness &h) {
  h.setCaps(fcitx::CapabilityFlag::Preedit | fcitx::CapabilityFlag::SurroundingText);

  // Backspace en composition réduit le buffer ; vidé → barre fermée
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.type("bo");
  h.key("BackSpace"); h.key("BackSpace");
  check("backspace: buffer vidé → barre fermée", h.candidates().empty(),
        h.candidates().empty() ? "" : h.candidates()[0]);

  // RÉGRESSION : barre mot-suivant ouverte, Ctrl+Backspace (buffer vide) la ferme
  h.daemon->setReply({"suivant", "autre"}, "", false);
  h.type("a"); h.type(" "); // commit → barre mot-suivant
  check("pré: barre mot-suivant ouverte", !h.candidates().empty());
  h.key("Control+BackSpace");
  check("régression: Ctrl+Backspace ferme la barre (input vidé)",
        h.candidates().empty(), h.candidates().empty() ? "" : h.candidates()[0]);
}
```

- [ ] **Step 2: Run — expect `ok` (and confirm the regression test passes on current HEAD, which has the bar-close fix)**

Run the build command. Expected: all `ok`. (On a tree *without* the bar-close fix, the last check would FAIL — that is the regression guard.)

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): Backspace édition + fermeture barre (régression Ctrl+Backspace)"
```

---

### Task 5: Capability paths + Password (the keyboard-crash class)

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Consumes: `Harness`.

- [ ] **Step 1: Write the tests**

```cpp
void capabilityTests(Harness &h) {
  // (a) client SANS Preedit : doit committer correctement et NE PAS planter.
  h.setCaps(fcitx::CapabilityFlags{}); // ni Preedit ni Surrounding
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("bonjour ");
  h.type("bonjour"); h.type(" ");
  check("caps: sans Preedit, commit OK + pas de crash", true);

  // (b) client AVEC Preedit : idem.
  h.setCaps(fcitx::CapabilityFlag::Preedit);
  h.daemon->setReply({"merci"}, "merci", false);
  h.expectCommit("merci ");
  h.type("merci"); h.type(" ");
  check("caps: avec Preedit, commit OK", true);

  // (c) champ Password : aucune prédiction, aucun preedit, touches passent.
  h.setCaps(fcitx::CapabilityFlag::Password);
  h.daemon->setReply({"secret"}, "secret", false);
  h.type("abc");
  check("password: pas de barre", h.candidates().empty());
  check("password: pas de preedit", h.preedit().empty(), h.preedit());
}
```

> The value of this task: it exercises both `CapabilityFlag::Preedit` present/absent (the area whose mishandling crashed fcitx5). The test reaching `instance.exit()` without abort *is* the crash-freedom assertion.

- [ ] **Step 2: Run — expect `ok`**

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): chemins de capacité Preedit + champ Password (anti-crash)"
```

---

### Task 6: Triggers, navigation, punctuation, next-word

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Consumes: `Harness`.

- [ ] **Step 1: Write the tests**

```cpp
void interactionTests(Harness &h) {
  h.setCaps(fcitx::CapabilityFlag::Preedit | fcitx::CapabilityFlag::SurroundingText);

  // emoji : ':coeur' → ❤️ proposé, Espace committe l'emoji
  h.daemon->setReply({"\xE2\x9D\xA4\xEF\xB8\x8F"}, "\xE2\x9D\xA4\xEF\xB8\x8F", false);
  h.expectCommit("\xE2\x9D\xA4\xEF\xB8\x8F "); // ❤️ + espace
  h.type(":coeur"); h.type(" ");
  check("emoji: ':coeur'→❤️ committé", true);

  // navigation : Tab surligne le 1er candidat, Espace le committe
  h.daemon->setReply({"alpha", "beta"}, "", false);
  h.expectCommit("alpha ");
  h.type("al"); h.key("Tab"); h.key("space");
  check("nav: Tab puis Espace committe le candidat surligné", true);

  // ponctuation : committe le mot puis la touche file à l'app (pas avalée)
  h.daemon->setReply({"fin"}, "fin", true);
  h.expectCommit("fin");
  h.type("fin"); h.key("period");
  check("ponctuation: mot committé sans espace, '.' file à l'app", true);
}
```

> `sendKeyEvent` returns whether the engine accepted the key; for the punctuation case the implementer can additionally assert `tf->call<ITestFrontend::sendKeyEvent>(...) == false` to prove `.` was NOT swallowed. Use that variant if `pushCommitExpectation` alone is ambiguous.

- [ ] **Step 2: Run — expect `ok`**

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): emoji/snippet, navigation Tab, ponctuation"
```

---

### Task 7: Opt-in features — frenchSpacing & autoCapitalize

**Files:**
- Modify: `engine/test_predict_engine.cpp`

**Interfaces:**
- Consumes: `Harness` (`setConfig`).

- [ ] **Step 1: Write the tests**

```cpp
void optInTests(Harness &h) {
  h.setCaps(fcitx::CapabilityFlag::Preedit | fcitx::CapabilityFlag::SurroundingText);

  // frenchSpacing : fine insécable U+202F avant '!'
  h.setConfig({{"frenchSpacing", true}});
  h.daemon->setReply({"bonjour"}, "", true);
  h.expectCommit("bonjour");           // le mot
  h.expectCommit("\xE2\x80\xAF");       // U+202F inséré avant '!'
  h.type("bonjour"); h.key("exclam");
  check("frenchSpacing: U+202F avant '!'", true);

  // autoCapitalize : début de champ → 1re lettre capitalisée
  h.setConfig({{"autoCapitalize", true}});
  h.setSurrounding("", 0); // champ vide → début de phrase
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("Bonjour ");
  h.type("bonjour"); h.type(" ");
  check("autoCapitalize: début de champ → Bonjour", true);

  h.setConfig({}); // reset
}
```

> `Key("exclam")` is the keysym for `!`. Confirm U+202F is committed *before* the app receives `!` (the engine commits the thin space, then lets `!` through). If `pushCommitExpectation` ordering is strict FIFO, the two expectations above must match the engine's commit order; adjust if the engine commits word+thinspace as one string.

- [ ] **Step 2: Run — expect `ok`**

- [ ] **Step 3: Commit**

```bash
git add engine/test_predict_engine.cpp
git commit -m "test(engine): frenchSpacing (U+202F) + autoCapitalize (opt-in)"
```

---

### Task 8: Nix flake checks (engine + daemon)

**Files:**
- Modify: `flake.nix`

**Interfaces:**
- Consumes: the `test_predict_engine` target (Task 1) and `daemon/test_predict.py`.

- [ ] **Step 1: Add the checks output**

In `flake.nix`, inside the per-system outputs, add:

```nix
checks.${system} = {
  # Engine : build le harnais (BUILD_TESTING) et l'exécute.
  engine = pkgs.stdenv.mkDerivation {
    pname = "fcitx5-predict-engine-test";
    version = "0.1";
    src = ./engine;
    nativeBuildInputs = [ pkgs.cmake pkgs.kdePackages.extra-cmake-modules pkgs.pkg-config ];
    buildInputs = [ pkgs.fcitx5 pkgs.nlohmann_json ];
    cmakeFlags = [ "-DBUILD_TESTING=ON" ];
    doCheck = true;
    checkPhase = "./test_predict_engine";
    installPhase = "touch $out";
  };

  # Daemon : la suite comportementale existante.
  daemon = pkgs.runCommand "ime-predictord-test"
    { nativeBuildInputs = [ pkgs.python3 ]; } ''
    python3 ${./daemon/test_predict.py} ${self.packages.${system}.predictord}/bin/predictord
    touch $out
  '';
};
```

> Note: if the engine test needs the fcitx5 test addons at runtime, they ship inside `pkgs.fcitx5` (`lib/fcitx5/libtestfrontend.so` etc.) and `setupTestingEnvironment` is given their dir via the CMake `-D` defs — no extra runtime inputs needed. Verify in Step 2; if the addons aren't found, add their dir to the compile defs from `${pkgs.fcitx5}/lib/fcitx5`.

- [ ] **Step 2: Run the checks**

```bash
nix flake check 2>&1 | tail -20
```
Expected: both `engine` and `daemon` checks build and pass.

- [ ] **Step 3: Commit**

```bash
git add flake.nix
git commit -m "ci(flake): checks engine (harnais) + daemon (test_predict.py)"
```

---

## Self-Review

**Spec coverage:** Walking skeleton + DSL (Tasks 1-2) ⇒ infra. Behaviors 1-20 from the spec map to: composition/casing/literalIsWord/ghost (T3); Backspace/bar-close incl. Ctrl+Backspace (T4); capability paths + Password (T5); emoji/snippet/Tab/punctuation/next-word (T6); frenchSpacing/autoCapitalize (T7); Nix integration (T8). Revert-window (#15), SurroundingText context (#19), multi-word (#20), and `nextWordBar:false` (#8) are NOT yet dedicated tasks — **add them as T6b/T7b** following the same pattern once the harness is proven (deferred to keep the first pass focused; flagged here so coverage is explicit, not silently dropped).

**Placeholder scan:** No "TBD/handle edge cases". The two "Note for implementer" blocks flag genuinely uncertain fcitx5 API names (CMake prefix vars, `toStringForCommit`/`clientPreedit`, keysym case) to verify against headers during the build step — these are verification cues, not missing content; the code is written in full.

**Type consistency:** `MockDaemon` API (`setReply`/`lastPrefix`/`lastContext`) and `Harness` API (`setConfig`/`setCaps`/`setSurrounding`/`type`/`key`/`expectCommit`/`candidates`/`preedit`) are used consistently across T3-T7. `ITestFrontend::{createInputContext,keyEvent,sendKeyEvent,pushCommitExpectation}` match `testfrontend_public.h`.

**Known risk:** Task 1 (fcitx5 wiring) is the make-or-break. If the test addons / IM activation can't be made to work in the Nix sandbox, fall back to running the harness as a `nix develop` dev-shell check rather than a sandboxed `flake check` (the harness itself is unchanged). This does not affect Tasks 2-7.
