// Harnais de test de l'engine fcitx5 « predict » : pilote le VRAI libpredict.so
// dans un fcitx::Instance headless (testfrontend/testim), avec un mock daemon
// in-process. Vérifie commits / candidats / preedit sans session graphique.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
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
    if (::bind(fd_, (sockaddr *)&addr, sizeof(addr)) < 0 ||
        ::listen(fd_, 16) < 0) {
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

std::string g_tmpDir;

void typeKey(fcitx::AddonInstance *tf, fcitx::ICUUID uuid, const char *k) {
  tf->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(k), false);
}

} // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);

  char tmpl[] = "/tmp/ime-engine-test-XXXXXX";
  g_tmpDir = ::mkdtemp(tmpl);
  std::string sockPath = g_tmpDir + "/predictord.sock";
  ::setenv("IME_PREDICTORD_SOCK", sockPath.c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (g_tmpDir + "/cfg").c_str(), 1);

  MockDaemon daemon(sockPath);
  daemon.setReply({"bonjour"}, "bonjour", false);
  daemon.start();

  fcitx::setupTestingEnvironment(
      TEST_BINARY_DIR, {PREDICT_ADDON_DIR, FCITX_ADDON_DIR},
      {TEST_DATA_DIR, FCITX_DATA_DIR});

  char a0[] = "test", a1[] = "--disable=all",
       a2[] = "--enable=testim,testfrontend,predict", a3[] = "--ui=testui";
  char *argv[] = {a0, a1, a2, a3};
  fcitx::Instance instance(4, argv);
  instance.addonManager().registerDefaultLoader(nullptr);

  instance.eventDispatcher().schedule([&] {
    // Groupe d'IM explicite contenant predict (sinon fcitx génère un groupe par
    // défaut clavier-only et nos touches n'atteignent jamais l'engine).
    auto &imMgr = instance.inputMethodManager();
    fcitx::InputMethodGroup group("Default");
    group.inputMethodList().push_back(fcitx::InputMethodGroupItem("predict"));
    group.setDefaultInputMethod("predict");
    imMgr.setGroup(std::move(group));
    imMgr.save();

    auto *tf = instance.addonManager().addon("testfrontend", true);
    if (!tf) {
      check("bootstrap: testfrontend chargé", false, "addon introuvable");
      instance.exit();
      return;
    }
    auto uuid = tf->call<fcitx::ITestFrontend::createInputContext>("app");
    auto *ic = instance.inputContextManager().findByUUID(uuid);
    ic->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    ic->focusIn();
    instance.setCurrentInputMethod("predict");

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
    printf("\n%d test(s) en echec\n", g_failures);
    return 1;
  }
  printf("\ntous les tests passent\n");
  return 0;
}
