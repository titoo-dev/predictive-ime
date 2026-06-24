// Track A — engine fcitx5 (InputMethodEngineV2).
//
// Glue mince (patron fcitx5-azookeyd) : parle au daemon de prédiction sur socket
// Unix et affiche les candidats via l'API CandidateList de fcitx5.
//
// Modèle de saisie v2 — robuste, calqué sur Gboard/clavier Windows :
//   - capture TOUT caractère de mot Unicode (accents, MAJUSCULES, apostrophe et
//     trait d'union en milieu de mot, chiffres en milieu de mot) — plus de
//     préédition corrompue dès qu'on écrit du français.
//   - les suggestions sont OPT-IN : tant qu'on ne navigue pas (Tab), les
//     chiffres, flèches, Entrée et la ponctuation passent NORMALEMENT à l'appli.
//     Plus de candidat avalé à la place d'un retour-ligne / d'un nombre.
//   - Espace COMPLÈTE un fragment ou AUTOCORRIGE une faute, mais n'écrase jamais
//     un mot déjà valide (le daemon renvoie `literalIsWord`).
//   - report de CASSE (Bonjou → Bonjour), contexte sur 2 mots + amorçage depuis
//     le texte environnant de l'application (surrounding text).
//
// Sélection : Tab / ⇧Tab navigue, Espace ou Entrée valide le candidat surligné.
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace {

// ------------------------------------------------------ config utilisateur --
// $XDG_CONFIG_HOME/ime-predictord/config.json (partagé avec le daemon),
// rechargé à chaud sur mtime. Côté engine :
//   ghostText            — le reste du mot auto-appliqué s'affiche dans le préedit
//   frenchSpacing        — espace fine insécable (U+202F) avant ; : ! ?
//   autoCapitalize       — majuscule automatique en début de phrase
//   nextWordBar          — barre mot-suivant après Espace/commit (false = calme)
//   autoApplyNeedsRevert — n'auto-applique que si l'app permet le revert
//                          Backspace (SurroundingText) ; sinon Tab choisit
//   escapeForward        — Échap ferme la barre PUIS atteint l'application
//                          (vim sort du mode insertion) ; false = avalé
struct EngineCfg {
  bool ghostText = true;
  bool frenchSpacing = false;
  bool autoCapitalize = false;
  bool nextWordBar = true;
  bool autoApplyNeedsRevert = true;
  bool escapeForward = true;
  // Timeout socket (ms). La complétion intra-mot (n-gram, <1 ms) garde
  // socketTimeoutMs → jamais de gel clavier. Le MOT-SUIVANT peut être neural
  // (~120-200 ms) : nextWordTimeoutMs le borne SÉPARÉMENT (à relever, ex. 300,
  // quand neural est activé ; au prix d'un léger hitch après Espace).
  int socketTimeoutMs = 150;
  int nextWordTimeoutMs = 150;
  // Programmes (sous-chaînes de ic->program(), ex. "ghostty") où la barre
  // SPÉCULATIVE mot-suivant est supprimée : dans un terminal elle n'a pas de
  // preedit pour s'ancrer au curseur et « traîne » derrière lui. La complétion
  // pendant la frappe (ancrée au preedit) reste, elle.
  std::vector<std::string> nextWordBarExclude;
};

const EngineCfg &engineCfg() {
  static EngineCfg cfg;
  static time_t stamp = -1;
  static const std::string path = [] {
    const char *x = ::getenv("XDG_CONFIG_HOME");
    const char *h = ::getenv("HOME");
    return (x ? std::string(x)
              : std::string(h ? h : "/tmp") + "/.config") +
           "/ime-predictord/config.json";
  }();
  struct stat st {};
  time_t t = ::stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
  if (t != stamp) {
    stamp = t;
    EngineCfg fresh;
    std::ifstream f(path);
    if (f) {
      try {
        json j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
        fresh.ghostText = j.value("ghostText", fresh.ghostText);
        fresh.frenchSpacing = j.value("frenchSpacing", fresh.frenchSpacing);
        fresh.autoCapitalize = j.value("autoCapitalize", fresh.autoCapitalize);
        fresh.nextWordBar = j.value("nextWordBar", fresh.nextWordBar);
        fresh.autoApplyNeedsRevert =
            j.value("autoApplyNeedsRevert", fresh.autoApplyNeedsRevert);
        fresh.escapeForward = j.value("escapeForward", fresh.escapeForward);
        fresh.socketTimeoutMs = j.value("socketTimeoutMs", fresh.socketTimeoutMs);
        fresh.nextWordTimeoutMs =
            j.value("nextWordTimeoutMs", fresh.nextWordTimeoutMs);
        for (const auto &e :
             j.value("nextWordBarExclude", nlohmann::json::array()))
          if (e.is_string())
            fresh.nextWordBarExclude.push_back(e.get<std::string>());
      } catch (...) {
      }
    }
    cfg = fresh;
  }
  return cfg;
}

// ----------------------------------------------------------------- UTF-8 ----
void appendCp(std::string &s, uint32_t cp) {
  if (cp < 0x80)
    s.push_back(char(cp));
  else if (cp < 0x800) {
    s.push_back(char(0xC0 | (cp >> 6)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(char(0xE0 | (cp >> 12)));
    s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(char(0xF0 | (cp >> 18)));
    s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  }
}

std::vector<uint32_t> decodeUtf8(const std::string &s) {
  std::vector<uint32_t> cps;
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0xF;
      len = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x7;
      len = 4;
    } else {
      i++;
      continue;
    }
    if (i + len > n)
      break;
    for (int k = 1; k < len; k++)
      cp = (cp << 6) | (s[i + k] & 0x3F);
    cps.push_back(cp);
    i += len;
  }
  return cps;
}

// Retire le dernier point de code UTF-8 (pour Backspace).
void popLastCp(std::string &s) {
  if (s.empty())
    return;
  size_t i = s.size();
  do {
    --i;
  } while (i > 0 && (uint8_t(s[i]) & 0xC0) == 0x80);
  s.erase(i);
}

// --------------------------------------------------------------- casse -------
bool isUpperCp(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7);
}
bool isLowerCp(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7);
}
bool isLetterCp(uint32_t cp) {
  return isUpperCp(cp) || isLowerCp(cp) || (cp >= 0x100 && cp <= 0x24F);
}
uint32_t toUpperCp(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z')
    return cp - 32;
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
    return cp - 0x20;
  return cp;
}

