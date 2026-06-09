// Track "câblage" + cerveau — daemon de prédiction (patron fcitx5-azookeyd).
//
// Découplé de l'addon fcitx5 : écoute sur un socket Unix, reçoit une requête
// JSON (préfixe en cours + contexte de mots), renvoie une liste de candidats.
//
// Cerveau v3 — modèle KNESER-NEY INTERPOLÉ précalculé (cf build_ngrams.py),
// toujours CPU-only et en lookups O(1) :
//   - mot-suivant et complétion scorés par P_KN(w | u,v) avec repli exact :
//       P(w|u,v) = p3(uvw) stocké, sinon γ3(uv)·P(w|v)
//       P(w|v)   = p2(vw) stocké,  sinon γ2(v)·P1(w)
//       P1(w)    = mélange continuation KN + fréquence brute (couverture)
//   - repli ACCENT-INSENSIBLE : tu tapes "francais"/"etre" → "français"/"être".
//   - AUTOCORRECTION en NOISY-CHANNEL : P(mot|ctx)·P(faute|mot), le canal
//     pondéré par type de faute (transposition > voisin AZERTY > lettre en
//     trop) — plus de pénalité plate arbitraire.
//   - apprentissage utilisateur prioritaire + persistant.
//   - signale `literalIsWord` : l'engine n'écrase un mot réellement tapé que
//     sur sélection explicite (jamais d'autocorrection d'un mot déjà valide).
//
// Protocole (une ligne JSON par message, '\n' terminé) :
//   <- {"context":["je"],"prefix":"v"}
//   -> {"candidates":["veux","vais","vous",...],"literalIsWord":false}
//   <- {"learn":{"prev":"je","word":"code"}}    -> {"ok":true}
//
// Run: predictord <words.tsv> [socket]
//   fichiers voisins chargés s'ils existent : bigrams.tsv, bigrams.bo.tsv,
//   trigrams.tsv, trigrams.bo.tsv, pcont.tsv (format build_ngrams.py).
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// ------------------------------------------------------------------ UTF-8 ----
namespace {

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

// Repli de recherche : minuscule + suppression des diacritiques latins. C'est la
// clé d'index pour matcher un préfixe tapé sans accent contre le vocabulaire
// accentué ("etre" → "être", "francais" → "français").
std::string foldStr(const std::string &in) {
  std::string out;
  for (uint32_t cp : decodeUtf8(in)) {
    if (cp >= 0x300 && cp <= 0x36F)
      continue; // marques combinantes
    if (cp >= 'A' && cp <= 'Z') {
      out.push_back(char(cp + 32));
      continue;
    }
    if (cp < 0x80) {
      out.push_back(char(cp));
      continue;
    }
    switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
      out.push_back('a'); break;
    case 0xC6: case 0xE6: out += "ae"; break;
    case 0xC7: case 0xE7: out.push_back('c'); break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
      out.push_back('e'); break;
    case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
      out.push_back('i'); break;
    case 0xD1: case 0xF1: out.push_back('n'); break;
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
      out.push_back('o'); break;
    case 0xD9: case 0xDA: case 0xDB: case 0xDC:
    case 0xF9: case 0xFA: case 0xFB: case 0xFC:
      out.push_back('u'); break;
    case 0xDD: case 0xFD: case 0xFF: case 0x178:
      out.push_back('y'); break;
    case 0xDF: out += "ss"; break;
    case 0x152: case 0x153: out += "oe"; break;
    case 0x2019: out.push_back('\''); break; // apostrophe typographique
    default: appendCp(out, cp); break;
    }
  }
  return out;
}

// Minuscule en gardant les accents (clé des n-grammes, construits en .lower()).
// Normalise aussi l'apostrophe typographique (les n-grammes sont en ').
std::string lowerKeep(const std::string &in) {
  std::string out;
  for (uint32_t cp : decodeUtf8(in)) {
    if (cp >= 'A' && cp <= 'Z')
      appendCp(out, cp + 32);
    else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
      appendCp(out, cp + 0x20); // latin-1 majuscule → minuscule
    else if (cp == 0x2019)
      out.push_back('\'');
    else
      appendCp(out, cp);
  }
  return out;
}

// Adjacence clavier AZERTY (modèle de faute par substitution). Le préfixe est
// folded (ascii a-z), donc une table a-z suffit.
const std::unordered_map<char, std::string> &azerty() {
  static const std::unordered_map<char, std::string> m = {
      {'a', "zq"},   {'z', "aes"},  {'e', "zrd"},  {'r', "etf"},
      {'t', "ryg"},  {'y', "tuh"},  {'u', "yij"},  {'i', "uok"},
      {'o', "ipl"},  {'p', "om"},   {'q', "asw"},  {'s', "qdzx"},
      {'d', "sfec"}, {'f', "dgrv"}, {'g', "fhtb"}, {'h', "gjyn"},
      {'j', "hku"},  {'k', "jli"},  {'l', "kmo"},  {'m', "lp"},
      {'w', "qx"},   {'x', "wcs"},  {'c', "xvd"},  {'v', "cbf"},
      {'b', "vng"},  {'n', "bh"}};
  return m;
}

} // namespace

// ------------------------------------------------------------------ Model ----
struct Result {
  std::vector<std::string> candidates;
  bool literalIsWord = false;
  // Mot que l'Espace doit auto-appliquer (haute confiance). "" → garder le
  // littéral. On n'y met une correction FLOUE que si le préfixe ne contient pas
  // d'apostrophe/trait d'union (sinon c'est une contraction qu'on ne mutile pas,
  // ex. "j'ai" qui ne doit jamais devenir "jail").
  std::string autocomplete;
};

