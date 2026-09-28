// Cœur portable — client du daemon de prédiction.
//
// Le protocole (une ligne JSON par message) est INCHANGÉ depuis l'engine
// fcitx5 : seul le transport est passé par oscompat (AF_UNIX des deux côtés,
// natif sous Windows 10 1803+).
//
// Règle de vie : la connexion est BORNÉE dans le temps. Côté fcitx5 on tourne
// sur le thread clavier de la session ; côté TSF on est chargé DANS le process
// de l'application. Un daemon coincé ne doit JAMAIS geler la frappe — au pire
// on tape sans candidats.
#pragma once

#include "config.h"
#include "os_compat.h"

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace core {

struct DaemonReply {
  std::vector<std::string> candidates;
  bool literalIsWord = false;
  std::string autocomplete; // mot à appliquer sur Espace, ou ""
  std::string ghost;        // complétion affichée en fantôme
  bool accentOnly = false;  // autocomplete = pure restauration d'accents
  bool pending = false;     // un refresh neural suivra sur la même connexion
};

struct ReformResult {
  std::vector<std::string> variants;
  std::string source; // "groq" / "local" / "none" → badge du header
  // pourquoi c'est vide : no_key / auth / network / http / empty / superseded
  std::string error;
};

constexpr int kDaemonTimeoutMs = 150;

// Dernier échec de connexion au daemon. Le frontal dégrade en silence (c'est
// voulu : on ne gèle pas la frappe), donc sans ce témoin un « aucune
// suggestion » est indiscernable d'un « le daemon n'a rien trouvé ».
inline int &lastConnectError() {
  static thread_local int err = 0;
  return err;
}

inline std::string daemonSocketPath() {
  const char *env = ::getenv("IME_PREDICTORD_SOCK");
  return env && *env ? std::string(env) : oscompat::defaultSockPath();
}

// connect() non bloquant (un backlog plein = daemon suspendu → échec immédiat,
// pas d'attente), puis timeouts d'E/S.
inline sock_t connectDaemon(int timeoutMs = kDaemonTimeoutMs) {
  const std::string path = daemonSocketPath();
  sock_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (!oscompat::sockValid(fd))
    return kBadSock;
  oscompat::sockNonBlock(fd);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    oscompat::sockClose(fd);
    return kBadSock;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
    // Sous Windows, un connect() AF_UNIX NON BLOQUANT rend toujours
    // WSAEWOULDBLOCK et s'achève de façon asynchrone ; sous Linux il réussit
    // du premier coup (un socket local n'a pas de poignée de main). Traiter ce
    // code comme un échec revenait à ne JAMAIS se connecter sous Windows — le
    // text service se rabattait silencieusement sur le mot tapé.
    // On termine donc la connexion, bornée par le même budget : un daemon
    // suspendu (backlog plein) échoue toujours vite, la frappe ne gèle pas.
    if (!oscompat::sockConnectPending() ||
        !oscompat::sockFinishConnect(fd, timeoutMs)) {
      lastConnectError() = oscompat::lastSockError();
      oscompat::sockClose(fd);
      return kBadSock;
    }
  }
  lastConnectError() = 0;
  oscompat::sockBlocking(fd);
  oscompat::sockTimeouts(fd, timeoutMs);
  return fd;
}

inline bool sendLine(sock_t fd, const std::string &line) {
  size_t sent = 0;
  while (sent < line.size()) {
    ssize_t n = oscompat::sockWrite(fd, line.data() + sent, line.size() - sent);
    if (n <= 0)
      return false;
    sent += size_t(n);
  }
  return true;
}

// Lit jusqu'au premier '\n'. `carry` garde ce qui dépasse (plusieurs lignes
// peuvent arriver dans le même paquet — streaming de reformulation).
inline bool readLine(sock_t fd, std::string &carry, std::string &out) {
  for (;;) {
    size_t nl = carry.find('\n');
    if (nl != std::string::npos) {
      out = carry.substr(0, nl);
      carry.erase(0, nl + 1);
      return true;
    }
    char tmp[8192];
    ssize_t n = oscompat::sockRead(fd, tmp, sizeof(tmp));
    if (n <= 0)
      return false;
    carry.append(tmp, size_t(n));
  }
}

// pendingFd : si non-nul et que le daemon annonce "pending":true, la connexion
// reste OUVERTE et son fd est rendu à l'appelant (qui la surveille depuis son
// frontal) ; sinon elle est fermée.
inline DaemonReply queryDaemon(const std::vector<std::string> &context,
                               const std::string &prefix,
                               const std::string &wide = {},
                               sock_t *pendingFd = nullptr) {
  DaemonReply out;
  // Mot-suivant (prefix vide) = peut être neural → budget séparé, relevable.
  // Complétion intra-mot = n-gram rapide → timeout court.
  int timeoutMs = prefix.empty() ? engineCfg().nextWordTimeoutMs
                                 : engineCfg().socketTimeoutMs;
  sock_t fd = connectDaemon(timeoutMs);
  if (!oscompat::sockValid(fd))
    return out;
  nlohmann::json req;
  req["context"] = context;
  req["prefix"] = prefix;
  // Contexte LARGE (texte brut avant le curseur) : réservé au prédicteur
  // neuronal — son avantage mesuré EXIGE le contexte long.
  if (!wide.empty())
    req["wide"] = wide;
  if (pendingFd)
    req["async"] = true;
  if (!sendLine(fd, req.dump() + "\n")) {
    oscompat::sockClose(fd);
    return out;
  }
  std::string carry, line;
  if (!readLine(fd, carry, line)) {
    oscompat::sockClose(fd);
    return out;
  }
  try {
    nlohmann::json resp = nlohmann::json::parse(line);
    for (auto &c : resp.value("candidates", nlohmann::json::array()))
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
    oscompat::sockNonBlock(fd); // surveillé, donc lu sans bloquer
    *pendingFd = fd;
    return out;
  }
  oscompat::sockClose(fd);
  return out;
}