// Caractère qui prolonge un mot : lettre toujours ; apostrophe / trait d'union /
// chiffre seulement si le buffer est déjà entamé (mot en cours). ':' sur buffer
// VIDE démarre le picker emoji (":coeur" → ❤️), ';' un SNIPPET (";mail" →
// expansion) — jamais en milieu de mot, donc "10:30" ou "voici :" tapent
// normalement.
bool isWordExtender(uint32_t cp, bool bufferEmpty) {
  if (isLetterCp(cp))
    return true;
  if (bufferEmpty)
    return cp == ':' || cp == ';';
  return cp == '\'' || cp == 0x2019 || cp == '-' || (cp >= '0' && cp <= '9');
}

// Buffer « déclencheur » (emoji ':' ou snippet ';') : pas d'apprentissage de
// bigramme ni de contexte — ce n'est pas de la prose.
bool isTriggerBuffer(const std::string &buffer) {
  return !buffer.empty() && (buffer[0] == ':' || buffer[0] == ';');
}

// Majuscule sur la première lettre (auto-capitalisation en début de phrase).
std::string capFirst(const std::string &w);

std::string capFirst(const std::string &w) {
  auto cps = decodeUtf8(w);
  std::string out;
  bool done = false;
  for (uint32_t cp : cps) {
    if (!done && isLetterCp(cp)) {
      appendCp(out, toUpperCp(cp));
      done = true;
    } else {
      appendCp(out, cp);
    }
  }
  return out;
}

// Reporte la casse du `buffer` tapé sur un candidat (minuscule du modèle).
// "Bonjou" → "Bonjour" ; "FRAN" → "FRANÇAIS" ; "le" → inchangé.
std::string applyCase(const std::string &cand, const std::string &buffer) {
  auto bcps = decodeUtf8(buffer);
  bool firstUpper = false, allUpper = true;
  int letters = 0;
  for (uint32_t cp : bcps) {
    if (!isLetterCp(cp))
      continue;
    if (letters == 0)
      firstUpper = isUpperCp(cp);
    if (!isUpperCp(cp))
      allUpper = false;
    letters++;
  }
  if (letters == 0)
    return cand;
  auto ccps = decodeUtf8(cand);
  std::string out;
  if (allUpper && letters >= 2) {
    for (uint32_t cp : ccps)
      appendCp(out, toUpperCp(cp));
  } else if (firstUpper) {
    bool done = false;
    for (uint32_t cp : ccps) {
      if (!done && isLetterCp(cp)) {
        appendCp(out, toUpperCp(cp));
        done = true;
      } else
        appendCp(out, cp);
    }
  } else {
    return cand;
  }
  return out;
}

// Ponctuation « haute » qui prend une fine insécable (U+202F) AVANT, en
// typographie française : point-virgule, deux-points, point d'exclamation,
// point d'interrogation et guillemet fermant « » » (U+00BB).
bool needsFrenchThinBefore(uint32_t cp) {
  return cp == ';' || cp == ':' || cp == '!' || cp == '?' || cp == 0x00BB;
}

// Les `maxWords` derniers mots d'un texte (pour amorcer le contexte depuis le
// texte environnant de l'application). S'ARRÊTE aux fins de phrase « . ! ? » :
// le contexte ne traverse jamais une frontière de phrase.
std::vector<std::string> lastWords(const std::vector<uint32_t> &cps,
                                   int maxWords) {
  std::vector<std::string> out;
  size_t i = cps.size();
  while (i > 0 && (int)out.size() < maxWords) {
    while (i > 0 && !isLetterCp(cps[i - 1])) {
      uint32_t c = cps[i - 1];
      if (c == '.' || c == '!' || c == '?')
        return out; // frontière de phrase
      --i; // saute les non-lettres
    }
    size_t end = i;
    while (i > 0 && (isLetterCp(cps[i - 1]) || cps[i - 1] == '\'' ||
                     cps[i - 1] == 0x2019 || cps[i - 1] == '-'))
      --i;
    if (end > i) {
      std::string w;
      for (size_t j = i; j < end; j++)
        appendCp(w, cps[j]);
      out.push_back(w);
    }
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// ----------------------------------------------------------- daemon IPC -----
struct DaemonReply {
  std::vector<std::string> candidates;
  bool literalIsWord = false;
  std::string autocomplete; // mot à appliquer sur Espace (haute confiance), ou ""
};

// Connexion au daemon BORNÉE dans le temps : on tourne sur le thread principal
// de fcitx, qui tient tout le clavier de la session — un daemon coincé ne doit
// JAMAIS geler la frappe. connect() non-bloquant (un backlog plein = daemon
// suspendu → échec immédiat, pas d'attente), puis timeouts d'E/S : au pire la
// frappe continue sans candidats.
constexpr int kDaemonTimeoutMs = 150;

int connectDaemon(int timeoutMs = kDaemonTimeoutMs) {
  const char *envSock = ::getenv("IME_PREDICTORD_SOCK");
  std::string path = envSock ? envSock : "/tmp/ime-predictord.sock";
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
    ::close(fd); // EAGAIN (backlog plein) compris : dégradation gracieuse
    return -1;
  }
  int fl = ::fcntl(fd, F_GETFL);
  if (fl >= 0)
    ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

DaemonReply queryDaemon(const std::vector<std::string> &context,
                        const std::string &prefix) {
  DaemonReply out;
  // Mot-suivant (prefix vide) = peut être neural → budget séparé, relevable.
  // Complétion intra-mot (prefix non vide) = n-gram rapide → timeout court, le
  // clavier ne gèle jamais derrière le daemon.
  int timeoutMs = prefix.empty() ? engineCfg().nextWordTimeoutMs
                                  : engineCfg().socketTimeoutMs;
  int fd = connectDaemon(timeoutMs);
  if (fd < 0)
    return out;
  json req;
  req["context"] = context;
  req["prefix"] = prefix;
  std::string line = req.dump() + "\n";
  if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
    ::close(fd);
    return out;
  }
  std::string buf;
  char tmp[4096];
  ssize_t n;
  while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) {
    buf.append(tmp, n);
    if (!buf.empty() && buf.back() == '\n')
      break;
  }
  ::close(fd);
  try {
    json resp = json::parse(buf);
    for (auto &c : resp.value("candidates", json::array()))
      out.candidates.push_back(c.get<std::string>());
    out.literalIsWord = resp.value("literalIsWord", false);
    out.autocomplete = resp.value("autocomplete", std::string{});
  } catch (...) {
  }
  return out;
}

