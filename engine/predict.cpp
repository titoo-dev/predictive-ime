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
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/eventdispatcher.h>
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
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
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
  // Deux phases (E5) : la barre mot-suivant s'affiche TOUT DE SUITE (n-gram),
  // puis se met à jour quand le neural aboutit (2e ligne sur la même
  // connexion, lue depuis la boucle d'événements fcitx — zéro blocage).
  // Sans neural côté daemon la réponse n'est jamais "pending" → aucun effet.
  bool asyncNextWord = true;
  // Nombre de variantes de reformulation demandées (borné 1-6 : sélection
  // aux chiffres 1-6 et bulle compacte).
  int reformCount = 3;
  // Programmes (sous-chaînes de ic->program(), ex. "ghostty") où la barre
  // SPÉCULATIVE mot-suivant est supprimée : dans un terminal elle n'a pas de
  // preedit pour s'ancrer au curseur et « traîne » derrière lui. La complétion
  // pendant la frappe (ancrée au preedit) reste, elle.
  std::vector<std::string> nextWordBarExclude;
};

const std::string &configPath() {
  static const std::string path = [] {
    const char *x = ::getenv("XDG_CONFIG_HOME");
    const char *h = ::getenv("HOME");
    return (x ? std::string(x)
              : std::string(h ? h : "/tmp") + "/.config") +
           "/ime-predictord/config.json";
  }();
  return path;
}

