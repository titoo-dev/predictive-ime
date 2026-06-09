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
#include <functional>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace {

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
// VIDE démarre le picker emoji (":coeur" → ❤️) — jamais en milieu de mot, donc
// "10:30" ou "voici :" tapent normalement.
bool isWordExtender(uint32_t cp, bool bufferEmpty) {
  if (isLetterCp(cp))
    return true;
  if (bufferEmpty)
    return cp == ':';
  return cp == '\'' || cp == 0x2019 || cp == '-' || (cp >= '0' && cp <= '9');
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

int connectDaemon() {
  const char *envSock = ::getenv("IME_PREDICTORD_SOCK");
  std::string path = envSock ? envSock : "/tmp/ime-predictord.sock";
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

DaemonReply queryDaemon(const std::vector<std::string> &context,
                        const std::string &prefix) {
  DaemonReply out;
  int fd = connectDaemon();
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
  uint32_t lastAutoCps = 0;          // points de code committés à effacer
  bool vetoAuto = false;             // l'utilisateur a refusé : Espace garde le littéral
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
      fprintf(stderr, "[predict] sym=0x%x cp=0x%x buf='%s' nav=%d cands=%zu\n",
              sym, cp, state->buffer.c_str(), int(state->navigating),
              state->cands.size());

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

    // (3) Composition active (buffer non vide).
    if (!state->buffer.empty()) {
      if (sym == FcitxKey_Tab || sym == FcitxKey_Down) {
        navigate(ic, state, +1);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Up) {
        navigate(ic, state, -1); // ⇧Tab/↑ : entre par la DROITE de la barre
        event.filterAndAccept();
        return;
      }
      // en navigation, la barre étant horizontale, ←/→ s'y déplacent aussi
      if (state->navigating &&
          (sym == FcitxKey_Left || sym == FcitxKey_Right)) {
        navigate(ic, state, sym == FcitxKey_Right ? +1 : -1);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_space) {
        std::string lit = state->buffer;
        std::string chosen = chooseOnSpace(state);
        bool autoApplied = !state->navigating && chosen != lit;
        std::string committed = applyCase(chosen, lit);
        commitWord(ic, state, chosen, /*trailingSpace=*/true);
        if (autoApplied) { // arme la fenêtre de revert (cf (0))
          state->lastAutoLit = lit;
          state->lastAutoCps = uint32_t(decodeUtf8(committed).size()) + 1;
        }
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) {
        if (state->navigating) {
          commitWord(ic, state, highlighted(state), /*space=*/false);
          event.filterAndAccept(); // suggestion prise → on avale Entrée
        } else {
          commitWord(ic, state, state->buffer, /*space=*/false);
          // littéral validé → on LAISSE passer Entrée (retour-ligne / envoi).
        }
        return;
      }
      if (sym == FcitxKey_Escape) {
        // Échap ANNULE la suggestion : ferme la barre et committe le littéral
        // tel quel (on ne perd jamais la frappe) — SANS apprendre le fragment
        // (annuler n'est pas valider).
        commitWord(ic, state, state->buffer, /*space=*/false, /*learn=*/false);
        event.filterAndAccept();
        return;
      }
      // toute autre touche (ponctuation, flèches, Home/End…) : termine le mot
      // SANS espace puis laisse la touche filer vers l'application. La
      // PONCTUATION applique la même correction que l'Espace (« teh. » →
      // « the. ») — sauf en mode emoji (« :xyz, » reste littéral).
      bool punctFix = (cp == '.' || cp == ',' || cp == ';' || cp == ':' ||
                       cp == '!' || cp == '?') &&
                      state->buffer[0] != ':';
      commitWord(ic, state, punctFix ? chooseOnSpace(state) : state->buffer,
                 /*space=*/false);
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
      // Échap ferme la barre mot-suivant (et n'atteint PAS l'application —
      // un 2e Échap, barre fermée, passera normalement).
      state->navigating = false;
      clearPanel(ic);
      event.filterAndAccept();
      return;
    }
    if (state->navigating && hasList) {
      if (sym == FcitxKey_Tab || sym == FcitxKey_Down ||
          sym == FcitxKey_Right) {
        navigate(ic, state, +1);
        event.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Up ||
          sym == FcitxKey_Left) {
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

  // Contexte pour une requête : nos derniers mots, sinon le texte environnant.
  std::vector<std::string> contextFor(fcitx::InputContext *ic,
                                      PredictState *state) {
    if (!state->ctx.empty())
      return state->ctx;
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      auto cps = decodeUtf8(ic->surroundingText().text());
      unsigned int cur = ic->surroundingText().cursor();
      if (cur < cps.size())
        cps.resize(cur);
      return lastWords(cps, 2);
    }
    return {};
  }

  void pushCtx(PredictState *state, const std::string &word) {
    state->ctx.push_back(word);
    if (state->ctx.size() > 2)
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
    if (next < 0)
      next = sz - 1;
    if (next >= sz)
      next = 0;
    state->navigating = true;
    state->navIndex = next;
    if (auto *cl = dynamic_cast<fcitx::CommonCandidateList *>(list.get()))
      cl->setGlobalCursorIndex(next);
    // reflète le candidat surligné dans la préédition (mode complétion).
    if (!state->buffer.empty() && next < (int)state->cands.size()) {
      std::string shown = applyCase(state->cands[next], state->buffer);
      fcitx::Text preedit(shown, fcitx::TextFormatFlag::Underline);
      preedit.setCursor(shown.size());
      ic->inputPanel().setClientPreedit(preedit);
      ic->updatePreedit();
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // Valide un mot : applique la casse du buffer, committe, apprend (sauf
  // annulation), met à jour le contexte, puis (si espace) propose le suivant.
  void commitWord(fcitx::InputContext *ic, PredictState *state,
                  const std::string &raw, bool trailingSpace,
                  bool learn = true) {
    bool emojiMode = !state->buffer.empty() && state->buffer[0] == ':';
    std::string word = applyCase(raw, state->buffer);
    ic->commitString(trailingSpace ? word + " " : word);
    if (emojiMode) {
      // un emoji choisi compte comme « favori » (le daemon le remontera) ; le
      // contexte de mots reste inchangé — un emoji ne porte pas de syntaxe.
      if (learn && word != state->buffer)
        learnDaemon(std::string{}, word);
    } else {
      if (learn) {
        std::string prev =
            state->ctx.empty() ? std::string{} : state->ctx.back();
        learnDaemon(prev, word);
      }
      pushCtx(state, word);
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

    fcitx::Text preedit(state->buffer, fcitx::TextFormatFlag::Underline);
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

  void setCandidates(fcitx::InputContext *ic, PredictState *state) {
    auto list = std::make_unique<fcitx::CommonCandidateList>();
    list->setPageSize(6);
    // le candidat que l'Espace appliquera est marqué (gras → liseré dans l'UI)
    bool willAuto = !state->buffer.empty() && !state->literalIsWord &&
                    !state->autocomplete.empty() && !state->vetoAuto;
    for (auto &w : state->cands)
      list->append<PredictCandidate>(applyCase(w, state->buffer),
                                     willAuto && w == state->autocomplete);
    list->setGlobalCursorIndex(-1); // aucun surlignage tant qu'on ne navigue pas
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