void learnDaemon(const std::string &prev, const std::string &word) {
  int fd = connectDaemon();
  if (fd < 0)
    return;
  json req;
  req["learn"]["prev"] = prev;
  req["learn"]["word"] = word;
  std::string line = req.dump() + "\n";
  ssize_t w = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
  (void)w;
  ::close(fd);
}

// L'utilisateur a reverté un remplacement (Backspace) : le daemon ne doit plus
// jamais auto-appliquer cette paire (persisté côté daemon).
void vetoDaemon(const std::string &typed, const std::string &applied) {
  int fd = connectDaemon();
  if (fd < 0)
    return;
  json req;
  req["veto"]["typed"] = typed;
  req["veto"]["applied"] = applied;
  std::string line = req.dump() + "\n";
  ssize_t w = ::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
  (void)w;
  ::close(fd);
}

// Reformulation : on envoie la phrase sélectionnée, le daemon GÉNÈRE (plusieurs
// secondes) 3 variantes. Timeout long (≠ prédiction par-frappe) — c'est une
// action explicite, l'utilisateur attend. (Bloquant pour l'instant ; async = TODO.)
std::vector<std::string> reformulateDaemon(const std::string &sentence) {
  std::vector<std::string> out;
  int fd = connectDaemon(/*timeoutMs=*/12000);
  if (fd < 0)
    return out;
  json req;
  req["reformulate"] = sentence;
  req["n"] = 3;
  std::string line = req.dump() + "\n";
  if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
    ::close(fd);
    return out;
  }
  std::string buf;
  char tmp[8192];
  ssize_t n;
  while ((n = ::read(fd, tmp, sizeof(tmp))) > 0) {
    buf.append(tmp, n);
    if (!buf.empty() && buf.back() == '\n')
      break;
  }
  ::close(fd);
  try {
    json resp = json::parse(buf);
    for (auto &v : resp.value("variants", json::array()))
      out.push_back(v.get<std::string>());
  } catch (...) {
  }
  return out;
}

// État par contexte d'entrée.
struct PredictState : public fcitx::InputContextProperty {
  std::string buffer;                // mot en cours (préfixe, UTF-8)
  std::vector<std::string> ctx;      // jusqu'à 2 derniers mots committés
  std::vector<std::string> cands;    // candidats courants (pour preedit/commit)
  int navIndex = 0;                  // candidat surligné quand on navigue
  bool navigating = false;           // l'utilisateur a commencé à choisir (Tab)
  bool literalIsWord = false;        // le préfixe tapé est-il déjà un vrai mot ?
  std::string autocomplete;          // mot appliqué sur Espace (haute confiance)
  // Fenêtre de REVERT d'une auto-application (Backspace juste après) :
  std::string lastAutoLit;           // le littéral qui a été remplacé
  std::string lastAutoWord;          // le mot qui avait été appliqué
  uint32_t lastAutoCps = 0;          // points de code committés à effacer
  bool vetoAuto = false;             // l'utilisateur a refusé : Espace garde le littéral
  bool reformulating = false;        // mode reformulation : cands = variantes de la sélection
};

class PredictCandidate : public fcitx::CandidateWord {
public:
  // `autoApply` : ce candidat sera appliqué par l'Espace — marqué en GRAS dans
  // le Text fcitx, l'UI (qmlpanel) le rend distinctement pour que
  // l'utilisateur sache toujours ce que l'Espace va faire.
  explicit PredictCandidate(std::string text, bool autoApply = false)
      : text_(std::move(text)) {
    fcitx::Text t;
    t.append(text_, autoApply ? fcitx::TextFormatFlags{fcitx::TextFormatFlag::Bold}
                              : fcitx::TextFormatFlags{});
    setText(std::move(t));
  }
  void select(fcitx::InputContext *) const override {} // sélection gérée à part
  const std::string &word() const { return text_; }

private:
  std::string text_;
};

// Liste de candidats MAISON : CommonCandidateList ABORT fcitx (FatalLog
// « invalid label idx ») au-delà de 10 candidats (ses labels « 1. »… « 0. »
// sont fixes) — la grille emoji en montre 24. Pas de pagination, pas de
// labels, un curseur : exactement ce que notre UI consomme.
class PredictCandidateList : public fcitx::CandidateList {
public:
  void append(std::string text, bool autoApply) {
    cands_.push_back(
        std::make_unique<PredictCandidate>(std::move(text), autoApply));
  }
  void setCursorIndex(int i) { cursor_ = i; }

  const fcitx::Text &label(int) const override { return emptyLabel_; }
  const fcitx::CandidateWord &candidate(int idx) const override {
    return *cands_[idx];
  }
  int size() const override { return (int)cands_.size(); }
  int cursorIndex() const override { return cursor_; }
  fcitx::CandidateLayoutHint layoutHint() const override {
    return fcitx::CandidateLayoutHint::Horizontal;
  }

private:
  std::vector<std::unique_ptr<PredictCandidate>> cands_;
  fcitx::Text emptyLabel_;
  int cursor_ = -1;
};

} // namespace