// Réglages utilisateur — $XDG_CONFIG_HOME/ime-predictord/config.json,
// rechargé À CHAUD quand le fichier change (pas de redémarrage).
struct Config {
  bool autoApply = true;    // l'Espace peut-il remplacer ?
  double autoDom = 2.0;     // dominance top/2e exigée pour auto-appliquer
  int autoMinLen = 3;       // longueur mini du préfixe pour auto-appliquer
  double langBoost = 1.6;   // boost des mots de la langue du contexte
  bool multiWord = true;    // suggestion multi-mots dans le mot-suivant
};

struct Model {
  std::vector<std::string> words; // tous les mots (forme d'affichage)
  std::vector<uint32_t> freq;     // fréquence par mot
  std::vector<uint8_t> lang;      // 0 = neutre/inconnu, 1 = fr, 2 = en
  std::vector<std::string> fold;  // forme repliée (minuscule sans accent)
  std::vector<uint32_t> byFold;   // indices triés par forme repliée
  std::unordered_set<std::string> caseWords; // mots en minuscule, accents gardés
  std::unordered_map<std::string, uint32_t> id_;
  double freqTot_ = 1.0; // Σ freq (P1, mélange unigramme)
  Config cfg;

  // Modèle KN précalculé. Identifiants 21 bits → clés compactées.
  static constexpr int SH = 21;
  // v -> [(w, P_KN(w|v))] ; (u<<21|v) -> [(w, P_KN(w|u,v))]
  std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, float>>> biAdj;
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, float>>> triAdj;
  std::unordered_map<uint32_t, float> biBo;  // γ2(v)
  std::unordered_map<uint64_t, float> triBo; // γ3(u,v)
  std::vector<float> pcont;                  // P_continuation(w) (KN)
  std::vector<uint32_t> topUni_;             // top mots par P1 (pool de repli)

  // pondérations
  // P1(w) : mélange continuation KN / fréquence brute. La continuation est le
  // bon prior de KN, la fréquence (OpenSubtitles, conversationnel) couvre les
  // mots absents du corpus n-gram.
  static constexpr double UNI_MIX = 0.7;
  // Canal de faute (noisy channel) : P(frappe|mot) par type d'opération.
  static constexpr double CH_TRANSPOSE = 0.12; // inversion de 2 lettres
  static constexpr double CH_SUBST = 0.10;     // voisin AZERTY
  static constexpr double CH_EXTRA = 0.07;     // lettre en trop
  // (Les garde-fous d'auto-application — longueur mini, dominance — sont dans
  // Config : réglables à chaud via config.json.)
  // Un mot APPRIS hors vocabulaire doit avoir été vu >= 2 fois avant de passer
  // devant le modèle (sinon un seul commit d'un fragment pollue à vie).
  static constexpr uint64_t USER_MIN = 2;
  // Continuation mini pour suggérer une expression multi-mots (« sais pas »).
  static constexpr double MULTI_MIN = 0.35;

  uint32_t intern(const std::string &w) {
    auto it = id_.find(w);
    if (it != id_.end())
      return it->second;
    uint32_t wid = words.size();
    id_[w] = wid;
    words.push_back(w);
    freq.push_back(0);
    lang.push_back(0);
    return wid;
  }

