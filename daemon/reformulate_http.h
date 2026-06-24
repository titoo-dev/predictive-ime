#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Reformulation via une API chat OpenAI-compatible (Groq par défaut, mais tout
// endpoint compatible marche). La clé est lue dans $GROQ_API_KEY, sinon dans
// <cfgDir>/groq.key (jamais dans config.json). Détecte la langue de la phrase
// et épingle la sortie dans cette langue (pas de traduction). Renvoie {} en cas
// d'échec quel qu'il soit (pas de clé, réseau, HTTP non-2xx, JSON invalide) —
// l'appelant retombe alors sur le prédicteur local (neural).
// mode : rephrase|formal|simple|short|correct|translate (cf reform_prompts.h).
// nonce : varie le seed → « régénérer » donne d'autres variantes.
std::vector<std::string> reformulateHttp(const std::string &sentence, int n,
                                         const std::string &baseUrl,
                                         const std::string &model,
                                         const std::string &cfgDir,
                                         long timeoutMs, const std::string &mode,
                                         uint32_t nonce);