class PredictEngine : public fcitx::InputMethodEngineV2 {
public:
  PredictEngine(fcitx::Instance *instance)
      : instance_(instance),
        factory_([](fcitx::InputContext &) { return new PredictState; }) {
    instance_->inputContextManager().registerProperty("predictState",
                                                       &factory_);
  }

  void keyEvent(const fcitx::InputMethodEntry &,
                fcitx::KeyEvent &event) override {
    if (event.isRelease())
      return;
    // Touche modificatrice SEULE (Shift, Ctrl, Alt…) : on ne touche à RIEN.
    // Sans cette garde, l'appui Shift en plein mot committait le buffer
    // (« jean-P » → « jean- » committé), faisait clignoter la barre à chaque
    // majuscule (clearPanel puis ré-apparition), et désarmait la fenêtre de
    // revert Backspace (consommée plus bas par n'importe quelle touche).
    if (event.key().isModifier())
      return;
    auto *ic = event.inputContext();
    // Champs mot de passe / sensibles : aucune prédiction, aucune préédition —
    // on laisse tout passer (vie privée + comportement attendu d'un clavier).
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
        ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive))
      return;
    auto *state = ic->propertyFor(&factory_);
    const auto &key = event.key();
    auto sym = key.sym();
    auto states = key.states();
    bool mod = states.test(fcitx::KeyState::Ctrl) ||
               states.test(fcitx::KeyState::Alt) ||
               states.test(fcitx::KeyState::Super);
    uint32_t cp = fcitx::Key::keySymToUnicode(sym);
    if (::getenv("IME_DEBUG"))
      fprintf(stderr, "[predict] sym=0x%x cp=0x%x buf='%s' nav=%d cands=%zu reform=%d\n",
              sym, cp, state->buffer.c_str(), int(state->navigating),
              state->cands.size(), int(state->reformulating));

    // (R) MODE REFORMULATION : la barre montre 3 variantes de la sélection ;
    // 1-3 ou Tab/flèches+Entrée REMPLACE la sélection, Échap annule. Toute autre
    // touche quitte le mode et retombe dans la saisie normale.
    if (state->reformulating) {
      int n = (int)state->cands.size();
      if (sym == FcitxKey_Escape) { exitReformulation(ic, state); return; }
      if (!mod && cp >= '1' && cp <= '9' && int(cp - '1') < n) {
        commitReformulation(ic, state, int(cp - '1'));
        return;
      }
      if (!mod && (sym == FcitxKey_Tab || sym == FcitxKey_Right) && n) {
        state->navIndex = (state->navIndex + 1) % n;
        setReformulationCandidates(ic, state);
        return;
      }
      if (!mod && (sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Left) && n) {
        state->navIndex = (state->navIndex - 1 + n) % n;
        setReformulationCandidates(ic, state);
        return;
      }
      if (!mod && (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)) {
        commitReformulation(ic, state, state->navIndex);
        return;
      }
      exitReformulation(ic, state); // autre touche → on sort et on continue
    }

    // (R-trig) DÉCLENCHEUR : Ctrl+Alt+R sur une SÉLECTION → 3 reformulations.
    if (states.test(fcitx::KeyState::Ctrl) &&
        states.test(fcitx::KeyState::Alt) && sym == FcitxKey_r) {
      enterReformulation(ic, state); // si pas de sélection/variantes : no-op
      return;                        // on consomme le raccourci dans tous les cas
    }

    // (0) Fenêtre de REVERT : Backspace IMMÉDIATEMENT après une
    // auto-application efface le mot appliqué, restaure le littéral tapé et
    // ré-ouvre la composition — et le prochain Espace gardera le littéral
    // (vetoAuto). C'est l'« undo » de l'autocorrection, façon Gboard.
    uint32_t autoCps = state->lastAutoCps;
    state->lastAutoCps = 0; // la fenêtre ne dure qu'une touche
    if (sym == FcitxKey_BackSpace && !mod && state->buffer.empty() &&
        autoCps > 0 &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      ic->deleteSurroundingText(-int(autoCps), autoCps);
      state->buffer = state->lastAutoLit;
      state->vetoAuto = true;
      // veto PERSISTANT : cette paire tapé→appliqué ne sera plus jamais
      // auto-appliquée (le daemon la journalise).
      vetoDaemon(state->lastAutoLit, state->lastAutoWord);
      if (!state->ctx.empty())
        state->ctx.pop_back(); // le mot remplacé n'est plus dans le texte
      state->navigating = false;
      updateCompletion(ic, state);
      event.filterAndAccept();
      return;
    }

    // (1) Caractère de mot (sans Ctrl/Alt/Super) → prolonge le buffer.
    //     Exception : en NAVIGATION, les chiffres 1-6 sélectionnent
    //     directement le candidat correspondant.
    if (!mod && cp && isWordExtender(cp, state->buffer.empty())) {
      if (state->navigating && cp >= '1' && cp <= '6' &&
          int(cp - '1') < (int)state->cands.size()) {
        commitWord(ic, state, state->cands[cp - '1'], /*trailingSpace=*/true);
        event.filterAndAccept();
        return;
      }
      appendCp(state->buffer, cp);
      state->navigating = false;
      updateCompletion(ic, state);
      event.filterAndAccept();
      return;
    }

    // (1bis) Ctrl+Backspace pendant la composition : ABANDONNE le mot en cours
    //        (rien n'est committé ni appris). Avant, le mot était committé puis
    //        l'app effaçait « le mot précédent » — c'est-à-dire celui qu'on
    //        venait de committer : flash visuel + apprentissage pollué.
    if (sym == FcitxKey_BackSpace &&
        states.test(fcitx::KeyState::Ctrl) && !state->buffer.empty()) {
      state->buffer.clear();
      state->navigating = false;
      clearPanel(ic);
      event.filterAndAccept();
      return;
    }

    // (2) Backspace pendant la composition → édite le buffer.
    if (sym == FcitxKey_BackSpace && !mod && !state->buffer.empty()) {
      popLastCp(state->buffer);
      state->navigating = false;
      if (state->buffer.empty())
        clearPanel(ic);
      else
        updateCompletion(ic, state);
      event.filterAndAccept();
      return;
    }

    // (2bis) Backspace AVEC BUFFER VIDE : on supprime du texte DÉJÀ committé
    //        — Backspace simple (un caractère) ou Ctrl+Backspace (un MOT). La
    //        barre mot-suivant est spéculative : on la FERME et on laisse la
    //        touche filer à l'application (pas de filterAndAccept). Sans cette
    //        branche, Ctrl+Backspace (mod) tombait dans « (4) if (mod) return »
    //        et la barre restait ouverte même une fois l'input entièrement vidé.
    if (sym == FcitxKey_BackSpace && state->buffer.empty()) {
      state->navigating = false;
      if (ic->inputPanel().candidateList())
        clearPanel(ic);
      return; // l'app reçoit le Backspace / la suppression de mot
    }

    // (3) Composition active (buffer non vide). Les branches dédiées exigent
    //     « sans modificateur » : Ctrl+Tab (onglet suivant), Ctrl+Entrée
    //     (envoi)… committent le littéral et FILENT à l'application (la
    //     branche « toute autre touche » en bas) au lieu d'être détournés.
    //     ↑/↓ ne sont PLUS capturés : dans un éditeur multi-lignes ils
    //     committent et déplacent le curseur — seul Tab/⇧Tab entre dans la
    //     barre (horizontale : ←/→ s'y déplacent une fois entré).
    if (!state->buffer.empty()) {
      // mode emoji (':') : la barre est une GRILLE de 8 colonnes (cf
      // panelview) → ↑/↓ y sautent d'une LIGNE (±8, wrap). HORS grille,
      // ↑/↓ ne sont pas capturés (ils déplacent le curseur — politique v6).
      bool emojiGrid = state->buffer[0] == ':';
      if (!mod && sym == FcitxKey_Tab) {
        navigate(ic, state, +1);
        event.filterAndAccept();
        return;
      }
      if (!mod && sym == FcitxKey_ISO_Left_Tab) {
        navigate(ic, state, -1); // ⇧Tab : entre par la DROITE de la barre
        event.filterAndAccept();
        return;
      }
      if (!mod && emojiGrid && sym == FcitxKey_Down) {
        navigate(ic, state, +8);
        event.filterAndAccept();
        return;
      }
      if (!mod && emojiGrid && sym == FcitxKey_Up) {
        navigate(ic, state, -8);
        event.filterAndAccept();
        return;
      }
      if (!mod && state->navigating &&
          (sym == FcitxKey_Left || sym == FcitxKey_Right)) {
        navigate(ic, state, sym == FcitxKey_Right ? +1 : -1);
        event.filterAndAccept();
        return;
      }
      if (!mod && sym == FcitxKey_space) {
        std::string lit = state->buffer;
        std::string chosen = chooseOnSpace(state);
        bool autoApplied = !state->navigating && chosen != lit;
        std::string committed = applyCase(chosen, lit);
        commitWord(ic, state, chosen, /*trailingSpace=*/true);
        if (autoApplied) { // arme la fenêtre de revert (cf (0))
          state->lastAutoLit = lit;
          state->lastAutoWord = committed;
          state->lastAutoCps = uint32_t(decodeUtf8(committed).size()) + 1;
        }
        event.filterAndAccept();
        return;
      }
      if (!mod && (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)) {
        if (state->navigating) {
          commitWord(ic, state, highlighted(state), /*space=*/false);
          event.filterAndAccept(); // suggestion prise → on avale Entrée
        } else {
          commitWord(ic, state, state->buffer, /*space=*/false);
          // littéral validé → on LAISSE passer Entrée (retour-ligne / envoi).
        }
        return;
      }
      if (!mod && sym == FcitxKey_Escape) {
        // Échap ANNULE la suggestion : committe le littéral tel quel (on ne
        // perd jamais la frappe) — SANS apprendre le fragment (annuler n'est
        // pas valider). Par défaut la touche FILE ensuite à l'application
        // (vim sort du mode insertion au premier Échap) ; escapeForward=false
        // pour l'avaler (un dialogue ne se ferme alors pas par surprise).
        commitWord(ic, state, state->buffer, /*space=*/false, /*learn=*/false);
        if (!engineCfg().escapeForward)
          event.filterAndAccept();
        return;
      }
      // toute autre touche (ponctuation, flèches ↑/↓, Home/End, raccourcis…) :
      // termine le mot SANS espace puis laisse la touche filer vers
      // l'application. La PONCTUATION applique la même correction que l'Espace
      // (« teh. » → « the. ») — sauf après un déclencheur (':xyz' / ';xyz').
      bool punctFix = !mod &&
                      (cp == '.' || cp == ',' || cp == ';' || cp == ':' ||
                       cp == '!' || cp == '?') &&
                      !isTriggerBuffer(state->buffer);
      bool fr = engineCfg().frenchSpacing && !mod &&
                !isTriggerBuffer(state->buffer);
      // typographie française (opt-in) — guillemet OUVRANT « : committer le mot,
      // puis « + fine insécable, et AVALER la touche (sinon « arriverait APRÈS
      // la fine). « U+00AB » suivi de « U+202F ».
      if (fr && cp == 0x00AB) {
        commitWord(ic, state, state->buffer, /*space=*/false);
        ic->commitString("\xC2\xAB\xE2\x80\xAF");
        event.filterAndAccept();
        return;
      }
      commitWord(ic, state, punctFix ? chooseOnSpace(state) : state->buffer,
                 /*space=*/false);
      // fine insécable AVANT ; : ! ? et guillemet fermant » (la touche file
      // ensuite à l'appli, qui insère la ponctuation après la fine).
      if (fr && needsFrenchThinBefore(cp))
        ic->commitString("\xE2\x80\xAF"); // U+202F
      if (cp == '.' || cp == '!' || cp == '?')
        state->ctx.clear(); // fin de phrase → on repart à neuf
      return;
    }

    // (4) Buffer vide. La barre de mot-suivant est purement informative tant
    //     qu'on n'appuie pas sur Tab : tout le reste passe à l'application.
    if (mod) {
      return; // raccourcis (Ctrl+C…) intacts
    }
    auto list = ic->inputPanel().candidateList();
    bool hasList = list && list->size() > 0;
    if (hasList && sym == FcitxKey_Escape) {
      // Échap ferme la barre mot-suivant ; par défaut (escapeForward) la
      // touche file AUSSI à l'application — cohérent avec la composition.
      state->navigating = false;
      clearPanel(ic);
      if (!engineCfg().escapeForward)
        event.filterAndAccept();
      return;
    }
    if (state->navigating && hasList) {
      // barre horizontale : Tab/→ et ⇧Tab/← naviguent ; ↑/↓ sortent de la
      // navigation et filent à l'application (branche « toute autre touche »).
      if (sym == FcitxKey_Tab || sym == FcitxKey_Right) {
        navigate(ic, state, +1);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Left) {
        navigate(ic, state, -1);
        event.filterAndAccept();
        return;
      }
      if (cp >= '1' && cp <= '6' &&
          int(cp - '1') < (int)state->cands.size()) {
        commitWord(ic, state, state->cands[cp - '1'], /*space=*/true);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_space) {
        commitWord(ic, state, highlighted(state), /*space=*/true);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) {
        commitWord(ic, state, highlighted(state), /*space=*/true);
        event.filterAndAccept();
        return;
      }
      // toute autre touche annule la navigation et file à l'appli.
      state->navigating = false;
      clearPanel(ic);
      return;
    }
    if (hasList && (sym == FcitxKey_Tab || sym == FcitxKey_ISO_Left_Tab)) {
      // Tab entre par la gauche, ⇧Tab par la DROITE de la barre.
      navigate(ic, state, sym == FcitxKey_ISO_Left_Tab ? -1 : 0);
      event.filterAndAccept();
      return;
    }
    // pas de composition, pas de navigation : on efface la barre éphémère et on
    // laisse la touche (chiffre, espace, Entrée, flèche…) agir normalement.
    if (hasList)
      clearPanel(ic);
    if (cp == '.' || cp == '!' || cp == '?')
      state->ctx.clear();
    // typographie française (opt-in) AUSSI quand le buffer est vide (mot déjà
    // committé, p.ex. après une espace) : « ouvrant → « + fine (touche avalée) ;
    // ; : ! ? » → fine insécable avant, en absorbant une espace ordinaire déjà
    // tapée (« mot ! » → mot + U+202F + !, jamais de double espace).
    if (engineCfg().frenchSpacing) {
      if (cp == 0x00AB) {
        ic->commitString("\xC2\xAB\xE2\x80\xAF");
        event.filterAndAccept();
        return;
      }
      if (needsFrenchThinBefore(cp))
        frenchThinBefore(ic);
    }
    // Espace sur buffer vide (après ponctuation, typiquement) : la touche file
    // à l'appli ET on affiche la barre de suggestion — contexte vide compris
    // (le daemon répond avec les amorces de phrase <s>).
    if (sym == FcitxKey_space)
      showNextWord(ic, state);
  }

  void reset(const fcitx::InputMethodEntry &,
             fcitx::InputContextEvent &event) override {
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&factory_);
    // Ne jamais perdre ce qui est tapé : un mot en cours (préédition) est
    // committé tel quel avant le reset (changement de focus, etc.).
    if (!state->buffer.empty())
      ic->commitString(state->buffer);
    state->buffer.clear();
    state->ctx.clear();
    state->cands.clear();
    state->navigating = false;
    state->vetoAuto = false;
    state->lastAutoCps = 0;
    state->lastAutoLit.clear();
    clearPanel(ic);
  }