  // Unigramme : "mot<sp>fréquence[<sp>langue]" — langue ∈ {fr,en,both}.
  void loadWords(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
      size_t sp = line.find_first_of(" \t");
      if (sp == std::string::npos)
        continue;
      std::string w = line.substr(0, sp);
      char *end = nullptr;
      uint64_t fr = std::strtoull(line.c_str() + sp + 1, &end, 10);
      if (w.empty() || fr == 0)
        continue;
      uint32_t wid = intern(w);
      freq[wid] = uint32_t(std::min<uint64_t>(freq[wid] + fr, 0xFFFFFFFFu));
      while (end && (*end == ' ' || *end == '\t'))
        end++;
      if (end && *end == 'f') // "fr"
        lang[wid] = 1;
      else if (end && *end == 'e') // "en"
        lang[wid] = 2;
      n++;
    }
    fprintf(stderr, "[predictord] %zu mots chargés (%zu lignes)\n", words.size(),
            n);
  }

  // Bigrammes KN : "v<TAB>w<TAB>p". Backoff : "v<TAB>γ".
  void loadBigrams(const std::string &dir) {
    std::ifstream f(dir + "bigrams.tsv");
    std::string a, b;
    double p;
    size_t n = 0;
    while (f >> a >> b >> p) {
      biAdj[intern(a)].push_back({intern(b), float(p)});
      n++;
    }
    std::ifstream g(dir + "bigrams.bo.tsv");
    while (g >> a >> p)
      biBo[intern(a)] = float(p);
    if (n)
      fprintf(stderr, "[predictord] %zu bigrammes KN, %zu contextes (γ:%zu)\n",
              n, biAdj.size(), biBo.size());
  }

  // Trigrammes KN : "u<TAB>v<TAB>w<TAB>p". Backoff : "u<TAB>v<TAB>γ".
  void loadTrigrams(const std::string &dir) {
    std::ifstream f(dir + "trigrams.tsv");
    std::string a, b, d;
    double p;
    size_t n = 0;
    while (f >> a >> b >> d >> p) {
      uint64_t key = (uint64_t(intern(a)) << SH) | intern(b);
      triAdj[key].push_back({intern(d), float(p)});
      n++;
    }
    std::ifstream g(dir + "trigrams.bo.tsv");
    while (g >> a >> b >> p)
      triBo[(uint64_t(intern(a)) << SH) | intern(b)] = float(p);
    if (n)
      fprintf(stderr, "[predictord] %zu trigrammes KN, %zu contextes (γ:%zu)\n",
              n, triAdj.size(), triBo.size());
  }

  // --- Emoji (picker ':' + suggestion par mot-clé exact) ---
  // emoji.tsv : "clé<TAB>emoji<TAB>poids", clé déjà repliée (build_emoji.py).
  struct EmojiKey {
    std::string key;
    uint32_t eid;
    float w;
  };
  std::vector<EmojiKey> emojiKeys_;             // trié par clé (recherche préfixe)
  std::vector<std::string> emojis_;             // eid -> glyphe
  std::unordered_map<std::string, uint32_t> emojiId_;
  std::unordered_map<std::string, uint32_t> emojiExact_; // clé exacte -> meilleur eid

  void loadEmoji(const std::string &dir) {
    std::ifstream f(dir + "emoji.tsv");
    if (!f)
      return;
    std::string line;
    while (std::getline(f, line)) {
      size_t t1 = line.find('\t');
      size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
      if (t2 == std::string::npos)
        continue;
      std::string key = line.substr(0, t1);
      std::string emo = line.substr(t1 + 1, t2 - t1 - 1);
      float w = strtof(line.c_str() + t2 + 1, nullptr);
      auto [it, fresh] = emojiId_.try_emplace(emo, emojis_.size());
      if (fresh)
        emojis_.push_back(emo);
      emojiKeys_.push_back({std::move(key), it->second, w});
    }
    // tri (clé, poids desc) : la 1re occurrence d'une clé est la meilleure →
    // emojiExact_ retient l'emoji canonique de chaque mot-clé.
    std::sort(emojiKeys_.begin(), emojiKeys_.end(),
              [](const EmojiKey &a, const EmojiKey &b) {
                return a.key != b.key ? a.key < b.key : a.w > b.w;
              });
    for (const auto &ek : emojiKeys_)
      emojiExact_.try_emplace(ek.key, ek.eid);
    if (!emojiKeys_.empty())
      fprintf(stderr, "[predictord] %zu clés emoji, %zu emojis\n",
              emojiKeys_.size(), emojis_.size());
  }

  // Continuation unigramme : "w<TAB>Pcont".
  void loadPcont(const std::string &dir) {
    std::ifstream f(dir + "pcont.tsv");
    std::string a;
    double p;
    size_t n = 0;
    while (f >> a >> p) {
      uint32_t wid = intern(a);
      if (wid >= pcont.size())
        pcont.resize(wid + 1, 0.f);
      pcont[wid] = float(p);
      n++;
    }
    if (n)
      fprintf(stderr, "[predictord] %zu Pcont chargés\n", n);
  }

  // À appeler une fois tous les mots internés : repli + index trié + priors.
  // RE-APPELABLE (le dictionnaire perso se recharge à chaud) : reconstruit.
  void finalize() {
    fold.assign(words.size(), {});
    caseWords.clear();
    caseWords.reserve(words.size());
    for (size_t i = 0; i < words.size(); i++) {
      fold[i] = foldStr(words[i]);
      caseWords.insert(lowerKeep(words[i]));
    }
    byFold.resize(words.size());
    for (uint32_t i = 0; i < words.size(); i++)
      byFold[i] = i;
    std::sort(byFold.begin(), byFold.end(),
              [&](uint32_t a, uint32_t b) { return fold[a] < fold[b]; });
    pcont.resize(words.size(), 0.f);
    freqTot_ = 1.0;
    for (uint32_t fr : freq)
      freqTot_ += fr;
    // pool de repli mot-suivant : les ~64 meilleurs mots par P1 (si le contexte
    // est inconnu du modèle, on propose au moins les mots les plus probables).
    topUni_.resize(words.size());
    for (uint32_t i = 0; i < words.size(); i++)
      topUni_[i] = i;
    size_t kk = std::min<size_t>(64, topUni_.size());
    std::partial_sort(topUni_.begin(), topUni_.begin() + kk, topUni_.end(),
                      [&](uint32_t a, uint32_t b) { return p1(a) > p1(b); });
    topUni_.resize(kk);
  }

  // --- Apprentissage utilisateur (persistant) ---
  std::unordered_map<std::string, uint64_t> userUni; // mot -> usage
  std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>>
      userBi; // prev -> mot -> usage
  std::string userLog;

  void loadUser(const std::string &path) {
    userLog = path;
    std::ifstream f(path);
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
      size_t tab = line.find('\t');
      if (tab == std::string::npos)
        continue;
      std::string prev = line.substr(0, tab), word = line.substr(tab + 1);
      if (word.empty())
        continue;
      userUni[word]++;
      if (!prev.empty())
        userBi[lowerKeep(prev)][word]++;
      n++;
    }
    if (n)
      fprintf(stderr, "[predictord] %zu événements utilisateur rejoués\n", n);
  }

  size_t learnEvents_ = 0;

  void learn(const std::string &prev, const std::string &word) {
    if (word.empty())
      return;
    userUni[word]++;
    if (!prev.empty())
      userBi[lowerKeep(prev)][word]++;
    if (!userLog.empty()) {
      std::ofstream f(userLog, std::ios::app);
      f << prev << '\t' << word << '\n';
    }
    if (++learnEvents_ % 512 == 0)
      ageUser();
  }

  // Vieillissement (cache-LM) : tous les 512 commits, les compteurs appris
  // décroissent (×3/4) — les habitudes récentes pèsent plus, les vieilles
  // s'estompent et finissent oubliées. Le journal est COMPACTÉ au même moment
  // (sinon le replay du démarrage annulerait le déclin).
  void ageUser() {
    for (auto it = userUni.begin(); it != userUni.end();)
      if ((it->second = it->second * 3 / 4) == 0)
        it = userUni.erase(it);
      else
        ++it;
    for (auto &kv : userBi)
      for (auto it = kv.second.begin(); it != kv.second.end();)
        if ((it->second = it->second * 3 / 4) == 0)
          it = kv.second.erase(it);
        else
          ++it;
    if (userLog.empty())
      return;
    std::ofstream f(userLog, std::ios::trunc);
    std::unordered_map<std::string, uint64_t> covered;
    for (auto &kv : userBi)
      for (auto &p : kv.second) {
        for (uint64_t i = 0; i < p.second; i++)
          f << kv.first << '\t' << p.first << '\n';
        covered[p.first] += p.second;
      }
    for (auto &kv : userUni)
      for (uint64_t i = covered[kv.first]; i < kv.second; i++)
        f << '\t' << kv.first << '\n';
  }

  // Un mot appris est « de confiance » s'il est un vrai mot du vocabulaire ou
  // s'il a été committé au moins USER_MIN fois — un fragment committé une
  // seule fois (Entrée en plein mot, par ex.) ne pollue pas les suggestions.
  bool userTrusted(const std::string &word, uint64_t count) const {
    return count >= USER_MIN || caseWords.count(lowerKeep(word)) > 0;
  }

  // ------------- Config / dictionnaire perso / snippets / veto (à chaud) ----
  // $XDG_CONFIG_HOME/ime-predictord/{config.json,dict.txt,snippets.tsv} —
  // rechargés quand leur mtime change (stow-ables dans les dotfiles).
  std::string cfgDir_;
  time_t cfgStamp_ = -1, dictStamp_ = -1, snipStamp_ = -1;
  std::vector<std::pair<std::string, std::string>> snips_; // fold(trig) → texte
  std::unordered_map<std::string, std::unordered_set<std::string>>
      veto_; // fold(tapé) -> remplacements refusés (revert utilisateur)
  std::string vetoLog;

  static time_t mtimeOf(const std::string &p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 ? st.st_mtime : 0;
  }

  void maybeReload() {
    if (cfgDir_.empty())
      return;
    time_t t = mtimeOf(cfgDir_ + "/config.json");
    if (t != cfgStamp_) {
      cfgStamp_ = t;
      Config fresh;
      std::ifstream f(cfgDir_ + "/config.json");
      if (f) {
        try {
          json j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
          fresh.autoApply = j.value("autoApply", fresh.autoApply);
          fresh.autoDom = j.value("autoDom", fresh.autoDom);
          fresh.autoMinLen = j.value("autoMinLen", fresh.autoMinLen);
          fresh.langBoost = j.value("langBoost", fresh.langBoost);
          fresh.multiWord = j.value("multiWord", fresh.multiWord);
        } catch (const std::exception &e) {
          fprintf(stderr, "[predictord] config.json invalide: %s\n", e.what());
        }
      }
      cfg = fresh;
    }
    t = mtimeOf(cfgDir_ + "/dict.txt");
    if (t != dictStamp_) {
      dictStamp_ = t;
      // dictionnaire PERSO : "mot [fréquence]" — vocabulaire déclaratif
      // (prénoms, jargon) : jamais autocorrigé (literalIsWord), complétable.
      std::ifstream f(cfgDir_ + "/dict.txt");
      std::string line;
      size_t n = 0;
      while (std::getline(f, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos)
          line.erase(h);
        std::istringstream is(line);
        std::string w;
        uint64_t fr = 5000;
        if (!(is >> w))
          continue;
        is >> fr;
        uint32_t wid = intern(w);
        freq[wid] = uint32_t(std::max<uint64_t>(freq[wid], fr));
        n++;
      }
      finalize(); // nouveaux mots → repli/index/priors reconstruits
      if (n)
        fprintf(stderr, "[predictord] dict perso : %zu mots\n", n);
    }
    t = mtimeOf(cfgDir_ + "/snippets.tsv");
    if (t != snipStamp_) {
      snipStamp_ = t;
      // snippets : "déclencheur<TAB>expansion" (";mail" → adresse…). Le
      // déclencheur exact s'auto-applique sur Espace, un préfixe l'affiche.
      snips_.clear();
      std::ifstream f(cfgDir_ + "/snippets.tsv");
      std::string line;
      while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
          continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0)
          continue;
        snips_.push_back(
            {foldStr(line.substr(0, tab)), line.substr(tab + 1)});
      }
      std::sort(snips_.begin(), snips_.end());
      if (!snips_.empty())
        fprintf(stderr, "[predictord] %zu snippets\n", snips_.size());
    }
  }

  void loadVeto(const std::string &path) {
    vetoLog = path;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      size_t tab = line.find('\t');
      if (tab != std::string::npos)
        veto_[line.substr(0, tab)].insert(line.substr(tab + 1));
    }
  }

  // L'utilisateur a REVERTÉ tapé→appliqué : ne plus jamais l'auto-appliquer.
  void addVeto(const std::string &typed, const std::string &applied) {
    if (typed.empty() || applied.empty())
      return;
    if (veto_[foldStr(typed)].insert(applied).second && !vetoLog.empty()) {
      std::ofstream f(vetoLog, std::ios::app);
      f << foldStr(typed) << '\t' << applied << '\n';
    }
  }

  // Oublie un mot appris (userUni + tous ses bigrammes) et réécrit le journal.
  size_t forget(const std::string &word) {
    size_t removed = userUni.erase(word);
    for (auto &kv : userBi)
      removed += kv.second.erase(word);
    if (!userLog.empty()) {
      std::ifstream in(userLog);
      std::string line, keep;
      while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos || line.substr(tab + 1) != word)
          keep += line + '\n';
      }
      in.close();
      std::ofstream out(userLog, std::ios::trunc);
      out << keep;
    }
    return removed;
  }

  // Borne [lo,hi) des mots dont le repli commence par `fp`.
  std::pair<std::vector<uint32_t>::const_iterator,
            std::vector<uint32_t>::const_iterator>
  foldedPrefixRange(const std::string &fp) const {
    auto lo = std::lower_bound(
        byFold.begin(), byFold.end(), fp,
        [&](uint32_t a, const std::string &p) { return fold[a] < p; });
    auto hi = lo;
    while (hi != byFold.end() && fold[*hi].compare(0, fp.size(), fp) == 0)
      ++hi;
    return {lo, hi};
  }

  // ------------------------------------------------------------- scoring ---
  // Langue dominante du contexte (vote des mots) : 0 neutre, 1 fr, 2 en.
  uint8_t ctxLang(const std::vector<std::string> &context) const {
    int fr = 0, en = 0;
    for (const auto &w : context) {
      auto it = id_.find(lowerKeep(w));
      if (it == id_.end())
        continue;
      if (lang[it->second] == 1)
        fr++;
      else if (lang[it->second] == 2)
        en++;
    }
    return fr > en ? 1 : en > fr ? 2 : 0;
  }

  // Facteur de boost : un contexte français remonte les mots français (et
  // réciproquement) — les mots neutres/inconnus ne bougent pas.
  double langFactor(uint8_t ctxL, uint32_t wid) const {
    if (!ctxL || wid >= lang.size() || !lang[wid])
      return 1.0;
    return lang[wid] == ctxL ? cfg.langBoost : 1.0 / cfg.langBoost;
  }

  // P1(w) : prior unigramme = mélange continuation KN + fréquence brute.
  double p1(uint32_t w) const {
    double pc = w < pcont.size() ? pcont[w] : 0.0;
    double pf = (double(freq[w]) + 1.0) / freqTot_;
    return UNI_MIX * pc + (1.0 - UNI_MIX) * pf;
  }

  // Évaluateur P_KN(w | contexte) pour UNE requête : les listes de suiveurs du
  // contexte sont indexées une fois, puis chaque candidat coûte O(1).
  struct CtxScorer {
    const Model *m = nullptr;
    std::unordered_map<uint32_t, float> bi, tri;
    double g2 = 1.0, g3 = 1.0;
    bool hasV = false, hasUV = false;

    void init(const Model &model, const std::vector<std::string> &context) {
      m = &model;
      if (context.empty())
        return;
      auto vIt = m->id_.find(lowerKeep(context.back()));
      if (vIt != m->id_.end()) {
        uint32_t v = vIt->second;
        auto a = m->biAdj.find(v);
        if (a != m->biAdj.end()) {
          hasV = true;
          bi.reserve(a->second.size() * 2);
          for (auto &pr : a->second)
            bi[pr.first] = pr.second;
        }
        auto bo = m->biBo.find(v);
        if (bo != m->biBo.end())
          g2 = bo->second;
        if (context.size() >= 2) {
          auto uIt = m->id_.find(lowerKeep(context[context.size() - 2]));
          if (uIt != m->id_.end()) {
            uint64_t key = (uint64_t(uIt->second) << SH) | v;
            auto t = m->triAdj.find(key);
            if (t != m->triAdj.end()) {
              hasUV = true;
              tri.reserve(t->second.size() * 2);
              for (auto &pr : t->second)
                tri[pr.first] = pr.second;
            }
            auto tbo = m->triBo.find(key);
            if (tbo != m->triBo.end())
              g3 = tbo->second;
          }
        }
      }
    }

    double pBi(uint32_t w) const {
      if (hasV) {
        auto it = bi.find(w);
        if (it != bi.end())
          return it->second;
      }
      return g2 * m->p1(w);
    }
    double operator()(uint32_t w) const {
      if (hasUV) {
        auto it = tri.find(w);
        if (it != tri.end())
          return it->second;
      }
      return (hasUV ? g3 : 1.0) * pBi(w);
    }
  };

  // ------------------------------------------------------------- stats -----
  // Fenêtre sur la boîte noire : ce que le modèle sait, ce qu'il a appris.
  json stats() const {
    json j;
    j["ok"] = true;
    j["vocab"] = words.size();
    j["bigramContexts"] = biAdj.size();
    j["trigramContexts"] = triAdj.size();
    j["emojis"] = emojis_.size();
    j["snippets"] = snips_.size();
    j["userWords"] = userUni.size();
    size_t nv = 0;
    for (auto &kv : veto_)
      nv += kv.second.size();
    j["vetoPairs"] = nv;
    std::vector<std::pair<std::string, uint64_t>> top(userUni.begin(),
                                                      userUni.end());
    std::sort(top.begin(), top.end(),
              [](auto &a, auto &b) { return a.second > b.second; });
    json tu = json::array(), te = json::array();
    for (auto &p : top) {
      if (emojiId_.count(p.first)) {
        if (te.size() < 8)
          te.push_back({{"emoji", p.first}, {"count", p.second}});
      } else if (tu.size() < 10) {
        tu.push_back({{"word", p.first}, {"count", p.second}});
      }
    }
    j["topUser"] = tu;
    j["topEmoji"] = te;
    return j;
  }

  // ----------------------------------------------------------- prédiction ---
  Result predict(const std::vector<std::string> &context,
                 const std::string &prefix, int k = 6) {
    Result res;
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string &w) {
      if ((int)res.candidates.size() < k && seen.insert(w).second)
        res.candidates.push_back(w);
    };

    if (!prefix.empty() && prefix[0] == ':')
      emojiSearch(prefix.substr(1), k, push, res);
    else if (!prefix.empty())
      completePrefix(context, prefix, k, push, res);
    else
      predictNext(context, k, push, res);
    return res;
  }

