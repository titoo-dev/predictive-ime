// Harnais de test de l'engine fcitx5 « predict » : pilote le VRAI libpredict.so
// dans un fcitx::Instance headless (testfrontend/testim), avec un mock daemon
// in-process. Vérifie commits / candidats / preedit sans session graphique.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

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
  void setReformReply(std::vector<std::string> vars) {
    reformVars_ = std::move(vars);
  }
  std::string lastReformMode() const { return lastReformMode_; }
  int lastReformN() const { return lastReformN_; }
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
        } else if (req.contains("reformulate")) {
          lastReformMode_ = req.value("mode", "");
          lastReformN_ = req.value("n", 0);
          json resp;
          resp["variants"] = reformVars_;
          resp["source"] = "groq";
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
  std::vector<std::string> reformVars_;
  std::string lastReformMode_;
  int lastReformN_ = 0;
  std::string lastPrefix_;
  std::vector<std::string> lastContext_;
};

std::string g_tmpDir;

// DSL au-dessus du TestFrontend : pilote l'engine et inspecte son état.
struct Harness {
  fcitx::Instance *instance;
  fcitx::AddonInstance *tf;
  fcitx::ICUUID uuid;
  fcitx::InputContext *ic;
  MockDaemon *daemon;
  int bump = 0;

  void setConfig(const json &j) {
    std::string p = g_tmpDir + "/cfg/ime-predictord/config.json";
    FILE *f = ::fopen(p.c_str(), "w");
    std::string s = j.dump();
    ::fwrite(s.data(), 1, s.size(), f);
    ::fclose(f);
    bump += 2; // mtime distinct (engineCfg recharge sur mtime en secondes)
    timespec t[2];
    t[0].tv_sec = t[1].tv_sec = ::time(nullptr) + bump;
    t[0].tv_nsec = t[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, p.c_str(), t, 0);
  }
  // Recrée un InputContext neuf (état/contexte vierge) — isole les groupes de
  // tests (le contexte committé s'accumule sinon entre les tests). `program`
  // simule l'app cliente (sert au gating par programme, cf nextWordBarExclude).
  void resetWith(const std::string &program) {
    tf->call<fcitx::ITestFrontend::destroyInputContext>(uuid);
    uuid = tf->call<fcitx::ITestFrontend::createInputContext>(program);
    ic = instance->inputContextManager().findByUUID(uuid);
    ic->focusIn();
  }
  void reset() { resetWith("app"); }
  void setCaps(fcitx::CapabilityFlags caps) { ic->setCapabilityFlags(caps); }
  void setSurrounding(const std::string &text, unsigned cursor) {
    setSurrounding(text, cursor, cursor);
  }
  // anchor != cursor simule une SÉLECTION rapportée par l'app (souris)
  void setSurrounding(const std::string &text, unsigned cursor,
                      unsigned anchor) {
    ic->surroundingText().setText(text, cursor, anchor);
    ic->updateSurroundingText();
  }
  // Cache de texte environnant que le client n'a JAMAIS publié (pas
  // d'événement) : c'est l'état d'un terminal GTK4 comme ghostty, où le cache
  // de fcitx décrit une autre fenêtre. Éditer là-dessus tue le client.
  void setSurroundingStale(const std::string &text, unsigned cursor) {
    ic->surroundingText().setText(text, cursor, cursor);
  }
  void key(const char *sym) {
    tf->call<fcitx::ITestFrontend::keyEvent>(uuid, fcitx::Key(sym), false);
  }
  void type(const std::string &ascii) {
    for (char c : ascii) {
      std::string k(1, c);
      if (c == ' ')
        k = "space";
      key(k.c_str());
    }
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
  // Préedit du PANNEAU (pas envoyé à l'application) : c'est là que vit la
  // requête du picker emoji, et le marqueur de mode grille pour l'UI.
  std::string panelPreedit() {
    return ic->inputPanel().preedit().toStringForCommit();
  }
};

// --- Tests comportementaux (chacun pousse une expectation pour CHAQUE commit
//     que l'engine fait ; le testfrontend abort sur commit inattendu/manquant).

void compositionTests(Harness &h) {
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  // surrounding text VALIDE (pas seulement la capacité) : sinon l'engine bride
  // l'auto-application (autoApplyNeedsRevert) — cf engine.
  h.setSurrounding("", 0);

  // candidats affichés pour un préfixe
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.type("bonjou");
  check("composition: candidats affichés",
        h.candidates() == std::vector<std::string>{"bonjour"},
        h.candidates().empty() ? "vide" : h.candidates()[0]);
  h.expectCommit("bonjour "); // espace auto-applique 'bonjour'
  h.type(" ");

  // report de casse : 'Bonjou' → 'Bonjour ' (auto-apply + majuscule initiale)
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("Bonjour ");
  h.key("B");
  h.type("onjou");
  h.type(" ");
  check("composition: casse Bonjou→Bonjour", true);

  // literalIsWord : un vrai mot n'est pas remplacé
  h.daemon->setReply({"le", "les"}, "", /*literalIsWord=*/true);
  h.expectCommit("le ");
  h.type("le");
  h.type(" ");
  check("composition: literalIsWord 'le' gardé", true);
}

void backspaceTests(Harness &h) {
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});

  // Backspace en composition, buffer vidé → barre fermée
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.type("bo");
  h.key("BackSpace");
  h.key("BackSpace");
  check("backspace: buffer vidé → barre fermée", h.candidates().empty(),
        h.candidates().empty() ? "" : h.candidates()[0]);

  // RÉGRESSION : barre mot-suivant ouverte, Ctrl+Backspace (buffer vide) ferme
  h.daemon->setReply({"suivant", "autre"}, "", false);
  h.expectCommit("a ");
  h.type("a");
  h.type(" "); // commit → barre mot-suivant
  check("backspace: pré — barre mot-suivant ouverte", !h.candidates().empty());
  h.key("Control+BackSpace");
  check("backspace: Ctrl+Backspace ferme la barre (input vidé)",
        h.candidates().empty(), h.candidates().empty() ? "" : h.candidates()[0]);
}

