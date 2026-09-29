// Cœur portable — réglages côté frontal, partagés avec le daemon via le MÊME
// config.json ($XDG_CONFIG_HOME/ime-predictord sous Linux,
// %APPDATA%\ime-predictord sous Windows). Rechargé à chaud sur mtime.
#pragma once

#include "os_compat.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace core {

// Réglages lus par le frontal (l'engine fcitx5 comme le text service TSF) :
//   frenchSpacing        — espace fine insécable (U+202F) avant ; : ! ?
//   autoCapitalize       — majuscule automatique en début de phrase
//   nextWordBar          — barre mot-suivant après Espace/commit (false = calme)
//   autoApplyNeedsRevert — n'auto-applique que si l'app permet le revert
//                          Backspace (texte environnant) ; sinon Tab choisit
//   escapeForward        — Échap ferme la barre PUIS atteint l'application
//                          (vim sort du mode insertion) ; false = avalé
struct EngineCfg {
  bool frenchSpacing = false;
  bool autoCapitalize = false;
  bool nextWordBar = true;
  bool autoApplyNeedsRevert = true;
  bool escapeForward = true;
  // Timeout socket (ms). La complétion intra-mot (n-gram, <1 ms) garde
  // socketTimeoutMs → jamais de gel clavier. Le MOT-SUIVANT peut être neural
  // (~120-200 ms) : nextWordTimeoutMs le borne SÉPARÉMENT.
  int socketTimeoutMs = 150;
  int nextWordTimeoutMs = 150;
  // Deux phases (E5) : la barre mot-suivant s'affiche TOUT DE SUITE (n-gram),
  // puis se met à jour quand le neural aboutit (2e ligne sur la même
  // connexion). Sans neural côté daemon la réponse n'est jamais "pending".
  bool asyncNextWord = true;
  // Nombre de variantes de reformulation demandées (borné 1-6).
  int reformCount = 3;
  // Programmes (sous-chaînes) où la barre SPÉCULATIVE mot-suivant est
  // supprimée : dans un terminal elle n'a pas de preedit pour s'ancrer au
  // curseur et « traîne » derrière lui.
  std::vector<std::string> nextWordBarExclude;
};

inline const std::string &configPath() {
  static const std::string path = oscompat::configDir() + "/config.json";
  return path;
}

inline const EngineCfg &engineCfg() {
  static EngineCfg cfg;
  static long long stamp = -1;
  const std::string &path = configPath();
  long long t = oscompat::fileMtime(path);
  if (t != stamp) {
    stamp = t;
    EngineCfg fresh;
    std::ifstream f(path);
    if (f) {
      try {
        nlohmann::json j =
            nlohmann::json::parse(f, nullptr, true, /*ignore_comments=*/true);
        fresh.frenchSpacing = j.value("frenchSpacing", fresh.frenchSpacing);
        fresh.autoCapitalize = j.value("autoCapitalize", fresh.autoCapitalize);
        fresh.nextWordBar = j.value("nextWordBar", fresh.nextWordBar);
        fresh.autoApplyNeedsRevert =
            j.value("autoApplyNeedsRevert", fresh.autoApplyNeedsRevert);
        fresh.escapeForward = j.value("escapeForward", fresh.escapeForward);
        fresh.socketTimeoutMs =
            j.value("socketTimeoutMs", fresh.socketTimeoutMs);
        fresh.nextWordTimeoutMs =
            j.value("nextWordTimeoutMs", fresh.nextWordTimeoutMs);
        fresh.asyncNextWord = j.value("asyncNextWord", fresh.asyncNextWord);
        fresh.reformCount = (std::min)(
            6, (std::max)(1, j.value("reformCount", fresh.reformCount)));
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
// formatage et clés-commentaires préservés (pas de re-sérialisation). On écrit
// sur la CIBLE réelle (le fichier peut être un lien stow vers des dotfiles), de
// façon atomique, en poussant le mtime si besoin pour que le daemon voie la
// bascule (il recharge à la seconde près).

// Localise la valeur de "lang" dans `text` → [q1+1, q2) ; false si absente.
inline bool langValueSpan(const std::string &text, size_t &q1, size_t &q2) {
  size_t k = text.find("\"lang\"");
  if (k == std::string::npos)
    return false;
  size_t colon = text.find(':', k + 6);
  q1 = colon == std::string::npos ? colon : text.find('"', colon + 1);
  q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  return q2 != std::string::npos;
}

inline std::string readLang() {
  std::ifstream in(configPath());
  if (!in)
    return "";
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  size_t q1, q2;
  return langValueSpan(text, q1, q2) ? text.substr(q1 + 1, q2 - q1 - 1)
                                     : std::string{};
}

inline bool writeLang(const std::string &next) {
  const std::string path = oscompat::realPath(configPath());
  long long before = oscompat::fileMtime(path);
  if (before == 0)
    return false; // pas de config.json : rien à réécrire
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
  if (!oscompat::replaceFileAtomic(path, text))
    return false;
  oscompat::bumpMtimeIfUnchanged(path, before);
  return true;
}

// Panneau de LANGUE (Ctrl+Shift+L) : valeur écrite dans config.json + libellé
// des chips. Extensible : ajouter une langue = une ligne ici.
struct LangChoice {
  const char *value;
  const char *label;
};
inline const LangChoice *langChoices(int &count) {
  static const LangChoice kChoices[] = {
      {"fr", "Français"}, {"en", "English"},
      {"auto", "Auto"},   {"off", "Libre"},
  };
  count = int(sizeof(kChoices) / sizeof(kChoices[0]));
  return kChoices;
}

// Modes de reformulation : clé envoyée au daemon (cf reform_prompts.h) +
// libellé affiché dans le header de la bulle. ←/→ cyclent dans cette liste.
struct ReformMode {
  const char *key;
  const char *label;
};
inline const ReformMode *reformModes(int &count) {
  static const ReformMode kModes[] = {
      {"rephrase", "Reformuler"}, {"formal", "Formel"},
      {"simple", "Simple"},       {"short", "Court"},
      {"correct", "Corriger"},    {"translate", "Traduire"},
  };
  count = int(sizeof(kModes) / sizeof(kModes[0]));
  return kModes;
}

} // namespace core