// Lit la 2e ligne (refresh neural) sur une connexion armée. Renvoie false tant
// qu'elle est incomplète ; `done` dit si la connexion est finie (EOF/erreur).
inline bool readRefresh(sock_t fd, std::string &carry,
                        std::vector<std::string> &cands, bool &done) {
  done = false;
  char tmp[4096];
  for (;;) {
    ssize_t n = oscompat::sockRead(fd, tmp, sizeof(tmp));
    if (n > 0) {
      carry.append(tmp, size_t(n));
      continue;
    }
    if (n == 0 || !oscompat::sockWouldBlock())
      done = true; // EOF ou erreur : plus rien à attendre
    break;
  }
  size_t nl = carry.find('\n');
  if (nl == std::string::npos)
    return false;
  std::string line = carry.substr(0, nl);
  carry.erase(0, nl + 1);
  try {
    nlohmann::json resp = nlohmann::json::parse(line);
    if (!resp.value("refresh", false))
      return false;
    for (auto &c : resp.value("candidates", nlohmann::json::array()))
      cands.push_back(c.get<std::string>());
  } catch (...) {
    return false;
  }
  return !cands.empty();
}

// Fire-and-forget : on n'attend pas la réponse (le daemon ignore SIGPIPE).
inline void learnDaemon(const std::string &prev, const std::string &word) {
  sock_t fd = connectDaemon();
  if (!oscompat::sockValid(fd))
    return;
  nlohmann::json req;
  req["learn"]["prev"] = prev;
  req["learn"]["word"] = word;
  sendLine(fd, req.dump() + "\n");
  oscompat::sockClose(fd);
}

// L'utilisateur a reverté un remplacement (Backspace) : le daemon ne doit plus
// jamais auto-appliquer cette paire (persisté côté daemon).
inline void vetoDaemon(const std::string &typed, const std::string &applied) {
  sock_t fd = connectDaemon();
  if (!oscompat::sockValid(fd))
    return;
  nlohmann::json req;
  req["veto"]["typed"] = typed;
  req["veto"]["applied"] = applied;
  sendLine(fd, req.dump() + "\n");
  oscompat::sockClose(fd);
}

// Reformulation : texte + MODE + NONCE (pour régénérer). Timeout LONG (≠
// prédiction par frappe) — action explicite, exécutée dans un thread.
// La réponse peut arriver en PLUSIEURS lignes : des {"variants":…,
// "partial":true} (streaming — la 1re variante s'affiche pendant que les
// suivantes se génèrent), puis la ligne FINALE qui clôt l'échange.
inline ReformResult reformulateDaemon(
    const std::string &sentence, const std::string &mode, uint32_t nonce,
    int nWant,
    const std::function<void(std::vector<std::string>)> &onPartial = nullptr) {
  ReformResult out;
  sock_t fd = connectDaemon(/*timeoutMs=*/12000);
  if (!oscompat::sockValid(fd))
    return out;
  nlohmann::json req;
  req["reformulate"] = sentence;
  req["n"] = nWant;
  req["mode"] = mode;
  req["nonce"] = nonce;
  if (!sendLine(fd, req.dump() + "\n")) {
    oscompat::sockClose(fd);
    return out;
  }
  std::string carry, line;
  while (readLine(fd, carry, line)) {
    try {
      nlohmann::json resp = nlohmann::json::parse(line);
      std::vector<std::string> vars;
      for (auto &v : resp.value("variants", nlohmann::json::array()))
        vars.push_back(v.get<std::string>());
      if (resp.value("partial", false)) {
        if (onPartial)
          onPartial(std::move(vars));
        continue;
      }
      out.variants = std::move(vars);
      out.source = resp.value("source", std::string{"none"});
      out.error = resp.value("error", std::string{});
      break;
    } catch (...) {
    }
  }
  oscompat::sockClose(fd);
  return out;
}

// Validation de clé Groq (panneau « fournir la clé ») — réponse différée.
inline ReformResult reformCheckDaemon() {
  ReformResult out;
  sock_t fd = connectDaemon(/*timeoutMs=*/8000);
  if (!oscompat::sockValid(fd))
    return out;
  if (!sendLine(fd, std::string("{\"reformCheck\":true}\n"))) {
    oscompat::sockClose(fd);
    return out;
  }
  std::string carry, line;
  if (readLine(fd, carry, line)) {
    try {
      nlohmann::json resp = nlohmann::json::parse(line);
      out.error = resp.value("error", std::string{});
      if (resp.value("keyValid", false))
        out.source = "groq";
    } catch (...) {
    }
  }
  oscompat::sockClose(fd);
  return out;
}

} // namespace core