void capabilityTests(Harness &h) {
  // (a) client SANS Preedit : commit correct + pas de crash
  h.setCaps(fcitx::CapabilityFlags{});
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("bonjour ");
  h.type("bonjour");
  h.type(" ");
  check("caps: sans Preedit, commit OK + pas de crash", true);

  // (b) client AVEC Preedit
  h.setCaps(fcitx::CapabilityFlag::Preedit);
  h.daemon->setReply({"merci"}, "merci", false);
  h.expectCommit("merci ");
  h.type("merci");
  h.type(" ");
  check("caps: avec Preedit, commit OK", true);

  // (c) champ Password : aucune prédiction, aucun preedit
  h.setCaps(fcitx::CapabilityFlag::Password);
  h.daemon->setReply({"secret"}, "secret", false);
  h.type("abc");
  check("password: pas de barre", h.candidates().empty());
  check("password: pas de preedit", h.preedit().empty(), h.preedit());
}

void interactionTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // déclencheur snippet ';mail' → expansion committée sur Espace (chemin trigger
  // — même code que l'emoji ':')
  h.daemon->setReply({"dev@x"}, "dev@x", false);
  h.expectCommit("dev@x ");
  h.type(";mail");
  h.type(" ");
  check("interaction: snippet ';mail' → expansion committée", true);

  // navigation : Tab surligne le 1er candidat, Espace le committe
  h.daemon->setReply({"alpha", "beta"}, "", false);
  h.expectCommit("alpha ");
  h.type("al");
  h.key("Tab");
  h.type(" ");
  check("interaction: Tab puis Espace committe le candidat surligné", true);

  // picker emoji : Super+; ouvre, Entrée SANS navigation prend le 1er candidat
  // (sans espace)
  h.daemon->setReply({"❤️", "💕"}, "❤️", false);
  h.expectCommit("❤️");
  h.key("Super+semicolon");
  h.type("coeur");
  h.key("Return");
  check("emoji: Super+; puis Entrée committe le 1er candidat", true);

  // ':' tapé n'ouvre PLUS le picker : caractère normal qui file à l'application
  h.daemon->setReply({"❤️"}, "", false);
  h.type(":");
  check("emoji: ':' tapé ne compose plus (pas de picker)", h.preedit().empty(),
        h.preedit());

  // Super+; EN PLEIN MOT : le fragment est committé tel quel, le picker s'ouvre
  h.daemon->setReply({"❤️", "💕"}, "", false);
  h.expectCommit("bonj");
  h.key("b"); h.key("o"); h.key("n"); h.key("j");
  h.key("Super+semicolon");
  check("emoji: Super+; en plein mot committe le fragment puis ouvre",
        h.panelPreedit() == ":", h.panelPreedit());
  h.key("Super+semicolon"); // re-presser referme le picker (rien de committé)
  check("emoji: Super+; referme le picker", h.panelPreedit().empty(),
        h.panelPreedit());

  // la RECHERCHE reste dans le panneau : l'application ne voit RIEN (avant, le
  // préedit client écrivait « :coeur » en plein champ de saisie).
  h.daemon->setReply({"❤️", "💕"}, "", false);
  h.key("Super+semicolon");
  h.type("coeur");
  check("emoji: la requête vit dans le préedit du panneau",
        h.panelPreedit() == ":coeur", h.panelPreedit());
  check("emoji: rien n'est écrit dans l'application", h.preedit().empty(),
        h.preedit());

  // recherche sans résultat : le picker RESTE ouvert (état vide côté UI), il ne
  // propose PAS le littéral ':zzz' en candidat.
  h.daemon->setReply({}, "", false);
  h.type("zz");
  check("emoji: aucun résultat → pas de candidat littéral",
        h.candidates().empty(), std::to_string(h.candidates().size()));
  check("emoji: aucun résultat → le picker reste ouvert",
        h.panelPreedit() == ":coeurzz", h.panelPreedit());

  // Échap dans le picker : ferme SANS committer le ':' synthétique ni la
  // requête (le testfrontend abort sur tout commit non attendu).
  h.key("Escape");
  check("emoji: Échap ferme le picker sans rien committer",
        h.panelPreedit().empty(), h.panelPreedit());

  // picker NU (aucune requête, grille de favoris) : Entrée prend le 1er emoji
  h.daemon->setReply({"😀", "❤️"}, "", false);
  h.expectCommit("😀");
  h.key("Super+semicolon");
  h.key("Return");
  check("emoji: Entrée sur le picker nu committe le 1er favori", true);

  // grille emoji : → entre en navigation sans Tab, ↓ saute une ligne (+8),
  // Entrée committe le surligné.
  h.daemon->setReply({"e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8",
                      "e9"}, "", false);
  h.expectCommit("e8");
  h.key("Super+semicolon");
  h.type("x");
  h.key("Right"); // entre dans la grille (index 0)
  h.key("Down");  // +8 → index 8
  h.key("Return");
  check("emoji: →/↓ naviguent la grille sans Tab, Entrée committe", true);

  // BORNAGE de la grille (10 candidats = 1 ligne pleine + 2) : ← sur la 1re
  // case ne reboucle pas sur la dernière, et ↓ depuis une ligne incomplète
  // tombe sur la DERNIÈRE case au lieu de repartir en haut.
  h.daemon->setReply({"e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8",
                      "e9"}, "", false);
  h.expectCommit("e9");
  h.key("Super+semicolon");
  h.type("x");
  h.key("Right"); // index 0
  h.key("Left");  // borné → reste 0
  h.key("Down");  // +8 → 8
  h.key("Down");  // +8 → 16, borné → 9 (dernier)
  h.key("Return");
  check("emoji: ←/↓ bornés aux extrémités (pas de wrap en grille)", true);

  // Début / Fin : extrémités directes de la grille
  h.expectCommit("e0");
  h.key("Super+semicolon");
  h.type("x");
  h.key("End");
  h.key("Home");
  h.key("Return");
  check("emoji: Fin puis Début sautent aux extrémités", true);

  // ponctuation : committe le mot (sans espace) ; le '.' file à l'application
  h.daemon->setReply({"fin"}, "", true);
  h.expectCommit("fin");
  h.type("fin");
  h.key("period");
  check("interaction: ponctuation committe le mot, '.' file à l'app", true);
}

void optInTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // frenchSpacing : fine insécable U+202F insérée avant '!'
  h.setConfig({{"frenchSpacing", true}});
  h.daemon->setReply({"bonjour"}, "", /*literalIsWord=*/true);
  h.expectCommit("bonjour");        // le mot
  h.expectCommit("\xE2\x80\xAF");    // U+202F, avant que l'app insère '!'
  h.type("bonjour");
  h.key("exclam");
  check("optin: frenchSpacing → U+202F avant '!'", true);

  // autoCapitalize : début de champ (IC neuf, contexte vide) → 1re lettre en maj
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);
  h.setConfig({{"autoCapitalize", true}});
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("Bonjour ");
  h.type("bonjour");
  h.type(" ");
  check("optin: autoCapitalize début de champ → Bonjour", true);

  h.setConfig(json::object()); // restaure les défauts
}

void revertTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // auto-application puis Backspace IMMÉDIAT = revert : restaure le littéral,
  // rouvre la composition, et le prochain Espace garde le littéral (veto).
  h.daemon->setReply({"bonjour"}, "bonjour", false);
  h.expectCommit("bonjour "); // 'bonjou' + Espace auto-applique
  h.type("bonjou");
  h.type(" ");
  h.key("BackSpace"); // fenêtre de revert
  check("revert: Backspace rouvre la composition sur le littéral 'bonjou'",
        h.preedit().rfind("bonjou", 0) == 0, h.preedit());
  h.expectCommit("bonjou "); // veto : le littéral est gardé cette fois
  h.type(" ");
  check("revert: après revert, Espace garde le littéral (veto)", true);
}

void recomposeTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // « je deman ␣ » puis Backspace (efface l'espace) : la composition doit se
  // ROUVRIR sur « deman » — barre re-proposée, contexte « je » conservé.
  h.daemon->setReply({"demande", "demander"}, "", false);
  h.expectCommit("je ");
  h.type("je");
  h.type(" ");
  h.expectCommit("deman ");
  h.type("deman");
  h.type(" ");
  // l'app contient « je deman » ; le harnais simule son SurroundingText
  h.setSurrounding("je deman ", 9);
  h.key("BackSpace");
  check("recompose: préedit rouvert sur 'deman'",
        h.preedit().rfind("deman", 0) == 0, h.preedit());
  check("recompose: candidats re-proposés", !h.candidates().empty(),
        h.candidates().empty() ? "vide" : h.candidates()[0]);
  bool hasJe = false, hasDeman = false;
  std::string dump;
  for (auto &w : h.daemon->lastContext()) {
    if (w == "je")
      hasJe = true;
    if (w == "deman")
      hasDeman = true;
    dump += w + " ";
  }
  check("recompose: contexte 'je' conservé (sans 'deman' résiduel)",
        hasJe && !hasDeman, dump);
  h.expectCommit("deman"); // Échap referme proprement (committe le littéral)
  h.key("Escape");

  // Backspace au MILIEU d'un mot (« dem|an ») : PAS de recomposition — la
  // touche file à l'app (recomposer la moitié gauche corromprait le texte).
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("deman", 3);
  h.key("BackSpace");
  check("recompose: pas de recomposition en milieu de mot",
        h.preedit().empty(), h.preedit());

  // Sans SurroundingText : comportement inchangé (la touche file à l'app).
  h.reset();
  h.setCaps(fcitx::CapabilityFlag::Preedit);
  h.key("BackSpace");
  check("recompose: sans SurroundingText, Backspace file à l'app",
        h.preedit().empty(), h.preedit());

  // RÉGRESSION « ghostty meurt en tapant puis effaçant » : la capacité est
  // annoncée et le cache de fcitx contient du texte, mais CE client ne l'a
  // jamais publié (terminal GTK4 : aucun tampon en face). Toute suppression
  // le fait déréférencer NULL et le TUE → on ne recompose pas.
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurroundingStale("bonjour ", 8);
  h.key("BackSpace");
  check("surrounding jamais publié : aucune édition du texte écrit",
        h.preedit().empty(), h.preedit());

  // …et dès que le client publie vraiment, la recomposition revient.
  h.daemon->setReply({"bonjour"}, "", false);
  h.setSurrounding("bonjour ", 8);
  h.key("BackSpace");
  check("surrounding publié : la recomposition marche de nouveau",
        h.preedit() == "bonjour", h.preedit());
}

void multiWordTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // candidat multi-mots (« sais pas ») proposé en barre mot-suivant, sélectionné
  // au Tab+Espace → committé en entier (l'engine apprend mot à mot en interne).
  h.daemon->setReply({"sais pas", "autre"}, "", false);
  h.expectCommit("je ");
  h.type("je");
  h.type(" "); // commit + barre mot-suivant avec 'sais pas'
  h.expectCommit("sais pas ");
  h.key("Tab"); // surligne 'sais pas'
  h.type(" ");  // committe le candidat surligné
  check("multi-mots: 'sais pas' committé en entier", true);
}

void englishTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // casse anglaise : le candidat « i'm » s'affiche ET se committe « I'm »
  h.daemon->setReply({"i'm"}, "", false);
  h.type("im");
  check("anglais: candidat i'm affiché I'm",
        !h.candidates().empty() && h.candidates()[0] == "I'm",
        h.candidates().empty() ? "vide" : h.candidates()[0]);
  h.expectCommit("I'm ");
  h.key("Tab");
  h.type(" ");
  check("anglais: I'm committé avec majuscule", true);

  // panneau de langue Ctrl+Shift+L : chips [Français|English|Auto|Libre],
  // chiffre/Entrée applique (réécrit "lang" dans config.json, formatage
  // préservé), Échap annule.
  auto readCfg = [] {
    std::ifstream f(g_tmpDir + "/cfg/ime-predictord/config.json");
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
  };
  h.setConfig({{"lang", "fr"}});
  h.key("Control+Shift+L");
  auto chips = h.candidates();
  check("langue: le panneau montre les chips",
        chips.size() == 4 && chips[0] == "Français" && chips[1] == "English",
        chips.empty() ? "vide" : chips[0] + "," + chips[1]);
  h.key("2"); // English
  check("langue: '2' applique lang=en",
        readCfg().find("\"lang\":\"en\"") != std::string::npos, readCfg());
  check("langue: le panneau est fermé après le choix", h.candidates().empty() ||
        h.candidates()[0] != "Français");
  h.key("Control+Shift+L");
  h.key("Escape");
  check("langue: Échap n'écrit rien (reste en)",
        readCfg().find("\"lang\":\"en\"") != std::string::npos, readCfg());
  h.key("Control+Shift+L"); // s'ouvre sur English (langue courante)
  h.key("Right");           // → Auto
  h.key("Return");
  check("langue: →/Entrée applique le choix surligné (auto)",
        readCfg().find("\"lang\":\"auto\"") != std::string::npos, readCfg());
  // AZERTY : la rangée de chiffres NON shiftée envoie &é"'(… — le panneau
  // doit l'accepter comme 1-9 (sinon : fermeture SILENCIEUSE, rien appliqué —
  // « je bascule en anglais et rien ne change »).
  h.setConfig({{"lang", "fr"}});
  h.key("Control+Shift+L");
  h.key("eacute"); // touche « 2 » AZERTY sans Shift → English
  check("langue: AZERTY 'é' (rangée 2) applique lang=en",
        readCfg().find("\"lang\":\"en\"") != std::string::npos, readCfg());
  h.setConfig(json::object()); // restaure les défauts
}

void surroundingContextTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  // le texte AVANT le curseur sert de contexte envoyé au daemon
  h.setSurrounding("je suis ", 8);
  h.daemon->setReply({"content"}, "", false);
  h.type("co");
  auto ctx = h.daemon->lastContext();
  bool hasJe = false, hasSuis = false;
  std::string dump;
  for (auto &w : ctx) {
    if (w == "je")
      hasJe = true;
    if (w == "suis")
      hasSuis = true;
    dump += w + " ";
  }
  check("contexte: le SurroundingText 'je suis' est envoyé au daemon",
        hasJe && hasSuis, dump);
}

