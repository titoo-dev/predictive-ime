#include "reformulate_http.h"
#include "reform_prompts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// UTF-8 valide : un fragment incomplet casserait json::dump côté appelant.
bool validUtf8(const std::string &s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
              : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + (size_t)len > n) return false;
    for (int k = 1; k < len; ++k)
      if ((((unsigned char)s[i + k]) >> 6) != 0x2) return false;
    i += len;
  }
  return true;
}

std::string trimmed(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t z = s.find_last_not_of(" \t\r\n");
  return s.substr(a, z - a + 1);
}

// Clé API (vide si absente). Ordre : $GROQ_API_KEY, puis le DATA dir
// (~/.local/share/ime-predictord/groq.key — non versionné, comme user.log),
// puis <cfgDir>/groq.key en dernier recours. IMPORTANT : le cfgDir est souvent
// un symlink stow vers un dépôt git → on n'y met PAS le secret par défaut.
std::string readKey(const std::string &cfgDir) {
  const char *env = getenv("GROQ_API_KEY");
  if (env && *env) return trimmed(env);
  std::vector<std::string> paths;
  const char *xdgData = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  if (xdgData && *xdgData)
    paths.push_back(std::string(xdgData) + "/ime-predictord/groq.key");
  else if (home)
    paths.push_back(std::string(home) + "/.local/share/ime-predictord/groq.key");
  if (!cfgDir.empty())
    paths.push_back(cfgDir + "/groq.key"); // dernier recours (à gitignore)
  for (const auto &p : paths) {
    std::ifstream f(p);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string k = trimmed(ss.str());
    if (!k.empty()) return k;
  }
  return {};
}

// Le token Groq part dans un header Authorization, et reformBaseUrl est
// rechargé à chaud depuis config.json : un baseUrl en clair enverrait la clé en
// cleartext. On exige donc https, sauf sur la boucle locale (le mock HTTP des
// tests écoute sur 127.0.0.1). Le parsing est délégué à curl — même analyseur
// que celui qui fera la requête, donc pas de divergence exploitable entre le
// host qu'on valide et celui qu'on contacte.
bool urlSafeForToken(const std::string &url) {
  CURLU *h = curl_url();
  if (!h) return false;
  bool ok = false;
  if (curl_url_set(h, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
    char *scheme = nullptr, *host = nullptr;
    curl_url_get(h, CURLUPART_SCHEME, &scheme, 0);
    curl_url_get(h, CURLUPART_HOST, &host, 0);
    std::string s = scheme ? scheme : "", ho = host ? host : "";
    ok = s == "https" || (s == "http" && (ho == "127.0.0.1" ||
                                          ho == "localhost" || ho == "::1"));
    curl_free(scheme);
    curl_free(host);
  }
  curl_url_cleanup(h);
  return ok;
}

size_t writeCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// minuscule ASCII + suppression des espaces (dédup + comparaison à la source)
std::string norm(const std::string &x) {
  std::string r;
  r.reserve(x.size());
  for (char ch : x)
    if (!std::isspace((unsigned char)ch))
      r += (char)std::tolower((unsigned char)ch);
  return r;
}

} // namespace

std::vector<std::string> reformulateHttp(const std::string &sentence, int n,
                                         const std::string &baseUrl,
                                         const std::string &model,
                                         const std::string &cfgDir,
                                         long timeoutMs, const std::string &mode,
                                         uint32_t nonce, const std::string &lang,
                                         std::string *errKind) {
  auto fail = [&](const char *kind) -> std::vector<std::string> {
    if (errKind)
      *errKind = kind;
    return {};
  };
  if (errKind)
    *errKind = "ok";
  if (sentence.empty()) return fail("empty");
  if (n < 1) n = 3;

  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  // Vérifié AVANT de lire la clé : une URL non fiable ne doit même pas faire
  // toucher au secret sur le disque.
  if (!urlSafeForToken(baseUrl)) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] baseUrl refusé (https requis hors "
                      "boucle locale) : %.200s\n", baseUrl.c_str());
    return fail("bad_url");
  }

  std::string key = readKey(cfgDir);
  if (key.empty()) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] pas de clé (GROQ_API_KEY / groq.key)\n");
    return fail("no_key");
  }

  // Prompt partagé avec le neural (reform_prompts.h) : mode + épinglage de
  // langue (translate inverse la langue). La langue CHOISIE (cfg.lang
  // "fr"/"en") prime ; l'heuristique ne sert qu'en "auto". Sur-génère (n+1).
  bool fr = lang == "fr" || (lang != "en" && reformIsFrench(sentence));
  int want = n + 1;
  std::string sys = reformSystemPrompt(mode, fr, want);

  json body = {
      {"model", model},
      {"temperature", 0.7},
      // seed = hash(phrase) ^ nonce : « régénérer » (nonce++) → autres variantes.
      {"seed", (int)((uint32_t)std::hash<std::string>{}(sentence) ^ nonce)},
      {"max_tokens", 600},
      {"messages",
       json::array({{{"role", "system"}, {"content", sys}},
                    {{"role", "user"}, {"content", sentence}}})},
  };
  std::string payload = body.dump();

  CURL *curl = curl_easy_init();
  if (!curl) return fail("network");
  std::string resp;
  curl_slist *hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  std::string auth = "Authorization: Bearer " + key;
  hdrs = curl_slist_append(hdrs, auth.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, baseUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs > 0 ? timeoutMs : 20000);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // sûr en contexte multi-thread
  // FOLLOWLOCATION reste à 0 (défaut) : une redirection pourrait rejouer le
  // header Authorization vers un autre hôte/schéma. Si on l'active un jour,
  // ajouter CURLOPT_REDIR_PROTOCOLS_STR "https".
  CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || http < 200 || http >= 300) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] rc=%d (%s) http=%ld resp=%.300s\n", rc,
              curl_easy_strerror(rc), http, resp.c_str());
    if (rc != CURLE_OK)
      return fail("network"); // injoignable / timeout
    return fail(http == 401 || http == 403 ? "auth" : "http");
  }

  std::string content;
  try {
    json r = json::parse(resp);
    content =
        r.at("choices").at(0).at("message").at("content").get<std::string>();
  } catch (const std::exception &e) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] parse KO: %s resp=%.300s\n", e.what(),
              resp.c_str());
    return fail("http");
  }
  if (getenv("IME_DEBUG"))
    fprintf(stderr, "[reform-http] lang=%s http=%ld content=\"%.300s\"\n",
            fr ? "fr" : "en", http, content.c_str());

  // une variante par ligne : trim numérotation/ponctuation de tête, dédup
  // (casse-insensible) et drop de la variante identique à la source.
  std::string srcN = norm(sentence);
  std::vector<std::string> variants, seen;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line) && (int)variants.size() < n) {
    size_t a = line.find_first_not_of(" \t\r\"'-*•0123456789.)(");
    if (a == std::string::npos) continue;
    size_t z = line.find_last_not_of(" \t\r\"");
    std::string v = line.substr(a, z - a + 1);
    if (v.empty() || !validUtf8(v)) continue;
    std::string vN = norm(v);
    if (vN == srcN) continue;
    bool dup = false;
    for (auto &s : seen)
      if (s == vN) { dup = true; break; }
    if (dup) continue;
    seen.push_back(vN);
    variants.push_back(v);
  }
  if (variants.empty())
    return fail("empty");
  return variants;
}