const EngineCfg &engineCfg() {
  static EngineCfg cfg;
  static time_t stamp = -1;
  const std::string &path = configPath();
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
        fresh.asyncNextWord = j.value("asyncNextWord", fresh.asyncNextWord);
        fresh.reformCount = std::min(
            6, std::max(1, j.value("reformCount", fresh.reformCount)));
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

// Lecture/écriture de la VALEUR de "lang" dans le TEXTE de config.json —
// formatage et clés-commentaires préservés (pas de re-sérialisation). On
// écrit sur la CIBLE réelle (realpath : le fichier peut être un lien stow
// vers les dotfiles), de façon atomique (tmp + rename). Le daemon recharge
// sur mtime (granularité seconde) : si le rename retombe dans la même
// seconde, on pousse le mtime d'une seconde pour que la bascule soit vue.
// Localise la valeur de "lang" dans `text` → [q1+1, q2) ; false si absente.
bool langValueSpan(const std::string &text, size_t &q1, size_t &q2) {
  size_t k = text.find("\"lang\"");
  if (k == std::string::npos)
    return false;
  size_t colon = text.find(':', k + 6);
  q1 = colon == std::string::npos ? colon : text.find('"', colon + 1);
  q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  return q2 != std::string::npos;
}

std::string readLang() {
  std::ifstream in(configPath());
  if (!in)
    return "";
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  size_t q1, q2;
  return langValueSpan(text, q1, q2) ? text.substr(q1 + 1, q2 - q1 - 1)
                                     : std::string{};
}

bool writeLang(const std::string &next) {
  char *rp = ::realpath(configPath().c_str(), nullptr);
  if (!rp)
    return false;
  const std::string path = rp;
  ::free(rp);
  struct stat before {};
  ::stat(path.c_str(), &before);
  std::ifstream in(path);
  if (!in)
    return false;
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  in.close();
  size_t q1, q2;
  if (!langValueSpan(text, q1, q2))
    return false;
  text.replace(q1 + 1, q2 - q1 - 1, next);
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp, std::ios::trunc);
  if (!out)
    return false;
  out << text;
  out.close();
  if (!out || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  struct stat after {};
  if (::stat(path.c_str(), &after) == 0 &&
      after.st_mtime <= before.st_mtime) {
    struct timespec ts[2] = {{0, UTIME_OMIT}, {before.st_mtime + 1, 0}};
    ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
  }
  return true;
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
// chiffre seulement si le buffer est déjà entamé (mot en cours). ';' sur buffer
// VIDE démarre un SNIPPET (";mail" → expansion) — jamais en milieu de mot. Le
// picker emoji, lui, n'est PLUS déclenché par ':' tapé mais par Super+; (cf
// bloc (0-emoji) du keyEvent) : ':' se tape donc littéralement partout.
bool isWordExtender(uint32_t cp, bool bufferEmpty) {
  if (isLetterCp(cp))
    return true;
  if (bufferEmpty)
    return cp == ';';
  return cp == '\'' || cp == 0x2019 || cp == '-' || (cp >= '0' && cp <= '9');
}

// Buffer « déclencheur » (emoji ':' ou snippet ';') : pas d'apprentissage de
// bigramme ni de contexte — ce n'est pas de la prose.
bool isTriggerBuffer(const std::string &buffer) {
  return !buffer.empty() && (buffer[0] == ':' || buffer[0] == ';');
}

// Buffer du PICKER EMOJI. Son ':' de tête est SYNTHÉTIQUE (posé par Super+;,
// jamais tapé) : aucun chemin « littéral » ne doit le committer comme texte —
// cf la garde en tête de commitWord, qui ferme le picker à la place. Le ';'
// des snippets, lui, est bien tapé et reste littéral.
bool isEmojiBuffer(const std::string &buffer) {
  return !buffer.empty() && buffer[0] == ':';
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
// Anglais : « i » seul et les contractions « i'… » (i'm, i'll, i've, i'd)
// prennent TOUJOURS la majuscule — aucun mot français n'est « i » ni ne
// commence par « i' », la règle est donc sûre sans condition de langue.
std::string applyCase(const std::string &cand_, const std::string &buffer) {
  std::string cand = cand_;
  if (cand == "i" || cand.rfind("i'", 0) == 0)
    cand[0] = 'I';
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

// Supprime `n` cp avant le curseur ET répercute sur la copie LOCALE du
// SurroundingText : l'app ne renvoie son update qu'après un aller-retour,
// et une complétion relancée juste derrière lirait sinon l'ancien texte
// (le mot supprimé polluerait son propre contexte).
void deleteSurroundingBefore(fcitx::InputContext *ic, unsigned n) {
  ic->deleteSurroundingText(-int(n), n);
  auto &st = ic->surroundingText();
  auto cps = decodeUtf8(st.text());
  size_t cur = st.cursor();
  if (n == 0 || n > cur || cur > cps.size())
    return;
  std::string out;
  for (size_t i = 0; i < cps.size(); i++)
    if (i < cur - n || i >= cur)
      appendCp(out, cps[i]);
  st.setText(out, cur - n, cur - n);
}

// ----------------------------------------------------------- daemon IPC -----
struct DaemonReply {
  std::vector<std::string> candidates;
  bool literalIsWord = false;
  std::string autocomplete; // mot à appliquer sur Espace (haute confiance), ou ""
  std::string ghost;        // complétion affichée en fantôme (→ l'accepte)
  bool accentOnly = false;  // autocomplete = pure restauration d'accents
  bool pending = false;     // un refresh neural suivra sur la même connexion
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

// pendingFd (E5) : si non-nul et que le daemon annonce un refresh à venir
// ("pending":true), la connexion reste OUVERTE et son fd est rendu à
// l'appelant (qui la surveille depuis la boucle d'événements fcitx) ; sinon
// elle est fermée comme avant.
DaemonReply queryDaemon(const std::vector<std::string> &context,
                        const std::string &prefix,
                        const std::string &wide = {},
                        int *pendingFd = nullptr) {
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
  // Contexte LARGE (texte brut avant le curseur, phrases précédentes et
  // ponctuation comprises) : réservé au prédicteur neuronal côté daemon — son
  // avantage mesuré EXIGE le contexte long. Le n-gram garde `context` (borné
  // à la phrase).
  if (!wide.empty())
    req["wide"] = wide;
  if (pendingFd)
    req["async"] = true; // deux phases : n-gram immédiat + refresh neural
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
    if (buf.find('\n') != std::string::npos)
      break;
  }
  try {
    json resp = json::parse(buf.substr(0, buf.find('\n')));
    for (auto &c : resp.value("candidates", json::array()))
      out.candidates.push_back(c.get<std::string>());
    out.literalIsWord = resp.value("literalIsWord", false);
    out.autocomplete = resp.value("autocomplete", std::string{});
    // repli vieux daemon (pas de champ ghost) : le fantôme suit l'autocomplete
    out.ghost = resp.value("ghost", out.autocomplete);
    out.accentOnly = resp.value("accentOnly", false);
    out.pending = resp.value("pending", false);
  } catch (...) {
  }
  if (out.pending && pendingFd) {
    *pendingFd = fd; // la 2e ligne arrivera ici — surveillée par l'event loop
    return out;
  }
  ::close(fd);
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

// Reformulation : on envoie le texte + le MODE + un NONCE (pour régénérer), le
// daemon GÉNÈRE les variantes (Groq <1s, ou neural local quelques s). Timeout
// long (≠ prédiction par-frappe) — action explicite, exécutée dans un thread.
struct ReformResult {
  std::vector<std::string> variants;
  std::string source; // "groq" / "none" → badge du header
  // pourquoi c'est vide : no_key / auth / network / http / empty / superseded
  // (cf reformulate_http.h) — l'engine en fait un panneau compact.
  std::string error;
};
// La réponse peut arriver en PLUSIEURS lignes : des lignes
// {"variants":[...],"partial":true} (STREAMING local — la 1re variante
// s'affiche pendant que les suivantes se génèrent encore), puis la ligne
// FINALE {"variants":[...],"source":...} qui clôt l'échange. `onPartial` est
// appelé depuis le thread appelant pour chaque ligne partielle.
ReformResult reformulateDaemon(
    const std::string &sentence, const std::string &mode, uint32_t nonce,
    int nWant,
    const std::function<void(std::vector<std::string>)> &onPartial = nullptr) {
  ReformResult out;
  int fd = connectDaemon(/*timeoutMs=*/12000);
  if (fd < 0)
    return out;
  json req;
  req["reformulate"] = sentence;
  req["n"] = nWant;
  req["mode"] = mode;
  req["nonce"] = nonce;
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
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string one = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      try {
        json resp = json::parse(one);
        std::vector<std::string> vars;
        for (auto &v : resp.value("variants", json::array()))
          vars.push_back(v.get<std::string>());
        if (resp.value("partial", false)) {
          if (onPartial)
            onPartial(std::move(vars));
          continue;
        }
        out.variants = std::move(vars);
        out.source = resp.value("source", std::string{"none"});
        out.error = resp.value("error", std::string{});
        ::close(fd);
        return out;
      } catch (...) {
      }
    }
  }
  ::close(fd);
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
  std::string ghost;                 // complétion fantôme (→ l'accepte)
  bool accentOnly = false;           // autocomplete = restauration d'accents pure
  // Fenêtre de REVERT d'une auto-application (Backspace juste après) :
  std::string lastAutoLit;           // le littéral qui a été remplacé
  std::string lastAutoWord;          // le mot qui avait été appliqué
  uint32_t lastAutoCps = 0;          // points de code committés à effacer
  bool vetoAuto = false;             // l'utilisateur a refusé : Espace garde le littéral
  bool langMenu = false;             // panneau de langue ouvert (Ctrl+Shift+L)
  int langIndex = 0;                 // choix surligné dans kLangChoices
  bool reformulating = false;        // mode reformulation : cands = variantes de la sélection
  bool reformLoading = false;        // génération en cours (placeholder « ⟳ »)
  std::string reformNotice;          // panneau d'échec (no_key/auth/network/…)
  std::string reformText;            // texte source (sélection / surrounding) — regen + revert
  bool reformFromSelection = false;  // false = repli « champ entier » : le
                                     // commit doit SUPPRIMER le champ lui-même
  int reformMode = 0;                // index dans kReformModes (←/→ pour changer)
  uint32_t reformNonce = 0;          // varie le seed → « régénérer » (Ctrl+Alt+R)
  uint32_t reformGen = 0;            // n° de génération : ignore les résultats obsolètes
  std::string reformSource;          // "groq"/"local"/"none" → badge du header
  // Revert (Backspace juste après remplacement → restaure l'original). One-shot.
  std::string reformRevertOrig;
  uint32_t reformRevertCps = 0;      // points de code de la variante committée à effacer
  // Génération de la barre mot-suivant (E5) : un refresh neural arrivé APRÈS
  // que l'état a changé (frappe, commit, reset) est jeté (gen différente).
  uint64_t nextWordGen = 0;
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
    labels_.emplace_back(); // pas de label (chips de mots/emoji)
  }
  // Candidat AVEC label (numéro) : mode reformulation → l'UI passe en liste
  // verticale numérotée (le label non vide est le signal lu par qmlui).
  void appendLabeled(std::string text, const std::string &label) {
    cands_.push_back(
        std::make_unique<PredictCandidate>(std::move(text), /*autoApply=*/false));
    fcitx::Text t;
    t.append(label);
    labels_.push_back(std::move(t));
  }
  void setCursorIndex(int i) { cursor_ = i; }

  const fcitx::Text &label(int idx) const override {
    return (idx >= 0 && idx < (int)labels_.size()) ? labels_[idx] : emptyLabel_;
  }
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
  std::vector<fcitx::Text> labels_;
  fcitx::Text emptyLabel_;
  int cursor_ = -1;
};

// Chiffre « physique » 0-based d'une touche pour la SÉLECTION dans les
// panneaux MODAUX (langue, reformulation) : '1'-'9' directs, sinon la rangée
// AZERTY non shiftée (&é"'(-è_ç — sur AZERTY les chiffres exigent Shift, et
// « autre touche » fermait le panneau EN SILENCE : bascule de langue perdue),
// sinon le keycode évdev 10-18 (rangée physique, indépendant de la
// disposition — couvre BÉPO & co en session réelle ; les tests headless ne
// portent pas de keycode). Ne PAS utiliser pendant la composition : é/è/ç/-
// y sont des lettres.
int panelDigit(const fcitx::Key &key, uint32_t cp) {
  if (cp >= '1' && cp <= '9')
    return int(cp - '1');
  static const uint32_t az[] = {'&', 0xE9, '"', '\'', '(', '-', 0xE8, '_', 0xE7};
  for (int i = 0; i < 9; i++)
    if (cp && cp == az[i])
      return i;
  if (key.code() >= 10 && key.code() <= 18)
    return key.code() - 10;
  return -1;
}

// Panneau de LANGUE (Ctrl+Shift+L) : valeur écrite dans config.json + libellé
// des chips. Extensible : ajouter une langue = une ligne ici (une fois le
// support daemon/modèle en place).
struct LangChoice { const char *value; const char *label; };
static const LangChoice kLangChoices[] = {
    {"fr", "Français"}, {"en", "English"},
    {"auto", "Auto"},   {"off", "Libre"},
};
static const int kNumLangChoices =
    int(sizeof(kLangChoices) / sizeof(kLangChoices[0]));

// Modes de reformulation : clé envoyée au daemon (cf reform_prompts.h) + libellé
// affiché dans le header de la bulle. ←/→ cyclent dans cette liste.
struct ReformMode { const char *key; const char *label; };
static const ReformMode kReformModes[] = {
    {"rephrase", "Reformuler"}, {"formal", "Formel"}, {"simple", "Simple"},
    {"short", "Court"},         {"correct", "Corriger"}, {"translate", "Traduire"},
};
static const int kNumReformModes =
    int(sizeof(kReformModes) / sizeof(kReformModes[0]));

} // namespace

class PredictEngine : public fcitx::InputMethodEngineV2 {
public:
  PredictEngine(fcitx::Instance *instance)
      : instance_(instance),
        factory_([](fcitx::InputContext &) { return new PredictState; }) {
    instance_->inputContextManager().registerProperty("predictState",
                                                       &factory_);
    // Notre propre dispatcher, greffé sur l'event loop de l'instance :
    // Instance::eventDispatcher() n'existe pas sur le fcitx5 des vieilles
    // distros (Ubuntu 24.04 livre 5.1.7). Cf postToMain.
    dispatcher_.attach(&instance_->eventLoop());
  }

  ~PredictEngine() override { disarmRefresh(); }

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
      fprintf(stderr,
              "[predict] sym=0x%x cp=0x%x C=%d A=%d S=%d Su=%d buf='%s' nav=%d reform=%d\n",
              sym, cp, int(states.test(fcitx::KeyState::Ctrl)),
              int(states.test(fcitx::KeyState::Alt)),
              int(states.test(fcitx::KeyState::Shift)),
              int(states.test(fcitx::KeyState::Super)), state->buffer.c_str(),
              int(state->navigating), int(state->reformulating));

    // (L) PANNEAU DE LANGUE : chips [Français|English|Auto|Libre], le choix
    // courant surligné. Toute touche gérée est CONSOMMÉE (cf mode
    // reformulation : sinon fcitx la réinjecte dans l'appli).
    if (state->langMenu) {
      if (sym == FcitxKey_Escape) {
        exitLangMenu(ic, state);
        event.filterAndAccept();
        return;
      }
      int ld = panelDigit(key, cp);
      if (!mod && ld >= 0 && ld < kNumLangChoices) {
        applyLangChoice(ic, state, ld);
        event.filterAndAccept();
        return;
      }
      // ←/→/Tab (et Ctrl+Shift+L à nouveau) : déplacer le surlignage.
      bool again = (sym == FcitxKey_l || sym == FcitxKey_L) &&
                   states.test(fcitx::KeyState::Ctrl) &&
                   states.test(fcitx::KeyState::Shift);
      if ((!mod && (sym == FcitxKey_Right || sym == FcitxKey_Tab)) || again) {
        state->langIndex = (state->langIndex + 1) % kNumLangChoices;
        setLangMenuCandidates(ic, state);
        event.filterAndAccept();
        return;
      }
      if (!mod && (sym == FcitxKey_Left || sym == FcitxKey_ISO_Left_Tab)) {
        state->langIndex =
            (state->langIndex - 1 + kNumLangChoices) % kNumLangChoices;
        setLangMenuCandidates(ic, state);
        event.filterAndAccept();
        return;
      }
      if (!mod && (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter ||
                   sym == FcitxKey_space)) {
        applyLangChoice(ic, state, state->langIndex);
        event.filterAndAccept();
        return;
      }
      exitLangMenu(ic, state); // autre touche → on sort et on continue
    }

    // (R-notice) PANNEAU D'ÉCHEC de reformulation (clé manquante/refusée,
    // API indisponible) : Entrée ouvre le dialogue de clé quand c'est une
    // affaire de clé ; Échap/Entrée sont avalées, toute autre touche ferme
    // le panneau puis continue sa vie normale.
    if (state->reformulating && !state->reformNotice.empty()) {
      bool keyIssue =
          state->reformNotice == "no_key" || state->reformNotice == "auth";
      bool enter = !mod &&
                   (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter);
      if (keyIssue && enter)
        spawnKeyDialog();
      exitReformulation(ic, state);
      if (enter || sym == FcitxKey_Escape) {
        event.filterAndAccept();
        return;
      }
      // fall-through : la touche est traitée normalement ci-dessous
    }

    // (R) MODE REFORMULATION : la barre montre les variantes de la sélection ;
    // 1-9 ou Tab/flèches+Entrée REMPLACE la sélection, ←/→ ou r/f/s/c/t change
    // de mode, Échap annule. Toute autre touche quitte le mode et retombe
    // dans la saisie normale.
    if (state->reformulating) {
      // IMPORTANT : toute touche qu'on GÈRE doit être CONSOMMÉE
      // (filterAndAccept), sinon fcitx la réinjecte dans l'appli — Tab tapait
      // une tabulation PAR-DESSUS la sélection (« grand blanc qui écrase le
      // texte »), les chiffres/Entrée fuyaient aussi.
      if (sym == FcitxKey_Escape) {
        exitReformulation(ic, state);
        event.filterAndAccept();
        return;
      }
      if (state->reformLoading) { // génération en cours → on avale tout (sauf Échap)
        event.filterAndAccept();
        return;
      }
      // RÉGÉNÉRER : re-presser Ctrl+Alt+R → nouvelles variantes (même mode).
      if (states.test(fcitx::KeyState::Ctrl) &&
          states.test(fcitx::KeyState::Alt) &&
          (sym == FcitxKey_r || sym == FcitxKey_R)) {
        state->reformNonce++;
        runReformulation(ic, state);
        event.filterAndAccept();
        return;
      }
      int n = (int)state->cands.size();
      int rd = panelDigit(key, cp);
      if (!mod && rd >= 0 && rd < n) {
        commitReformulation(ic, state, rd);
        event.filterAndAccept();
        return;
      }
      // Raccourcis DIRECTS de mode : r/f/s/c/t sautent au mode (Reformuler/
      // Formel/Simple/Corriger/Traduire ; « Court » reste accessible aux
      // ←/→ — 'c' est pris par Corriger). Même mode → no-op (avalé).
      if (!mod && cp) {
        uint32_t lc = cp | 0x20; // tolère la majuscule
        const char *k = lc == 'r'   ? "rephrase"
                        : lc == 'f' ? "formal"
                        : lc == 's' ? "simple"
                        : lc == 'c' ? "correct"
                        : lc == 't' ? "translate"
                                    : nullptr;
        if (k) {
          for (int m = 0; m < kNumReformModes; m++)
            if (std::string(kReformModes[m].key) == k && m != state->reformMode) {
              state->reformMode = m;
              state->reformNonce = 0; // nouveau mode → repart du 1er tirage
              runReformulation(ic, state);
              break;
            }
          event.filterAndAccept();
          return;
        }
      }
      // ←/→ : changer de MODE (régénère dans le nouveau mode).
      if (!mod && (sym == FcitxKey_Right || sym == FcitxKey_Left)) {
        int d = (sym == FcitxKey_Right) ? 1 : -1;
        state->reformMode =
            (state->reformMode + d + kNumReformModes) % kNumReformModes;
        state->reformNonce = 0; // nouveau mode → repart du 1er tirage
        runReformulation(ic, state);
        event.filterAndAccept();
        return;
      }
      // bulle VERTICALE → ↓/Tab = variante suivante, ↑/⇧Tab = précédente.
      if (!mod && (sym == FcitxKey_Tab || sym == FcitxKey_Down) && n) {
        state->navIndex = (state->navIndex + 1) % n;
        setReformulationCandidates(ic, state);
        event.filterAndAccept();
        return;
      }
      if (!mod && (sym == FcitxKey_ISO_Left_Tab || sym == FcitxKey_Up) && n) {
        state->navIndex = (state->navIndex - 1 + n) % n;
        setReformulationCandidates(ic, state);
        event.filterAndAccept();
        return;
      }
      if (!mod && (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)) {
        commitReformulation(ic, state, state->navIndex);
        event.filterAndAccept();
        return;
      }
      exitReformulation(ic, state); // autre touche → on sort et on continue
    }

    // (R-trig) DÉCLENCHEUR : Ctrl+Alt+R sur une SÉLECTION → 3 reformulations.
    // NB: sur AZERTY/Wayland, Ctrl+lettre remonte le keysym en MAJUSCULE
    // (FcitxKey_R, pas FcitxKey_r) → on accepte les deux casses.
    if (states.test(fcitx::KeyState::Ctrl) &&
        states.test(fcitx::KeyState::Alt) &&
        (sym == FcitxKey_r || sym == FcitxKey_R)) {
      enterReformulation(ic, state); // si pas de sélection/variantes : no-op
      event.filterAndAccept();       // consomme le raccourci dans tous les cas
      return;
    }

    // (0-bis) REVERT REFORMULATION : Backspace IMMÉDIATEMENT après avoir choisi
    // une reformulation efface la variante committée et restaure le texte
    // original. One-shot (ne dure qu'une touche), façon undo.
    uint32_t reformRevCps = state->reformRevertCps;
    state->reformRevertCps = 0;
    if (sym == FcitxKey_BackSpace && !mod && state->buffer.empty() &&
        reformRevCps > 0 &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      ic->deleteSurroundingText(-int(reformRevCps), reformRevCps);
      ic->commitString(state->reformRevertOrig);
      event.filterAndAccept();
      return;
    }

    // (0-emoji) Super+; : ouvre le PICKER EMOJI, TOUJOURS disponible — buffer
    //      vide, en plein mot, barre mot-suivant ouverte. Remplace l'ancien
    //      déclencheur ':' tapé (qui redevient un caractère normal : « 10:30 »,
    //      « voici : »). Le mot en cours est committé tel quel, SANS apprendre
    //      (fragment tapé, pas un mot validé). Re-presser referme le picker.
    //      NB: sur AZERTY, ';' est en Shift+, → le keysym peut remonter en
    //      ':' ; on accepte les deux.
    if ((sym == FcitxKey_semicolon || sym == FcitxKey_colon) &&
        states.test(fcitx::KeyState::Super)) {
      if (!state->buffer.empty() && state->buffer[0] == ':') {
        state->buffer.clear();
        state->navigating = false;
        clearPanel(ic);
      } else {
        if (!state->buffer.empty())
          commitWord(ic, state, state->buffer, /*trailingSpace=*/false,
                     /*learn=*/false);
        state->buffer = ":";
        state->navigating = false;
        updateCompletion(ic, state);
      }
      event.filterAndAccept();
      return;
    }

    // (0-) Ctrl+Shift+L : ouvre le PANNEAU DE LANGUE (chips compactes, choix
    //      courant surligné — cf bloc (L) plus haut pour la navigation).
    if ((sym == FcitxKey_l || sym == FcitxKey_L) &&
        states.test(fcitx::KeyState::Ctrl) &&
        states.test(fcitx::KeyState::Shift)) {
      enterLangMenu(ic, state);
      event.filterAndAccept();
      return;
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
      deleteSurroundingBefore(ic, autoCps);
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

    // (2bis) Backspace AVEC BUFFER VIDE : on supprime du texte DÉJÀ committé.
    //        Si la suppression « rentre » dans un mot (fin de mot juste avant
    //        le curseur — cas typique : « deman ␣ » puis ⌫ efface l'espace),
    //        on RECOMPOSE : le mot repasse en préedit et la barre revient,
    //        contexte intact — sinon revenir sur un mot déjà committé ne
    //        proposait plus rien. Même patron que le revert (0) :
    //        deleteSurroundingText + buffer + updateCompletion.
    if (sym == FcitxKey_BackSpace && !mod && state->buffer.empty() &&
        ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      const auto &st = ic->surroundingText();
      auto cps = decodeUtf8(st.text());
      size_t cur = st.cursor();
      auto isWordCp = [](uint32_t c) {
        return isLetterCp(c) || c == '\'' || c == 0x2019 || c == '-';
      };
      // pas de sélection, curseur en FIN de mot (jamais en plein milieu —
      // recomposer la moitié gauche corromprait le texte au commit suivant)
      if (st.anchor() == cur && cur > 0 && cur <= cps.size() &&
          (cur == cps.size() || !isWordCp(cps[cur]))) {
        size_t end = cur - 1; // état après le Backspace simulé
        size_t start = end;
        while (start > 0 && isWordCp(cps[start - 1]))
          --start;
        bool hasLetter = false;
        for (size_t i = start; i < end; i++)
          hasLetter = hasLetter || isLetterCp(cps[i]);
        // ponytail: cap à 32 cp — un token géant (URL…) ne se recompose pas
        if (hasLetter && end - start <= 32) {
          std::string word;
          for (size_t i = start; i < end; i++)
            appendCp(word, cps[i]);
          deleteSurroundingBefore(ic, cur - start);
          state->buffer = word;
          // le mot recomposé n'est plus committé — mais seulement s'il est
          // bien le dernier du contexte (on peut backspacer un VIEUX mot)
          if (!state->ctx.empty() && state->ctx.back().rfind(word, 0) == 0)
            state->ctx.pop_back();
          state->navigating = false;
          updateCompletion(ic, state);
          event.filterAndAccept();
          return;
        }
      }
    }
    //        Sinon : Backspace simple (un caractère) ou Ctrl+Backspace (un
    //        MOT). La barre mot-suivant est spéculative : on la FERME et on
    //        laisse la touche filer à l'application (pas de filterAndAccept).
    //        Sans cette branche, Ctrl+Backspace (mod) tombait dans « (4) if
    //        (mod) return » et la barre restait ouverte même une fois l'input
    //        entièrement vidé.
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
      if (!mod && emojiGrid && (sym == FcitxKey_Down || sym == FcitxKey_Up)) {
        navigate(ic, state, sym == FcitxKey_Down ? +8 : -8, /*clamp=*/true);
        event.filterAndAccept();
        return;
      }
      // Début/Fin : première / dernière case de la grille (sauter au bout sans
      // marteler la flèche). Hors grille elles filent à l'application.
      if (!mod && emojiGrid &&
          (sym == FcitxKey_Home || sym == FcitxKey_End)) {
        int sz = (int)state->cands.size();
        navigateTo(ic, state, sym == FcitxKey_Home ? 0 : sz - 1);
        event.filterAndAccept();
        return;
      }
      // ←/→ naviguent aussi ; en GRILLE emoji ils ENTRENT directement (pas
      // besoin de Tab d'abord — sans ça ils committaient le littéral ':xyz'
      // et la touche fuyait vers l'application) et se BORNENT aux extrémités.
      if (!mod && (state->navigating || emojiGrid) &&
          (sym == FcitxKey_Left || sym == FcitxKey_Right)) {
        navigate(ic, state, sym == FcitxKey_Right ? +1 : -1, emojiGrid);
        event.filterAndAccept();
        return;
      }
      // → ACCEPTE le texte fantôme (accept explicite, façon Copilot/fish) :
      // committe la complétion SANS espace — la frappe continue naturellement.
      // Seulement quand le fantôme est réellement AFFICHÉ (mêmes conditions
      // que updateCompletion) ; sinon → committe le littéral et file à l'app
      // (branche « toute autre touche » plus bas), comme avant.
      if (!mod && sym == FcitxKey_Right && !state->navigating &&
          engineCfg().ghostText && !state->literalIsWord &&
          !state->vetoAuto &&
          state->ghost.size() > state->buffer.size() &&
          state->ghost.compare(0, state->buffer.size(), state->buffer) == 0) {
        commitWord(ic, state, state->ghost, /*trailingSpace=*/false);
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
        } else if (isTriggerBuffer(state->buffer) && !state->cands.empty() &&
                   (state->buffer.size() > 1 || isEmojiBuffer(state->buffer))) {
          // Picker emoji (même SANS requête : la grille montre les favoris) et
          // snippet AVEC requête : Entrée prend le 1er candidat (comme
          // l'Espace, mais sans espace final). Le snippet nu (';' seul) garde
          // le comportement littéral — Entrée = retour-ligne.
          commitWord(ic, state, state->cands[0], /*space=*/false);
          event.filterAndAccept();
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
    state->ghost.clear();
    state->accentOnly = false;
    state->nextWordGen++; // un refresh neural en vol devient périmé
    clearPanel(ic);
  }

private:
  // ---- Refresh mot-suivant asynchrone (E5) --------------------------------
  // Une seule connexion en attente à la fois (un clavier, un curseur) : armer
  // remplace/ferme la précédente. Le fd est surveillé depuis la boucle
  // d'événements de fcitx — rien ne bloque jamais le thread clavier.
  void disarmRefresh() {
    if (refreshWatch_)
      refreshWatch_->setEnabled(false);
    if (refreshFd_ >= 0) {
      ::close(refreshFd_);
      refreshFd_ = -1;
    }
    refreshBuf_.clear();
  }

  void armRefresh(int fd, fcitx::InputContext *ic, PredictState *state) {
    disarmRefresh();
    refreshWatch_.reset(); // l'ancienne source (désactivée) peut mourir ici
    refreshFd_ = fd;
    refreshUUID_ = ic->uuid();
    refreshGen_ = state->nextWordGen;
    refreshWatch_ = instance_->eventLoop().addIOEvent(
        fd, fcitx::IOEventFlags{fcitx::IOEventFlag::In},
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
          onRefreshReadable();
          return true;
        });
  }

  void onRefreshReadable() {
    if (refreshFd_ < 0)
      return;
    char tmp[4096];
    ssize_t n;
    while ((n = ::read(refreshFd_, tmp, sizeof(tmp))) > 0)
      refreshBuf_.append(tmp, n);
    size_t nl = refreshBuf_.find('\n');
    if (nl == std::string::npos) {
      // EOF/erreur sans ligne complète (daemon redémarré…) : on désarme.
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        disarmRefresh();
      return;
    }
    std::string line = refreshBuf_.substr(0, nl);
    disarmRefresh(); // single-shot : une ligne = un refresh
    std::vector<std::string> cands;
    try {
      json resp = json::parse(line);
      if (!resp.value("refresh", false))
        return;
      for (auto &c : resp.value("candidates", json::array()))
        cands.push_back(c.get<std::string>());
    } catch (...) {
      return;
    }
    if (cands.empty())
      return;
    auto *ic = instance_->inputContextManager().findByUUID(refreshUUID_);
    if (!ic || !ic->hasFocus())
      return;
    auto *state = ic->propertyFor(&factory_);
    // Périmé si quoi que ce soit a bougé depuis l'armement (frappe, commit,
    // navigation, reset) — la barre affichée doit toujours refléter l'état.
    if (state->nextWordGen != refreshGen_ || !state->buffer.empty() ||
        state->navigating)
      return;
    state->cands = std::move(cands);
    state->navIndex = 0;
    setCandidates(ic, state);
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }
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

  // Contexte LARGE pour le prédicteur neuronal : le texte BRUT avant le
  // curseur (casse, ponctuation, phrases précédentes — tout ce qu'un LLM
  // exploite et que le contexte n-gram borné à la phrase jette). ~240
  // caractères, coupés à un début de mot. Repli sans SurroundingText : les
  // derniers mots committés.
  std::string wideTextFor(fcitx::InputContext *ic, PredictState *state) {
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      auto cps = decodeUtf8(ic->surroundingText().text());
      unsigned int cur = ic->surroundingText().cursor();
      if (cur < cps.size())
        cps.resize(cur);
      size_t from = cps.size() > 240 ? cps.size() - 240 : 0;
      if (from > 0) // ne pas démarrer en plein mot
        while (from < cps.size() && isLetterCp(cps[from]))
          ++from;
      std::string out;
      for (size_t i = from; i < cps.size(); i++)
        appendCp(out, cps[i]);
      if (!out.empty())
        return out;
    }
    std::string out;
    for (const auto &w : state->ctx) {
      if (!out.empty())
        out += ' ';
      out += w;
    }
    return out;
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
    // Une RESTAURATION D'ACCENTS (fold-equal : francais→français) s'applique
    // même si le tapé est un vrai mot du corpus — elle ne change jamais le mot.
    if (!state->autocomplete.empty() &&
        (!state->literalIsWord || state->accentOnly))
      return state->autocomplete;
    return state->buffer;
  }

  // Surligne un candidat. dir : +1 suivant, -1 précédent, 0 (1er appui) → le
  // 1er. Premier appui en ARRIÈRE (⇧Tab/↑) → on entre par la droite (dernier).
  // On calcule l'index nous-mêmes (robuste, indépendant de nextCandidate()).
  // `clamp` : borne au lieu de boucler — c'est le mode GRILLE (emoji). Dans une
  // grille, boucler désoriente : ↓ sur la dernière ligne renvoyait en haut, →
  // sur la dernière case revenait à la première. Borné, une flèche qui ne peut
  // plus avancer ne bouge pas ; ↓ depuis une ligne incomplète tombe sur la
  // DERNIÈRE case (comportement des grilles emoji système).
  void navigate(fcitx::InputContext *ic, PredictState *state, int dir,
                bool clamp = false) {
    auto list = ic->inputPanel().candidateList();
    if (!list || list->size() == 0)
      return;
    int sz = list->size();
    int next = state->navigating ? state->navIndex + dir
                                 : (dir < 0 ? sz - 1 : 0);
    next = clamp ? std::max(0, std::min(next, sz - 1))
                 : ((next % sz) + sz) % sz;
    state->navigating = true;
    state->navIndex = next;
    if (auto *cl = dynamic_cast<PredictCandidateList *>(list.get()))
      cl->setCursorIndex(next);
    // reflète le candidat surligné dans la préédition (mode complétion) —
    // SAUF en mode emoji : le préedit CLIENT reste vide (la requête vit dans
    // le champ de recherche du panneau, cf updateCompletion).
    if (!state->buffer.empty() && !isEmojiBuffer(state->buffer) &&
        next < (int)state->cands.size()) {
      std::string shown = applyCase(state->cands[next], state->buffer);
      fcitx::Text preedit(shown, fcitx::TextFormatFlag::Underline);
      preedit.setCursor(shown.size());
      ic->inputPanel().setClientPreedit(preedit);
      ic->updatePreedit();
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // Place le surlignage sur un index ABSOLU (Début/Fin de la grille).
  void navigateTo(fcitx::InputContext *ic, PredictState *state, int index) {
    auto list = ic->inputPanel().candidateList();
    if (!list || list->size() == 0)
      return;
    state->navigating = true; // → navigate(0) repart de `index` exactement
    state->navIndex = index;
    navigate(ic, state, 0, /*clamp=*/true);
  }

  // Valide un mot : applique la casse du buffer, committe, apprend (sauf
  // annulation), met à jour le contexte, puis (si espace) propose le suivant.
  // Gère aussi les candidats MULTI-MOTS (« sais pas » : apprentissage et
  // contexte mot à mot) et l'auto-majuscule de début de phrase (opt-in).
  void commitWord(fcitx::InputContext *ic, PredictState *state,
                  const std::string &raw, bool trailingSpace,
                  bool learn = true) {
    // Picker emoji : committer le LITTÉRAL (Échap, Entrée nue, ponctuation,
    // Espace sans candidat…) cracherait ':requête' dans le texte alors que ce
    // ':' vient du raccourci Super+;, pas de la frappe. Un picker qu'on annule
    // se ferme, point — c'est un menu, pas une composition.
    if (isEmojiBuffer(state->buffer) && raw == state->buffer) {
      state->buffer.clear();
      state->cands.clear();
      state->navigating = false;
      clearPanel(ic);
      return;
    }
    state->nextWordGen++; // le commit invalide tout refresh en vol
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
    state->nextWordGen++; // la frappe invalide tout refresh mot-suivant en vol
    ic->inputPanel().reset();
    if (state->buffer.empty()) {
      showNextWord(ic, state);
      return;
    }
    auto reply = queryDaemon(contextFor(ic, state), state->buffer,
                             wideTextFor(ic, state));
    state->cands = reply.candidates;
    state->literalIsWord = reply.literalIsWord;
    state->autocomplete = reply.autocomplete;
    state->ghost = reply.ghost;
    state->accentOnly = reply.accentOnly;
    // Repli « le brut » : le mot tapé reste proposable quand le modèle ne rend
    // rien. PAS pour le picker emoji — proposer ':zzz' comme candidat n'a aucun
    // sens ; la grille affiche son état vide (cf panelview).
    if (state->cands.empty() && !isEmojiBuffer(state->buffer))
      state->cands.push_back(state->buffer);

    // PICKER EMOJI : la requête n'est PAS du texte en cours de saisie, c'est
    // une recherche. Elle ne va donc plus dans le préedit CLIENT (qui
    // l'écrivait dans l'application : « :coeur » visible en plein champ) mais
    // dans le préedit du PANNEAU — la barre QML en fait un champ de recherche,
    // et l'UI fcitx classique l'affiche au-dessus des candidats.
    if (isEmojiBuffer(state->buffer)) {
      fcitx::Text query(state->buffer);
      query.setCursor(state->buffer.size());
      ic->inputPanel().setPreedit(query);
      ic->inputPanel().setClientPreedit(fcitx::Text{});
      setCandidates(ic, state);
      ic->updatePreedit();
      ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
      return;
    }

    // Pas de filet, pas de remplacement : si l'app n'expose pas le
    // SurroundingText, le revert Backspace d'une auto-application est
    // IMPOSSIBLE — l'Espace garde alors le littéral (les candidats restent,
    // Tab choisit). Les déclencheurs ':'/';' restent explicites.
    // Opt-out : autoApplyNeedsRevert=false.
    if (engineCfg().autoApplyNeedsRevert && !isTriggerBuffer(state->buffer) &&
        !(ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
          ic->surroundingText().isValid())) {
      state->autocomplete.clear();
      state->accentOnly = false;
      // le fantôme reste : → est un accept EXPLICITE, pas besoin de revert.
    }

    // GHOST TEXT : le reste de la complétion haute-confiance s'affiche dans
    // le préedit, curseur entre le tapé et le fantôme ("bonjou‸r") — que
    // l'Espace l'applique (autoApply) ou non : → l'accepte EXPLICITEMENT.
    // Uniquement quand la complétion PROLONGE octet-à-octet la frappe
    // (jamais pour une correction floue : la barre + liseré s'en chargent).
    std::string ghost;
    if (engineCfg().ghostText && !state->literalIsWord && !state->vetoAuto &&
        state->ghost.size() > state->buffer.size() &&
        state->ghost.compare(0, state->buffer.size(), state->buffer) == 0)
      ghost = state->ghost.substr(state->buffer.size());

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
    state->ghost.clear();
    state->accentOnly = false;
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
    state->nextWordGen++; // nouvelle barre → tout refresh antérieur est périmé
    int pendingFd = -1;
    auto reply =
        queryDaemon(ctx, "", wideTextFor(ic, state),
                    engineCfg().asyncNextWord ? &pendingFd : nullptr);
    // Deux phases (E5) : la 1re réponse (n-gram, instantanée) s'affiche tout
    // de suite ; si le daemon annonce un refresh neural, la connexion reste
    // ouverte et la barre se mettra à jour depuis la boucle d'événements.
    if (pendingFd >= 0)
      armRefresh(pendingFd, ic, state);
    state->cands = reply.candidates;
    if (state->cands.empty()) {
      clearPanel(ic);
      return;
    }
    setCandidates(ic, state);
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // --- Panneau de LANGUE (Ctrl+Shift+L) : chips horizontales compactes, le
  //     choix courant surligné. 1-9/←→/Tab naviguent, Entrée/Espace applique
  //     (écrit `lang` dans config.json, rechargé à chaud), Échap annule. ---
  void setLangMenuCandidates(fcitx::InputContext *ic, PredictState *state) {
    auto list = std::make_unique<PredictCandidateList>();
    for (int i = 0; i < kNumLangChoices; i++)
      list->append(kLangChoices[i].label, /*autoApply=*/false);
    list->setCursorIndex(state->langIndex);
    ic->inputPanel().reset();
    ic->inputPanel().setAuxUp(fcitx::Text("Langue"));
    ic->inputPanel().setCandidateList(std::move(list));
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void enterLangMenu(fcitx::InputContext *ic, PredictState *state) {
    const std::string cur = readLang();
    state->langIndex = 0;
    for (int i = 0; i < kNumLangChoices; i++)
      if (cur == kLangChoices[i].value)
        state->langIndex = i;
    state->langMenu = true;
    state->navigating = false;
    setLangMenuCandidates(ic, state);
  }

  void exitLangMenu(fcitx::InputContext *ic, PredictState *state) {
    state->langMenu = false;
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    // reprendre là où on en était : complétion du mot en cours, ou barre
    // mot-suivant (dans la langue active — c'est le retour visuel du choix).
    if (!state->buffer.empty())
      updateCompletion(ic, state);
    else
      showNextWord(ic, state);
  }

  void applyLangChoice(fcitx::InputContext *ic, PredictState *state, int idx) {
    if (idx >= 0 && idx < kNumLangChoices)
      writeLang(kLangChoices[idx].value); // échec silencieux : le panneau se ferme
    exitLangMenu(ic, state);
  }

  // Lance le dialogue de saisie de clé (ime-preferences --groq-key) : une
  // fenêtre Qt où COLLER la clé fonctionne — un Ctrl+V n'atteint jamais
  // l'IME, la saisie ne peut donc pas se faire dans le panneau lui-même.
  // Double fork : pas de zombie, le dialogue survit à l'engine.
  static void spawnKeyDialog() {
    pid_t pid = ::fork();
    if (pid == 0) {
      if (::fork() == 0) {
        ::setsid();
        ::execlp("ime-preferences", "ime-preferences", "--groq-key",
                 (char *)nullptr);
        ::_exit(127);
      }
      ::_exit(0);
    }
    if (pid > 0)
      ::waitpid(pid, nullptr, 0);
  }

  // Panneau COMPACT d'échec de reformulation : un seul chip, message clair.
  // no_key/auth → Entrée ouvre le dialogue de clé ; sinon toute touche ferme.
  void showReformNotice(fcitx::InputContext *ic, PredictState *state,
                        const std::string &kind) {
    state->reformNotice = kind.empty() ? "network" : kind;
    state->reformLoading = false;
    state->navigating = false;
    std::string msg;
    if (state->reformNotice == "no_key")
      msg = "Clé API Groq requise — Entrée : configurer · Échap";
    else if (state->reformNotice == "auth")
      msg = "Clé API refusée — Entrée : reconfigurer · Échap";
    else if (state->reformNotice == "no_text")
      msg = "Rien à reformuler — sélectionnez du texte";
    else if (state->reformNotice == "bad_url")
      msg = "reformBaseUrl refusé — https requis (voir config.json)";
    else
      msg = "⚠ Reformulation indisponible (réseau/API)";
    state->cands = {msg};
    auto list = std::make_unique<PredictCandidateList>();
    list->append(msg, /*autoApply=*/false);
    list->setCursorIndex(-1);
    ic->inputPanel().reset();
    ic->inputPanel().setCandidateList(std::move(list));
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  // --- Reformulation : sélection → 3 variantes LLM, le choix remplace ---
  void setReformulationCandidates(fcitx::InputContext *ic, PredictState *state) {
    auto list = std::make_unique<PredictCandidateList>();
    // Pendant le chargement, un seul candidat « ⟳ … » SANS numéro (l'UI le
    // détecte → spinner). Sinon, chaque variante est NUMÉROTÉE (1,2,3…) : le
    // label non vide fait basculer l'UI en liste verticale lisible.
    if (state->reformLoading) {
      for (auto &v : state->cands)
        list->append(v, /*autoApply=*/false);
      ic->inputPanel().setAuxUp(fcitx::Text()); // QML affiche « Reformulation… »
    } else {
      int i = 1;
      for (auto &v : state->cands)
        list->appendLabeled(v, std::to_string(i++)); // verbatim (phrases)
      // Header de la bulle (auxUp, lu par qmlui) : mode courant + badge source.
      std::string badge = state->reformSource == "groq"   ? "  ⚡ Groq"
                          : state->reformSource == "local" ? "  ◆ local"
                                                           : "";
      std::string header =
          std::string(kReformModes[state->reformMode].label) + badge +
          "   · ←→/rfsct mode";
      ic->inputPanel().setAuxUp(fcitx::Text(header));
    }
    list->setCursorIndex(state->navIndex); // variante surlignée
    ic->inputPanel().setCandidateList(std::move(list));
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void enterReformulation(fcitx::InputContext *ic, PredictState *state) {
    bool cap = ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText);
    bool valid = cap && ic->surroundingText().isValid();
    std::string sel = valid ? ic->surroundingText().selectedText() : std::string{};
    // Texte à reformuler :
    //  - sélection rapportée (souris) → la sélection.
    //  - PAS de sélection rapportée mais surrounding text court (ex : Ctrl+A que
    //    l'app n'expose pas comme sélection à l'IME) → TOUT le surrounding text.
    //    Le remplacement repose sur commitString (cf commitReformulation) qui
    //    remplace la sélection active de l'app — active dans les deux cas.
    std::string sentence;
    if (!sel.empty()) {
      sentence = sel;
    } else if (cap && valid) {
      std::string full = ic->surroundingText().text();
      if (!full.empty() && decodeUtf8(full).size() <= 400) // champ court, pas un doc
        sentence = full;
    }
    if (::getenv("IME_DEBUG"))
      fprintf(stderr,
              "[reform-enter] cap=%d valid=%d selLen=%zu src='%.60s'\n",
              int(cap), int(valid), sel.size(), sentence.c_str());
    if (sentence.empty()) {
      // FEEDBACK au lieu d'un no-op silencieux : panneau compact fermé par
      // n'importe quelle touche (patron R-notice).
      state->reformulating = true;
      showReformNotice(ic, state, "no_text");
      return;
    }
    state->reformulating = true;
    state->reformText = sentence;
    state->reformFromSelection = !sel.empty();
    state->reformMode = lastReformMode_; // dernier mode utilisé mémorisé
    state->reformNonce = 0;
    state->reformSource.clear();
    runReformulation(ic, state);
  }

  // Lance (ou relance) la génération pour le texte/mode/nonce courants. Feedback
  // IMMÉDIAT : placeholder + spinner, génération dans un THREAD, résultat reposté
  // sur le thread principal via l'eventDispatcher. Un compteur de génération
  // (reformGen) ignore les résultats obsolètes quand on change vite de mode.
  void runReformulation(fcitx::InputContext *ic, PredictState *state) {
    state->reformLoading = true;
    state->navigating = false;
    state->navIndex = 0;
    state->cands = {"⟳ Reformulation…"};
    setReformulationCandidates(ic, state);

    uint32_t gen = ++state->reformGen;
    lastReformMode_ = state->reformMode; // mémorise pour le prochain Ctrl+Alt+R
    auto ref = ic->watch();
    std::string text = state->reformText;
    std::string mode = kReformModes[state->reformMode].key;
    uint32_t nonce = state->reformNonce;
    int n = engineCfg().reformCount;
    std::thread([this, ref, text, mode, nonce, n, gen]() mutable {
      // STREAMING : chaque ligne partielle remplace le spinner par les
      // variantes déjà prêtes — navigables tout de suite, la liste se
      // complète au fil des lignes. La position de navigation est préservée.
      auto onPartial = [this, ref, gen](std::vector<std::string> vars) {
        postToMain(
            ref, [this, ref, vars = std::move(vars), gen]() {
              fcitx::InputContext *ic = ref.get();
              if (!ic)
                return;
              auto *st = ic->propertyFor(&factory_);
              if (!st->reformulating || st->reformGen != gen || vars.empty())
                return;
              bool first = st->reformLoading;
              st->reformLoading = false;
              st->cands = vars;
              if (first) {
                st->navIndex = 0;
                st->navigating = true;
              }
              if (st->navIndex >= (int)st->cands.size())
                st->navIndex = 0;
              setReformulationCandidates(ic, st);
            });
      };
      ReformResult r =
          reformulateDaemon(text, mode, nonce, n, onPartial); // bloque DANS le thread
      postToMain(ref, [this, ref, r, gen]() {
        fcitx::InputContext *ic = ref.get();
        if (!ic)
          return;
        auto *st = ic->propertyFor(&factory_);
        if (!st->reformulating || st->reformGen != gen)
          return; // annulé ou résultat obsolète (mode changé entre-temps)
        bool wasLoading = st->reformLoading;
        st->reformLoading = false;
        if (r.variants.empty()) {
          // une demande plus récente est en route → on laisse sa place
          if (r.error == "superseded")
            return;
          // qualité d'abord (Groq-only) : l'échec s'AFFICHE au lieu de
          // disparaître en silence — clé manquante/refusée → Entrée ouvre
          // le dialogue de configuration, sinon message compact.
          showReformNotice(ic, st, r.error);
          return;
        }
        st->cands = r.variants;
        st->reformSource = r.source;
        // streaming : si des partielles étaient déjà affichées, on garde la
        // position de navigation (l'utilisateur explore peut-être déjà).
        if (wasLoading || st->navIndex >= (int)st->cands.size())
          st->navIndex = 0;
        st->navigating = true;
        setReformulationCandidates(ic, st);
      });
    }).detach();
  }

  void exitReformulation(fcitx::InputContext *ic, PredictState *state) {
    state->reformulating = false;
    state->reformLoading = false;
    state->navigating = false;
    state->reformText.clear();
    state->reformSource.clear();
    state->reformNotice.clear();
    // NB : on NE touche PAS à reformRevert* — le revert doit survivre au commit
    // (il s'arme dans commitReformulation et se consomme au prochain Backspace).
    state->cands.clear();
    ic->inputPanel().reset();
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void commitReformulation(fcitx::InputContext *ic, PredictState *state, int idx) {
    // Sélection RAPPORTÉE : commitString suffit (commit-over-selection) — pas
    // de deleteSurroundingText, certains toolkits double-supprimeraient
    // (sélection effacée puis delete relatif au curseur).
    // Repli « champ entier » (AUCUNE sélection rapportée) : commitString seul
    // INSÉRERAIT la variante en plus du texte — on supprime d'abord TOUT le
    // champ. Si l'app avait en fait un Ctrl+A non rapporté, la plage
    // supprimée == la sélection : le résultat reste correct.
    if (idx >= 0 && idx < (int)state->cands.size()) {
      const std::string &variant = state->cands[idx];
      if (!state->reformFromSelection &&
          ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
          ic->surroundingText().isValid()) {
        auto &st = ic->surroundingText();
        auto cps = decodeUtf8(st.text());
        size_t cur = std::min(size_t(st.cursor()), cps.size());
        if (!cps.empty()) {
          ic->deleteSurroundingText(-int(cur), cps.size());
          st.setText("", 0, 0); // copie locale (cf deleteSurroundingBefore)
        }
      }
      ic->commitString(variant);
      // arme le REVERT : Backspace juste après restaure le texte original.
      state->reformRevertOrig = state->reformText;
      state->reformRevertCps = (uint32_t)decodeUtf8(variant).size();
    }
    exitReformulation(ic, state);
  }

  void setCandidates(fcitx::InputContext *ic, PredictState *state) {
    // liste maison : jusqu'à 24 candidats (grille emoji) sans la limite de
    // 10 labels de CommonCandidateList (qui FATAL-abort fcitx au-delà).
    auto list = std::make_unique<PredictCandidateList>();
    // le candidat que l'Espace appliquera est marqué (gras → liseré dans l'UI)
    bool willAuto = !state->buffer.empty() && !state->autocomplete.empty() &&
                    !state->vetoAuto &&
                    (!state->literalIsWord || state->accentOnly);
    for (auto &w : state->cands)
      list->append(applyCase(w, state->buffer),
                   willAuto && w == state->autocomplete);
    list->setCursorIndex(-1); // aucun surlignage tant qu'on ne navigue pas
    ic->inputPanel().setCandidateList(std::move(list));
  }

  fcitx::Instance *instance_;
  // Poste un travail depuis un thread de reformulation vers le thread principal,
  // en n'exécutant que si le contexte d'entrée est toujours vivant. C'est ce que
  // fait EventDispatcher::scheduleWithContext, mais celui-ci n'existe que depuis
  // fcitx5 5.1.8 : on le refait à la main pour rester buildable sur le fcitx5
  // stock des distros plus anciennes (Ubuntu 24.04 → 5.1.7).
  void postToMain(fcitx::TrackableObjectReference<fcitx::InputContext> ref,
                  std::function<void()> fn) {
    if (!ref.isValid())
      return;
    dispatcher_.schedule([ref, fn = std::move(fn)]() {
      if (ref.isValid())
        fn();
    });
  }

  fcitx::EventDispatcher dispatcher_;
  // Dernier mode de reformulation utilisé — le prochain Ctrl+Alt+R repart de
  // là (partagé entre les contextes : préférence de session, pas de champ).
  int lastReformMode_ = 0;
  // Refresh asynchrone (E5) — au plus UNE connexion en attente.
  std::unique_ptr<fcitx::EventSourceIO> refreshWatch_;
  int refreshFd_ = -1;
  fcitx::ICUUID refreshUUID_{};
  uint64_t refreshGen_ = 0;
  std::string refreshBuf_;
  fcitx::FactoryFor<PredictState> factory_;
};

class PredictEngineFactory : public fcitx::AddonFactory {
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new PredictEngine(manager->instance());
  }
};

FCITX_ADDON_FACTORY(PredictEngineFactory)