void nextWordBarTests(Harness &h) {
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);

  // nextWordBar=false : pas de barre spéculative après commit (mode calme — la
  // piste de mitigation Ghostty). La barre de COMPLÉTION pendant la frappe reste.
  h.setConfig({{"nextWordBar", false}});
  h.daemon->setReply({"jour"}, "", false);
  h.expectCommit("je ");
  h.type("je");
  h.type(" "); // commit → PAS de barre mot-suivant attendue
  check("nextWordBar=false: aucune barre spéculative après commit",
        h.candidates().empty(), h.candidates().empty() ? "" : h.candidates()[0]);
  h.setConfig(json::object()); // restaure
}

void ghosttyTests(Harness &h) {
  // Mitigation suivi-curseur terminal : la barre spéculative mot-suivant (sans
  // preedit pour l'ancrer) est supprimée pour les programmes de nextWordBarExclude
  // (motif sous-chaîne sur ic->program()). La complétion pendant la frappe reste.
  h.setConfig({{"nextWordBarExclude", json::array({"ghostty"})}});

  // (a) dans Ghostty (programme matché) → pas de barre spéculative après commit
  h.resetWith("com.mitchellh.ghostty");
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);
  h.daemon->setReply({"jour", "suis"}, "", false);
  h.expectCommit("je ");
  h.type("je");
  h.type(" ");
  check("ghostty: nextWordBarExclude → pas de barre spéculative",
        h.candidates().empty(), h.candidates().empty() ? "" : h.candidates()[0]);

  // (b) hors terminal (programme NON matché) → la barre spéculative s'affiche
  h.resetWith("org.kde.kwrite");
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});
  h.setSurrounding("", 0);
  h.daemon->setReply({"jour", "suis"}, "", false);
  h.expectCommit("je ");
  h.type("je");
  h.type(" ");
  check("ghostty: hors terminal → barre spéculative présente",
        !h.candidates().empty(),
        h.candidates().empty() ? "vide" : h.candidates()[0]);

  h.setConfig(json::object());
}

