#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Reformulation via une API chat OpenAI-compatible (Groq par défaut, mais tout
// endpoint compatible marche). La clé est lue dans $GROQ_API_KEY, sinon dans
// le DATA dir (~/.local/share/ime-predictord/groq.key), sinon <cfgDir>/groq.key
// (jamais dans config.json). Détecte la langue de la phrase et épingle la
// sortie dans cette langue (pas de traduction). Renvoie {} en cas d'échec.
// `errKind` (optionnel) dit POURQUOI — l'engine en fait un panneau compact :
//   "ok"      variantes rendues
//   "no_key"  aucune clé trouvée → inviter l'utilisateur à en fournir une
//   "auth"    clé refusée (HTTP 401/403) → inviter à la reconfigurer
//   "network" réseau injoignable / timeout (erreur curl)
//   "http"    HTTP non-2xx (hors auth) ou réponse inexploitable
//   "empty"   HTTP 200 mais aucune variante utilisable
// mode : rephrase|formal|simple|short|correct|translate (cf reform_prompts.h).
// nonce : varie le seed → « régénérer » donne d'autres variantes.
std::vector<std::string> reformulateHttp(const std::string &sentence, int n,
                                         const std::string &baseUrl,
                                         const std::string &model,
                                         const std::string &cfgDir,
                                         long timeoutMs, const std::string &mode,
                                         uint32_t nonce,
                                         std::string *errKind = nullptr);