private:
  // Picker emoji (préfixe ':') : recherche par mot-clé CLDR (FR+EN, replié).
  // ":"     → favoris de l'utilisateur (usage appris), sinon sélection courante.
  // ":cœur" → ❤️ … classement : poids CLDR, bonus match exact, malus clé
  // longue, et les emojis déjà utilisés remontent fortement.
  template <class Push>
  void emojiSearch(const std::string &rawQuery, int k, Push &&push,
                   Result &res) {
    res.literalIsWord = false;
    const std::string q = foldStr(rawQuery);
    if (q.empty()) {
      std::vector<std::pair<std::string, uint64_t>> fav;
      for (auto &kv : userUni)
        if (emojiId_.count(kv.first))
          fav.push_back({kv.first, kv.second});
      std::sort(fav.begin(), fav.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      for (auto &p : fav)
        push(p.first);
      static const char *defaults[] = {"😂", "❤️", "😊", "👍", "😭", "🙏"};
      for (const char *d : defaults)
        if (emojiId_.count(d))
          push(d);
      return; // pas d'autocomplete : Espace après ':' garde le littéral
    }
    auto lo = std::lower_bound(
        emojiKeys_.begin(), emojiKeys_.end(), q,
        [](const EmojiKey &a, const std::string &p) { return a.key < p; });
    std::unordered_map<uint32_t, double> bestPer;
    for (auto it = lo;
         it != emojiKeys_.end() && it->key.compare(0, q.size(), q) == 0; ++it) {
      double s = it->w + (it->key.size() == q.size() ? 2.0 : 0.0) -
                 0.05 * double(it->key.size() - q.size());
      auto u = userUni.find(emojis_[it->eid]);
      if (u != userUni.end())
        s += 10.0 + double(u->second);
      auto [b, fresh] = bestPer.try_emplace(it->eid, s);
      if (!fresh && s > b->second)
        b->second = s;
    }
    std::vector<std::pair<uint32_t, double>> v(bestPer.begin(), bestPer.end());
    size_t kk = std::min<size_t>(size_t(k), v.size());
    std::partial_sort(v.begin(), v.begin() + kk, v.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < kk; i++)
      push(emojis_[v[i].first]);
    if (!res.candidates.empty())
      res.autocomplete = res.candidates.front(); // ":coeur"+Espace → ❤️
  }

  template <class Push>
  void completePrefix(const std::vector<std::string> &context,
                      const std::string &prefix, int k, Push &&push,
                      Result &res) {
    const std::string fp = foldStr(prefix);
    auto [lo, hi] = foldedPrefixRange(fp);

    // (0) le préfixe tapé est-il déjà un mot réel (à la casse près, accents
    //     compris) ? Si oui on ne l'écrase jamais ; sinon Espace peut compléter
    //     ou auto-accentuer (ex. "etre"→être quand "etre" n'est pas au dico).
    res.literalIsWord = caseWords.count(lowerKeep(prefix)) > 0;

    CtxScorer ctxScore;
    ctxScore.init(*this, context);
    bool hasCtx = ctxScore.hasV || ctxScore.hasUV;
    uint8_t ctxL = ctxLang(context);

    // Score d'un candidat : P_KN(w|ctx) si on a du contexte, sinon le prior
    // P(w|préfixe) ∝ fréquence (en début de phrase la fréquence brute est le
    // bon prior, la « continuation » KN n'a pas de sens sans contexte) — le
    // tout × le boost de langue (contexte fr → mots fr devant, et vice-versa).
    auto scoreOf = [&](uint32_t wid) -> double {
      double s = hasCtx ? ctxScore(wid) : (double(freq[wid]) + 1.0) / freqTot_;
      return s * langFactor(ctxL, wid);
    };

    std::vector<std::pair<std::string, double>> ranked;
    std::unordered_map<std::string, size_t> have;
    auto offer = [&](const std::string &w, double s) {
      auto [it, fresh] = have.try_emplace(w, ranked.size());
      if (fresh)
        ranked.push_back({w, s});
      else if (s > ranked[it->second].second)
        ranked[it->second].second = s; // même mot via 2 fautes → la + probable
    };

    // (0bis) SNIPPETS : déclencheur exact → expansion auto-appliquée ;
    //        préfixe d'un déclencheur → expansion affichée (on voit venir).
    std::string snippetExact;
    auto sl = std::lower_bound(
        snips_.begin(), snips_.end(), fp,
        [](const auto &a, const std::string &p) { return a.first < p; });
    for (auto it = sl;
         it != snips_.end() && it->first.compare(0, fp.size(), fp) == 0;
         ++it) {
      if (it->first == fp)
        snippetExact = it->second;
      offer(it->second, 3e18);
    }

    // (1) mots APPRIS (de confiance) dont le repli commence par le préfixe →
    //     priorité absolue.
    for (auto &kv : userUni)
      if (userTrusted(kv.first, kv.second) &&
          foldStr(kv.first).compare(0, fp.size(), fp) == 0)
        offer(kv.first, 1e18 + double(kv.second));

    // (2) correspondances exactes du modèle.
    size_t exact = 0;
    for (auto it = lo; it != hi; ++it, ++exact)
      offer(words[*it], scoreOf(*it));

    // (3) autocorrection noisy-channel (edit-distance 1) si l'exact est maigre.
    if (exact < size_t(k))
      fuzzyComplete(fp, scoreOf, offer);

    std::partial_sort(
        ranked.begin(),
        ranked.begin() + std::min<size_t>(k, ranked.size()), ranked.end(),
        [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &p : ranked)
      push(p.first);

    // Auto-complétion sur Espace (haute confiance seulement — les candidats
    // restent affichés, on bride uniquement le REMPLACEMENT automatique) :
    //  - préfixe assez long (un sigle de 2 lettres « az » ne devient pas
    //    « aziz ») ;
    //  - le top doit DOMINER le 2e candidat (ambigu → on garde le littéral,
    //    Tab choisit) — sauf mot appris (priorité voulue) ;
    //  - une correction FLOUE ne raccourcit jamais la frappe (« pcq » ne
    //    devient pas « pc ») et jamais à travers une apostrophe/trait d'union
    //    (on ne mutile pas une contraction, « j'ai » ≠ jail).
    if (!snippetExact.empty()) {
      res.autocomplete = snippetExact; // déclencheur explicite → toujours
    } else if (cfg.autoApply && !res.candidates.empty() &&
               fp.size() >= size_t(cfg.autoMinLen)) {
      const std::string &top = res.candidates.front();
      const std::string ftop = foldStr(top);
      bool topIsPrefix = ftop.compare(0, fp.size(), fp) == 0;
      bool fpHasPunct = fp.find_first_of("'-;") != std::string::npos;
      bool isUser = ranked.size() >= 1 && ranked[0].second >= 1e18;
      bool dominant =
          isUser || ranked.size() < 2 ||
          ranked[0].second >= cfg.autoDom * std::max(ranked[1].second, 1e-300);
      bool fuzzyOk = ftop.size() >= fp.size() && !fpHasPunct;
      if (dominant && (topIsPrefix || fuzzyOk))
        res.autocomplete = top;
    }
    // VETO : remplacement déjà refusé par un revert → plus jamais auto.
    if (!res.autocomplete.empty()) {
      auto vIt = veto_.find(fp);
      if (vIt != veto_.end() && vIt->second.count(res.autocomplete))
        res.autocomplete.clear();
    }

    // Suggestion emoji : le mot tapé est exactement un mot-clé emoji
    // ("coeur", "fire"…) → l'emoji s'ajoute en DERNIÈRE position (jamais en
    // tête, jamais auto-appliqué — il faut le choisir explicitement).
    if (fp.size() >= 3) {
      auto e = emojiExact_.find(fp);
      if (e != emojiExact_.end()) {
        const std::string &emo = emojis_[e->second];
        if ((int)res.candidates.size() >= k)
          res.candidates.back() = emo;
        else
          res.candidates.push_back(emo);
      }
    }
  }

  // Canal de faute : génère les variantes du préfixe à distance d'édition 1 et
  // garde le POIDS du type d'opération — score final = P(w|ctx)·P(frappe|w).
  template <class Score, class Offer>
  void fuzzyComplete(const std::string &fp, Score &&score, Offer &&offer) {
    size_t L = fp.size();
    if (L < 3 || L > 14)
      return;
    std::unordered_map<std::string, double> variants; // variante -> poids canal
    auto addv = [&](const std::string &v, double ch) {
      if (v.size() >= 2 && v != fp) {
        auto [it, fresh] = variants.try_emplace(v, ch);
        if (!fresh && ch > it->second)
          it->second = ch; // plusieurs fautes mènent ici → garde la + probable
      }
    };
    auto punct = [](char c) { return c == '\'' || c == '-'; };
    for (size_t i = 0; i + 1 < L; i++) { // transpositions (jamais autour d'un '/-)
      if (punct(fp[i]) || punct(fp[i + 1]))
        continue;
      std::string v = fp;
      std::swap(v[i], v[i + 1]);
      addv(v, CH_TRANSPOSE);
    }
    for (size_t i = 0; i < L; i++) { // suppressions (lettre en trop, pas un '/-)
      if (punct(fp[i]))
        continue;
      std::string v = fp;
      v.erase(i, 1);
      addv(v, CH_EXTRA);
    }
    const auto &adj = azerty(); // substitutions par adjacence
    for (size_t i = 0; i < L; i++) {
      auto it = adj.find(fp[i]);
      if (it == adj.end())
        continue;
      for (char nb : it->second) {
        std::string v = fp;
        v[i] = nb;
        addv(v, CH_SUBST);
      }
    }
    for (const auto &[v, ch] : variants) {
      auto [lo, hi] = foldedPrefixRange(v);
      // top-3 par variante suffisent (on ne veut pas noyer les exacts)
      std::vector<std::pair<uint32_t, double>> tops;
      for (auto it = lo; it != hi; ++it)
        tops.push_back({*it, score(*it)});
      std::partial_sort(
          tops.begin(), tops.begin() + std::min<size_t>(3, tops.size()),
          tops.end(), [](auto &a, auto &b) { return a.second > b.second; });
      size_t kk = std::min<size_t>(3, tops.size());
      for (size_t i = 0; i < kk; i++)
        offer(words[tops[i].first], tops[i].second * ch);
    }
  }

  // Mot-suivant : P_KN(w|u,v) sur l'union des suiveurs observés (trigramme +
  // bigramme) + le pool des meilleurs P1 (contexte inconnu → mots probables).
  // Contexte VIDE = début de phrase → contexte synthétique "<s>" (le builder
  // compte les bigrammes d'amorce). L'apprentissage utilisateur passe devant.
  template <class Push>
  void predictNext(const std::vector<std::string> &context, int k,
                   Push &&push, Result &res) {
    std::vector<std::string> ctx = context;
    if (ctx.empty())
      ctx.push_back("<s>");
    const std::string prev = lowerKeep(ctx.back());

    // (1) bigrammes APPRIS (de confiance) pour ce contexte → priorité.
    auto ub = userBi.find(prev);
    if (ub != userBi.end()) {
      std::vector<std::pair<std::string, uint64_t>> v;
      for (auto &p : ub->second)
        if (userTrusted(p.first, p.second))
          v.push_back(p);
      std::sort(v.begin(), v.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      for (auto &p : v)
        push(p.first);
    }

    // (2) modèle : score exact P_KN(w|ctx) × boost de langue sur le pool.
    CtxScorer ctxScore;
    ctxScore.init(*this, ctx);
    uint8_t ctxL = ctxLang(ctx);
    std::unordered_set<uint32_t> pool;
    for (auto &pr : ctxScore.tri)
      pool.insert(pr.first);
    for (auto &pr : ctxScore.bi)
      pool.insert(pr.first);
    if (pool.size() < size_t(k))
      for (uint32_t w : topUni_)
        pool.insert(w);

    std::vector<std::pair<uint32_t, double>> v;
    v.reserve(pool.size());
    for (uint32_t w : pool)
      v.push_back({w, ctxScore(w) * langFactor(ctxL, w)});
    size_t kk = std::min<size_t>(size_t(k) * 2, v.size());
    std::partial_sort(v.begin(), v.begin() + kk, v.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });

    // (3) MULTI-MOTS : si le meilleur candidat a une continuation très sûre
    //     (P >= MULTI_MIN), proposer l'expression entière en FIN de barre
    //     (« sais pas ») — visible sans déplacer le top des mots simples.
    std::string phrase;
    if (cfg.multiWord && kk > 0) {
      std::vector<std::string> c2(ctx);
      c2.push_back(words[v[0].first]);
      if (c2.size() > 2)
        c2.erase(c2.begin(), c2.end() - 2);
      CtxScorer s2;
      s2.init(*this, c2);
      uint32_t best = 0;
      double bp = 0;
      for (auto &pr : s2.tri)
        if (pr.second > bp) {
          bp = pr.second;
          best = pr.first;
        }
      for (auto &pr : s2.bi)
        if (pr.second > bp) {
          bp = pr.second;
          best = pr.first;
        }
      if (bp >= MULTI_MIN)
        phrase = words[v[0].first] + ' ' + words[best];
    }
    for (size_t i = 0; i < kk; i++)
      push(words[v[i].first]);
    if (!phrase.empty()) {
      if ((int)res.candidates.size() >= k)
        res.candidates.back() = phrase;
      else
        push(phrase);
    }
  }
};

// ------------------------------------------------------------------- main ----
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <words.tsv> [socket-path]\n", argv[0]);
    fprintf(stderr, "  fichiers voisins chargés s'ils existent : bigrams.tsv, "
                    "bigrams.bo.tsv, trigrams.tsv, trigrams.bo.tsv, "
                    "pcont.tsv\n");
    return 2;
  }
  // Un client (l'engine) ferme souvent la connexion sans lire la réponse —
  // notamment les messages "learn" en fire-and-forget. Sans ça, le write côté
  // daemon lèverait SIGPIPE et TUERAIT le daemon : les prédictions mourraient
  // silencieusement après quelques mots (cause majeure de "pas robuste").
  signal(SIGPIPE, SIG_IGN);

  Model model;
  std::string wpath = argv[1];
  model.loadWords(wpath);
  std::string dir = wpath.substr(0, wpath.find_last_of('/') + 1);
  model.loadBigrams(dir);
  model.loadTrigrams(dir);
  model.loadPcont(dir);
  model.loadEmoji(dir);
  model.finalize();

  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  std::string userBase = xdg ? std::string(xdg)
                             : std::string(home ? home : "/tmp") +
                                   "/.local/share";
  std::string userDir = userBase + "/ime-predictord";
  mkdir(userBase.c_str(), 0755);
  mkdir(userDir.c_str(), 0755);
  model.loadUser(userDir + "/user.log");
  model.loadVeto(userDir + "/veto.log");

  // config/dictionnaire perso/snippets ($XDG_CONFIG_HOME/ime-predictord),
  // rechargés à chaud sur mtime à chaque requête.
  const char *xdgc = getenv("XDG_CONFIG_HOME");
  model.cfgDir_ = (xdgc ? std::string(xdgc)
                        : std::string(home ? home : "/tmp") + "/.config") +
                  "/ime-predictord";
  model.maybeReload();

  std::string sockpath = argc > 2 ? argv[2] : "/tmp/ime-predictord.sock";
  unlink(sockpath.c_str());
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockpath.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  listen(srv, 8);
  fprintf(stderr, "[predictord] écoute sur %s\n", sockpath.c_str());

  for (;;) {
    int c = accept(srv, nullptr, nullptr);
    if (c < 0)
      continue;
    std::string buf;
    char tmp[4096];
    ssize_t n;
    while ((n = read(c, tmp, sizeof(tmp))) > 0) {
      buf.append(tmp, n);
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        json resp;
        try {
          json req = json::parse(line);
          if (req.contains("learn")) {
            auto l = req["learn"];
            model.learn(l.value("prev", std::string{}),
                        l.value("word", std::string{}));
            resp["ok"] = true;
          } else if (req.contains("forget")) {
            // outil d'hygiène : oublier un mot appris (et réécrire le journal)
            //   echo '{"forget":{"word":"bonjo"}}' | nc -U $SOCK
            resp["removed"] =
                model.forget(req["forget"].value("word", std::string{}));
            resp["ok"] = true;
          } else if (req.contains("veto")) {
            // l'utilisateur a reverté tapé→appliqué : ne plus auto-appliquer
            auto v = req["veto"];
            model.addVeto(v.value("typed", std::string{}),
                          v.value("applied", std::string{}));
            resp["ok"] = true;
          } else if (req.contains("stats")) {
            resp = model.stats();
          } else {
            std::vector<std::string> ctx =
                req.value("context", std::vector<std::string>{});
            std::string prefix = req.value("prefix", std::string{});
            model.maybeReload(); // config/dict/snippets à chaud (mtime)
            Result r = model.predict(ctx, prefix);
            resp["candidates"] = r.candidates;
            resp["literalIsWord"] = r.literalIsWord;
            resp["autocomplete"] = r.autocomplete;
          }
        } catch (const std::exception &e) {
          resp["candidates"] = json::array();
          resp["error"] = e.what();
        }
        std::string out = resp.dump() + "\n";
        if (write(c, out.data(), out.size()) < 0)
          break;
      }
    }
    close(c);
  }
}