// Reformulation — asynchrone (worker + eventDispatcher) : les asserts vivent
// dans des timers chaînés, la boucle d'événements tourne entre chaque étape.
// L1 : Ctrl+Alt+R SANS sélection dans un champ court reformule tout le champ ;
// le commit doit alors REMPLACER le champ (delete explicite) — commitString
// seul INSÉRAIT la variante en plus du texte.
void reformTests(Harness h, fcitx::Instance *instance) {
  static std::unique_ptr<fcitx::EventSourceTime> t1, t2, t3, t4, t5;
  h.reset();
  h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                   fcitx::CapabilityFlag::SurroundingText});

  // FEEDBACK : Ctrl+Alt+R sans rien à reformuler → panneau compact (pas un
  // no-op silencieux) ; n'importe quelle touche le ferme.
  h.setSurrounding("", 0);
  h.key("Control+Alt+R");
  check("reform: sans texte → panneau « Rien à reformuler »",
        !h.candidates().empty() &&
            h.candidates()[0].find("Rien à reformuler") != std::string::npos,
        h.candidates().empty() ? "vide" : h.candidates()[0]);
  h.key("Escape");
  check("reform: Échap ferme le panneau « rien à reformuler »",
        h.candidates().empty(),
        h.candidates().empty() ? "" : h.candidates()[0]);

  h.setSurrounding("bonjour le monde", 16); // curseur en fin, AUCUNE sélection
  h.daemon->setReformReply({"salut le monde", "coucou le monde"});
  h.key("Control+Alt+R");
  check("reform: panneau ouvert (spinner)", !h.candidates().empty(),
        h.candidates().empty() ? "vide" : h.candidates()[0]);
  t1 = instance->eventLoop().addTimeEvent(
      CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
      [h, instance](fcitx::EventSourceTime *, uint64_t) mutable {
        check("reform: variantes reçues",
              h.candidates().size() == 2 &&
                  h.candidates()[0] == "salut le monde",
              h.candidates().empty() ? "vide" : h.candidates()[0]);
        h.expectCommit("salut le monde");
        h.key("1");
        check("reform L1: sans sélection, le champ est remplacé",
              h.ic->surroundingText().text().empty(),
              "reste: '" + h.ic->surroundingText().text() + "'");

        // cas SÉLECTION rapportée : commit-over-selection, PAS de delete
        h.reset();
        h.setCaps(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit,
                                         fcitx::CapabilityFlag::SurroundingText});
        h.setSurrounding("bonjour le monde", 16, 8); // « le monde » sélectionné
        h.daemon->setReformReply({"la planète"});
        h.key("Control+Alt+R");
        t2 = instance->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
            [h, instance](fcitx::EventSourceTime *, uint64_t) mutable {
              check("reform: variantes (sélection)",
                    h.candidates().size() == 1 &&
                        h.candidates()[0] == "la planète",
                    h.candidates().empty() ? "vide" : h.candidates()[0]);
              h.expectCommit("la planète");
              h.key("ampersand"); // AZERTY : « 1 » sans Shift = '&'
              check("reform: avec sélection, pas de delete du champ",
                    h.ic->surroundingText().text() == "bonjour le monde",
                    h.ic->surroundingText().text());

              // — raccourci de mode (f = Formel), mémoire du dernier mode,
              //   et reformCount (config) envoyé au daemon —
              h.reset();
              h.setCaps(fcitx::CapabilityFlags{
                  fcitx::CapabilityFlag::Preedit,
                  fcitx::CapabilityFlag::SurroundingText});
              h.setConfig({{"reformCount", 2}});
              h.setSurrounding("une phrase a reformuler", 23);
              h.daemon->setReformReply({"variante A", "variante B"});
              h.key("Control+Alt+R");
              t3 = instance->eventLoop().addTimeEvent(
                  CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
                  [h, instance](fcitx::EventSourceTime *, uint64_t) mutable {
                    check("reform: reformCount=2 envoyé au daemon",
                          h.daemon->lastReformN() == 2,
                          std::to_string(h.daemon->lastReformN()));
                    check("reform: mode initial rephrase",
                          h.daemon->lastReformMode() == "rephrase",
                          h.daemon->lastReformMode());
                    h.key("f"); // raccourci direct → mode Formel, régénère
                    t4 = instance->eventLoop().addTimeEvent(
                        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 500000,
                        0,
                        [h, instance](fcitx::EventSourceTime *,
                                      uint64_t) mutable {
                          check("reform: 'f' saute au mode formal",
                                h.daemon->lastReformMode() == "formal",
                                h.daemon->lastReformMode());
                          h.key("Escape"); // sort sans committer
                          // MÉMOIRE : le prochain Ctrl+Alt+R repart en Formel
                          h.setSurrounding("encore un autre texte", 21);
                          h.key("Control+Alt+R");
                          t5 = instance->eventLoop().addTimeEvent(
                              CLOCK_MONOTONIC,
                              fcitx::now(CLOCK_MONOTONIC) + 500000, 0,
                              [h, instance](fcitx::EventSourceTime *,
                                            uint64_t) mutable {
                                check("reform: dernier mode mémorisé (formal)",
                                      h.daemon->lastReformMode() == "formal",
                                      h.daemon->lastReformMode());
                                h.key("Escape");
                                h.setConfig(json::object());
                                instance->exit();
                                return true;
                              });
                          return true;
                        });
                    return true;
                  });
              return true;
            });
        return true;
      });
}

} // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  ::setvbuf(stdout, nullptr, _IONBF, 0); // non bufferisé : visible même sur abort

  char tmpl[] = "/tmp/ime-engine-test-XXXXXX";
  g_tmpDir = ::mkdtemp(tmpl);
  std::string sockPath = g_tmpDir + "/predictord.sock";
  ::setenv("IME_PREDICTORD_SOCK", sockPath.c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (g_tmpDir + "/cfg").c_str(), 1);
  ::mkdir((g_tmpDir + "/cfg").c_str(), 0755);
  ::mkdir((g_tmpDir + "/cfg/ime-predictord").c_str(), 0755);

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
    ic->focusIn();
    instance.setCurrentInputMethod("predict");

    Harness h{&instance, tf, uuid, ic, &daemon};
    compositionTests(h);
    backspaceTests(h);
    capabilityTests(h);
    interactionTests(h);
    optInTests(h);
    revertTests(h);
    recomposeTests(h);
    multiWordTests(h);
    englishTests(h);
    surroundingContextTests(h);
    nextWordBarTests(h);
    ghosttyTests(h);
    reformTests(h, &instance); // async — appelle instance.exit() à la fin
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
