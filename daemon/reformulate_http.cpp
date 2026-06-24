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

// Langue de la phrase (FR par défaut). Cf daemon/neural.cpp::isFrench — accents
// latins FR fréquents → FR certain ; sinon vote des mots vides.
bool isFrench(const std::string &s) {
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if ((unsigned char)s[i] == 0xC3) {
      unsigned char b = (unsigned char)s[i + 1];
      if (b == 0xA9 || b == 0xA8 || b == 0xAA || b == 0xAB || b == 0xA0 ||
          b == 0xA2 || b == 0xA7 || b == 0xB4 || b == 0xB6 || b == 0xB9 ||
          b == 0xBB || b == 0xBC || b == 0xAE || b == 0xAF)
        return true;
    }
  }
  std::string low;
  low.reserve(s.size() + 2);
  low += ' ';
  for (char c : s)
    low += (std::isalpha((unsigned char)c) ? (char)std::tolower((unsigned char)c)
                                           : ' ');
  low += ' ';
  static const char *fr[] = {" le ", " la ", " les ", " un ", " une ", " des ",
                             " je ", " tu ", " est ", " et ", " pour ", " que ",
                             " pas ", " vous ", " ne ", " dans ", " avec ",
                             " ce ", " sur ", " au ", " du ", " ca ", nullptr};
  static const char *en[] = {" the ", " is ", " are ", " you ", " for ", " and ",
                             " of ", " to ", " that ", " with ", " would ",
                             " like ", " this ", " it ", " on ", " be ",
                             " have ", " do ", " can ", " your ", " my ", nullptr};
  auto count = [&](const char **ws) {
    int n = 0;
    for (int i = 0; ws[i]; ++i)
      for (size_t p = low.find(ws[i]); p != std::string::npos;
           p = low.find(ws[i], p + 1))
        ++n;
    return n;
  };
  return count(fr) >= count(en);
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
                                         uint32_t nonce) {
  if (sentence.empty()) return {};
  if (n < 1) n = 3;
  std::string key = readKey(cfgDir);
  if (key.empty()) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] pas de clé (GROQ_API_KEY / groq.key) — repli local\n");
    return {}; // pas de clé → on retombe sur le local
  }

  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  // Prompt partagé avec le neural (reform_prompts.h) : mode + épinglage de
  // langue (translate inverse la langue). On sur-génère (n+1).
  bool fr = isFrench(sentence);
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
  if (!curl) return {};
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
  CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || http < 200 || http >= 300) {
    if (getenv("IME_DEBUG"))
      fprintf(stderr, "[reform-http] rc=%d (%s) http=%ld resp=%.300s\n", rc,
              curl_easy_strerror(rc), http, resp.c_str());
    return {};
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
    return {};
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
  return variants;
}