private:
  void clearPanel(fcitx::InputContext *ic) {
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // Insère une fine insécable (U+202F) avant la ponctuation haute (amélioration
  // C). Absorbe une espace ORDINAIRE déjà tapée (sinon « mot  ! » double espace)
  // et ne fait rien si une fine est déjà là — best-effort via SurroundingText.
  void frenchThinBefore(fcitx::InputContext *ic) {
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      auto cps = decodeUtf8(ic->surroundingText().text());
      unsigned int cur = ic->surroundingText().cursor();
      if (cur > 0 && cur <= cps.size()) {
        uint32_t prev = cps[cur - 1];
        if (prev == 0x202F)
          return; // déjà une fine insécable → ne rien ajouter
        if (prev == ' ')
          ic->deleteSurroundingText(-1, 1); // absorbe l'espace ordinaire
      }
    }
    ic->commitString("\xE2\x80\xAF"); // U+202F
  }

  // Contexte pour une requête : la VRAIE phrase avant le curseur
  // (SurroundingText) en priorité — source de vérité pour l'accord grammatical
  // et le mot-suivant —, sinon nos derniers mots committés (apps sans
  // SurroundingText : terminaux, certains Electron). Fenêtre élargie à 8 mots :
  // le n-gramme n'en lit que 2, mais la couche d'accord du daemon a besoin de
  // tout le groupe nominal (déterminant + adjectifs intercalés).
  std::vector<std::string> contextFor(fcitx::InputContext *ic,
                                      PredictState *state) {
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      auto cps = decodeUtf8(ic->surroundingText().text());
      unsigned int cur = ic->surroundingText().cursor();
      if (cur < cps.size())
        cps.resize(cur);
      auto ws = lastWords(cps, 8);
      if (!ws.empty())
        return ws;
    }
    return state->ctx;
  }

  // 8 mots de contexte (repli quand pas de SurroundingText) : le daemon
  // n'utilise que les 2 derniers pour les n-grammes, mais tout le contexte sert
  // à la DÉTECTION DE LANGUE et à l'ACCORD (déterminant gouverneur du SN).
  void pushCtx(PredictState *state, const std::string &word) {
    state->ctx.push_back(word);
    if (state->ctx.size() > 8)
      state->ctx.erase(state->ctx.begin());
  }

  const std::string &highlighted(PredictState *state) {
    if (state->cands.empty())
      return state->buffer;
    int i = state->navIndex;
    if (i < 0 || i >= (int)state->cands.size())
      i = 0;
    return state->cands[i];
  }

  // Mot retenu quand on appuie sur Espace en cours de composition :
  //  - en navigation → le candidat surligné ;
  //  - sinon, si le préfixe n'est PAS un mot réel → autocomplétion/autocorrection ;
  //  - sinon → le littéral (on ne touche pas à un vrai mot).
  std::string chooseOnSpace(PredictState *state) {
    if (state->navigating && !state->cands.empty())
      return highlighted(state);
    // l'utilisateur vient de refuser une auto-application (revert Backspace) :
    // on respecte son choix, le littéral reste.
    if (state->vetoAuto)
      return state->buffer;
    // auto-application haute confiance seulement (complétion de préfixe, ou
    // faute simple) ; sinon on garde le littéral — jamais "j'ai" → "jail".
    if (!state->literalIsWord && !state->autocomplete.empty())
      return state->autocomplete;
    return state->buffer;
  }

  // Surligne un candidat. dir : +1 suivant, -1 précédent, 0 (1er appui) → le
  // 1er. Premier appui en ARRIÈRE (⇧Tab/↑) → on entre par la droite (dernier).
  // On calcule l'index nous-mêmes (robuste, indépendant de nextCandidate()).
  void navigate(fcitx::InputContext *ic, PredictState *state, int dir) {
    auto list = ic->inputPanel().candidateList();
    if (!list || list->size() == 0)
      return;
    int sz = list->size();
    int next = state->navigating ? state->navIndex + dir
                                 : (dir < 0 ? sz - 1 : 0);
    next = ((next % sz) + sz) % sz; // wrap (marche aussi pour ±8 en grille)
    state->navigating = true;
    state->navIndex = next;
    if (auto *cl = dynamic_cast<PredictCandidateList *>(list.get()))
      cl->setCursorIndex(next);
    // reflète le candidat surligné dans la préédition (mode complétion) —
    // SAUF en mode emoji : le préedit reste ":requête" (le pill de la grille
    // montre déjà la sélection, et l'UI déduit le mode grille du préfixe ':').
    if (!state->buffer.empty() && next < (int)state->cands.size()) {
      std::string shown = state->buffer[0] == ':'
                              ? state->buffer
                              : applyCase(state->cands[next], state->buffer);
      fcitx::Text preedit(shown, fcitx::TextFormatFlag::Underline);
      preedit.setCursor(shown.size());
      ic->inputPanel().setClientPreedit(preedit);
      ic->updatePreedit();
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // Valide un mot : applique la casse du buffer, committe, apprend (sauf
  // annulation), met à jour le contexte, puis (si espace) propose le suivant.
  // Gère aussi les candidats MULTI-MOTS (« sais pas » : apprentissage et
  // contexte mot à mot) et l'auto-majuscule de début de phrase (opt-in).
  void commitWord(fcitx::InputContext *ic, PredictState *state,
                  const std::string &raw, bool trailingSpace,
                  bool learn = true) {
    bool trigger = isTriggerBuffer(state->buffer); // emoji ':' / snippet ';'
    std::string word = applyCase(raw, state->buffer);
    // Auto-majuscule (amélioration D) en DÉBUT DE PHRASE : on s'appuie sur le
    // contexte EFFECTIF (contextFor → SurroundingText prioritaire). lastWords
    // s'arrête aux frontières « . ! ? », donc un contexte VIDE signifie début de
    // champ OU juste après une fin de phrase — exactement les cas à capitaliser.
    // capFirst ne touche que la 1re lettre (sigles/mots déjà capitalisés sains).
    if (engineCfg().autoCapitalize && !trigger && contextFor(ic, state).empty())
      word = capFirst(word);
    ic->commitString(trailingSpace ? word + " " : word);
    if (trigger) {
      // un emoji/snippet choisi compte comme « favori » (le daemon le
      // remontera) ; le contexte de mots reste inchangé.
      if (learn && word != state->buffer)
        learnDaemon(std::string{}, word);
    } else {
      // multi-mots : chaque mot nourrit l'apprentissage et le contexte.
      size_t from = 0;
      while (from <= word.size()) {
        size_t sp = word.find(' ', from);
        std::string w = word.substr(from, sp == std::string::npos
                                               ? std::string::npos
                                               : sp - from);
        if (!w.empty()) {
          if (learn) {
            std::string prev =
                state->ctx.empty() ? std::string{} : state->ctx.back();
            learnDaemon(prev, w);
          }
          pushCtx(state, w);
        }
        if (sp == std::string::npos)
          break;
        from = sp + 1;
      }
    }
    state->buffer.clear();
    state->cands.clear();
    state->navigating = false;
    state->vetoAuto = false; // le veto ne vaut que pour le mot en cours
    if (trailingSpace)
      showNextWord(ic, state);
    else
      clearPanel(ic);
  }

  // Mode complétion : candidats commençant par le buffer (avec autocorrection).
  void updateCompletion(fcitx::InputContext *ic, PredictState *state) {
    ic->inputPanel().reset();
    if (state->buffer.empty()) {
      showNextWord(ic, state);
      return;
    }
    auto reply = queryDaemon(contextFor(ic, state), state->buffer);
    state->cands = reply.candidates;
    state->literalIsWord = reply.literalIsWord;
    state->autocomplete = reply.autocomplete;
    if (state->cands.empty())
      state->cands.push_back(state->buffer); // repli : le brut

    // Pas de filet, pas de remplacement : si l'app n'expose pas le
    // SurroundingText, le revert Backspace d'une auto-application est
    // IMPOSSIBLE — l'Espace garde alors le littéral (les candidats restent,
    // Tab choisit). Les déclencheurs ':'/';' restent explicites.
    // Opt-out : autoApplyNeedsRevert=false.
    if (engineCfg().autoApplyNeedsRevert && !isTriggerBuffer(state->buffer) &&
        !(ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
          ic->surroundingText().isValid()))
      state->autocomplete.clear();

    // GHOST TEXT : si l'Espace va compléter le mot, le reste s'affiche déjà
    // dans le préedit, curseur entre le tapé et le fantôme ("bonjou‸r") —
    // uniquement quand l'auto-complétion PROLONGE octet-à-octet la frappe
    // (jamais pour une correction floue : la barre + liseré s'en chargent).
    std::string ghost;
    if (engineCfg().ghostText && !state->literalIsWord && !state->vetoAuto &&
        state->autocomplete.size() > state->buffer.size() &&
        state->autocomplete.compare(0, state->buffer.size(), state->buffer) ==
            0)
      ghost = state->autocomplete.substr(state->buffer.size());

    fcitx::Text preedit;
    preedit.append(state->buffer,
                   fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
    if (!ghost.empty())
      preedit.append(ghost,
                     fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline} |
                         fcitx::TextFormatFlag::Italic);
    preedit.setCursor(state->buffer.size());
    ic->inputPanel().setClientPreedit(preedit);
    setCandidates(ic, state);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // Mode mot-suivant : candidats prédits depuis le contexte (display-only).
  // Un contexte VIDE est une requête valide : le daemon répond avec les
  // amorces de phrase (<s>) — début de champ, ou après « . ! ? ».
  void showNextWord(fcitx::InputContext *ic, PredictState *state) {
    ic->inputPanel().reset();
    state->autocomplete.clear(); // pas de marquage « auto » en mot-suivant
    state->literalIsWord = false;
    if (!engineCfg().nextWordBar) { // mode calme : pas de barre spéculative
      state->cands.clear();
      clearPanel(ic);
      return;
    }
    // Programme exclu (terminal où la barre ne peut pas s'ancrer au curseur) :
    // mode calme ciblé, sans toucher la complétion pendant la frappe. Match
    // sous-chaîne INSENSIBLE à la casse (« ghostty » matche « com.…Ghostty »).
    auto lc = [](std::string s) {
      for (char &c : s)
        if (c >= 'A' && c <= 'Z')
          c += 32;
      return s;
    };
    const std::string prog = lc(ic->program());
    for (const auto &pat : engineCfg().nextWordBarExclude)
      if (!pat.empty() && prog.find(lc(pat)) != std::string::npos) {
        state->cands.clear();
        clearPanel(ic);
        return;
      }
    auto ctx = contextFor(ic, state);
    auto reply = queryDaemon(ctx, "");
    state->cands = reply.candidates;
    if (state->cands.empty()) {
      clearPanel(ic);
      return;
    }
    setCandidates(ic, state);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // --- Reformulation : sélection → 3 variantes LLM, le choix remplace ---
  void setReformulationCandidates(fcitx::InputContext *ic, PredictState *state) {
    auto list = std::make_unique<PredictCandidateList>();
    for (auto &v : state->cands)
      list->append(v, /*autoApply=*/false); // verbatim (phrases) — pas d'applyCase
    list->setCursorIndex(state->navIndex);   // variante surlignée
    ic->inputPanel().setCandidateList(std::move(list));
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void enterReformulation(fcitx::InputContext *ic, PredictState *state) {
    if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) ||
        !ic->surroundingText().isValid())
      return; // l'app n'expose pas la sélection (ex. certains Electron)
    std::string sel = ic->surroundingText().selectedText();
    if (sel.empty())
      return; // rien de sélectionné
    auto variants = reformulateDaemon(sel); // BLOQUANT quelques secondes (génération)
    if (variants.empty())
      return;
    state->cands = std::move(variants);
    state->navIndex = 0;
    state->navigating = true;
    state->reformulating = true;
    setReformulationCandidates(ic, state);
  }

  void exitReformulation(fcitx::InputContext *ic, PredictState *state) {
    state->reformulating = false;
    state->navigating = false;
    state->cands.clear();
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void commitReformulation(fcitx::InputContext *ic, PredictState *state, int idx) {
    if (idx >= 0 && idx < (int)state->cands.size())
      ic->commitString(state->cands[idx]); // remplace la sélection (commit-over-selection)
    exitReformulation(ic, state);
  }

  void setCandidates(fcitx::InputContext *ic, PredictState *state) {
    // liste maison : jusqu'à 24 candidats (grille emoji) sans la limite de
    // 10 labels de CommonCandidateList (qui FATAL-abort fcitx au-delà).
    auto list = std::make_unique<PredictCandidateList>();
    // le candidat que l'Espace appliquera est marqué (gras → liseré dans l'UI)
    bool willAuto = !state->buffer.empty() && !state->literalIsWord &&
                    !state->autocomplete.empty() && !state->vetoAuto;
    for (auto &w : state->cands)
      list->append(applyCase(w, state->buffer),
                   willAuto && w == state->autocomplete);
    list->setCursorIndex(-1); // aucun surlignage tant qu'on ne navigue pas
    ic->inputPanel().setCandidateList(std::move(list));
  }

  fcitx::Instance *instance_;
  fcitx::FactoryFor<PredictState> factory_;
};

class PredictEngineFactory : public fcitx::AddonFactory {
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new PredictEngine(manager->instance());
  }
};

FCITX_ADDON_FACTORY(PredictEngineFactory)
