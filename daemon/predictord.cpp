// Track "câblage" + cerveau — daemon de prédiction (patron fcitx5-azookeyd).
//
// Découplé de l'addon fcitx5 : écoute sur un socket Unix, reçoit une requête
// JSON (préfixe en cours + contexte de mots), renvoie une liste de candidats.
//
// Cerveau v2 — robustesse type Gboard/SwiftKey, toujours CPU-only :
//   - repli ACCENT-INSENSIBLE : tu tapes "francais"/"etre" → "français"/"être".
//   - AUTOCORRECTION floue (edit-distance 1 : transposition, suppression,
//     substitution par adjacence clavier AZERTY) quand le préfixe exact ne
//     donne rien : "bonjuor" → "bonjour", "qaund" → "quand".
//   - complétion RE-CLASSÉE par le contexte (P(mot|précédent) du bigramme) :
//     "je v…" remonte "veux/vais" avant "vous/va".
//   - MOT-SUIVANT en trigramme + stupid-backoff → bigramme → modèle de base,
//     l'apprentissage utilisateur passant toujours devant.
//   - signale `literalIsWord` : l'engine n'écrase un mot réellement tapé que sur
//     sélection explicite (jamais d'autocorrection d'un mot déjà valide).
//
// Protocole (une ligne JSON par message, '\n' terminé) :
//   <- {"context":["je"],"prefix":"v"}
//   -> {"candidates":["veux","vais","vous",...],"literalIsWord":false}
//   <- {"learn":{"prev":"je","word":"code"}}    -> {"ok":true}
//
// Run: predictord <words.tsv> [socket]
//   un 'bigrams.tsv' et un 'trigrams.tsv' voisins sont chargés s'ils existent.
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
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
    default: appendCp(out, cp); break;
    }
  }
  return out;
}

// Minuscule en gardant les accents (clé des n-grammes, construits en .lower()).
std::string lowerKeep(const std::string &in) {
  std::string out;
  for (uint32_t cp : decodeUtf8(in)) {
    if (cp >= 'A' && cp <= 'Z')
      appendCp(out, cp + 32);
    else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
      appendCp(out, cp + 0x20); // latin-1 majuscule → minuscule
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

struct Model {
  std::vector<std::string> words; // tous les mots (forme d'affichage)
  std::vector<uint32_t> freq;     // fréquence par mot
  std::vector<std::string> fold;  // forme repliée (minuscule sans accent)
  std::vector<uint32_t> byFold;   // indices triés par forme repliée
  std::unordered_set<std::string> caseWords; // mots en minuscule, accents gardés
  std::unordered_map<std::string, uint32_t> id_;

  // mot précédent -> (idx du mot suivant, count) ; + total par contexte
  std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>
      bigram;
  std::unordered_map<std::string, uint64_t> bigramTot;
  // "w1\x01w2" -> (idx du mot suivant, count) ; + total par contexte
  std::unordered_map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>
      trigram;
  std::unordered_map<std::string, uint64_t> trigramTot;

  // pondérations
  static constexpr double CTX_LAMBDA = 0.5;     // part du contexte dans la complétion
  static constexpr double FUZZY_PENALTY = 0.08; // pénalité d'une correction floue
  static constexpr double BO_BIGRAM = 0.4;      // stupid-backoff trigramme→bigramme

  uint32_t intern(const std::string &w) {
    auto it = id_.find(w);
    if (it != id_.end())
      return it->second;
    uint32_t wid = words.size();
    id_[w] = wid;
    words.push_back(w);
    freq.push_back(0);
    return wid;
  }

  // Unigramme : "mot<sp|tab>fréquence" (OpenSubtitles fr_50k/en_50k fusionnés).
  void loadWords(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
      size_t sp = line.find_first_of(" \t");
      if (sp == std::string::npos)
        continue;
      std::string w = line.substr(0, sp);
      uint64_t fr = std::strtoull(line.c_str() + sp + 1, nullptr, 10);
      if (w.empty() || fr == 0)
        continue;
      uint32_t wid = intern(w);
      freq[wid] = uint32_t(std::min<uint64_t>(freq[wid] + fr, 0xFFFFFFFFu));
      n++;
    }
    fprintf(stderr, "[predictord] %zu mots chargés (%zu lignes)\n", words.size(),
            n);
  }

  // Bigrammes : "mot1<TAB>mot2<TAB>count".
  void loadBigrams(const std::string &path) {
    std::ifstream f(path);
    if (!f)
      return;
    std::string a, b;
    uint64_t c;
    size_t n = 0;
    while (f >> a >> b >> c) {
      bigram[a].push_back({intern(b), uint32_t(c)});
      bigramTot[a] += c;
      n++;
    }
    fprintf(stderr, "[predictord] %zu bigrammes, %zu contextes\n", n,
            bigram.size());
  }

  // Trigrammes : "mot1<TAB>mot2<TAB>mot3<TAB>count".
  void loadTrigrams(const std::string &path) {
    std::ifstream f(path);
    if (!f)
      return;
    std::string a, b, d;
    uint64_t c;
    size_t n = 0;
    while (f >> a >> b >> d >> c) {
      std::string key = a + '\x01' + b;
      trigram[key].push_back({intern(d), uint32_t(c)});
      trigramTot[key] += c;
      n++;
    }
    fprintf(stderr, "[predictord] %zu trigrammes, %zu contextes\n", n,
            trigram.size());
  }

  // À appeler une fois tous les mots internés : calcule le repli + index trié.
  void finalize() {
    fold.resize(words.size());
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

  // ----------------------------------------------------------- prédiction ---
  Result predict(const std::vector<std::string> &context,
                 const std::string &prefix, int k = 6) {
    Result res;
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string &w) {
      if ((int)res.candidates.size() < k && seen.insert(w).second)
        res.candidates.push_back(w);
    };

    if (!prefix.empty())
      completePrefix(context, prefix, k, push, res);
    else
      predictNext(context, k, push);
    return res;
  }

private:
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

    // contexte : comptes du bigramme P(mot | mot précédent).
    std::unordered_map<uint32_t, uint32_t> bg;
    double bgTot = 0;
    if (!context.empty()) {
      std::string prev = lowerKeep(context.back());
      auto bi = bigram.find(prev);
      if (bi != bigram.end()) {
        for (auto &pr : bi->second)
          bg[pr.first] = pr.second;
        bgTot = double(bigramTot[prev]);
      }
    }

    // somme des fréquences des correspondances exactes → P(mot | préfixe).
    double totFreq = 0;
    for (auto it = lo; it != hi; ++it)
      totFreq += double(freq[*it]) + 1.0;
    if (totFreq <= 0)
      totFreq = 1.0;

    // Score interpolé (façon clavier) : λ·P(mot|contexte) + (1-λ)·P(mot|préfixe).
    // Le contexte pèse autant que la fréquence brute, donc "je v…" remonte
    // "vais/veux" même si "va/vous" sont globalement plus fréquents.
    auto scoreOf = [&](uint32_t wid) -> double {
      double puni = (double(freq[wid]) + 1.0) / totFreq;
      double pctx = 0.0;
      if (bgTot > 0) {
        auto c = bg.find(wid);
        if (c != bg.end())
          pctx = double(c->second) / bgTot;
      }
      return CTX_LAMBDA * pctx + (1.0 - CTX_LAMBDA) * puni;
    };

    std::vector<std::pair<std::string, double>> ranked;
    std::unordered_set<std::string> have;
    auto offer = [&](const std::string &w, double s) {
      if (have.insert(w).second)
        ranked.push_back({w, s});
    };

    // (1) mots APPRIS dont le repli commence par le préfixe → priorité absolue.
    for (auto &kv : userUni)
      if (foldStr(kv.first).compare(0, fp.size(), fp) == 0)
        offer(kv.first, 1e18 + double(kv.second));

    // (2) correspondances exactes du modèle.
    size_t exact = 0;
    for (auto it = lo; it != hi; ++it, ++exact)
      offer(words[*it], scoreOf(*it));

    // (3) autocorrection floue (edit-distance 1) si l'exact est maigre.
    if (exact < size_t(k))
      fuzzyComplete(fp, scoreOf, offer);

    std::partial_sort(
        ranked.begin(),
        ranked.begin() + std::min<size_t>(k, ranked.size()), ranked.end(),
        [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &p : ranked)
      push(p.first);

    // Auto-complétion sur Espace (haute confiance) : le meilleur candidat s'il
    // PROLONGE le préfixe ; sinon (correction floue) seulement si le préfixe ne
    // contient pas d'apostrophe/trait d'union — on ne mutile pas une contraction.
    if (!res.candidates.empty()) {
      const std::string &top = res.candidates.front();
      bool topIsPrefix = foldStr(top).compare(0, fp.size(), fp) == 0;
      bool fpHasPunct =
          fp.find('\'') != std::string::npos || fp.find('-') != std::string::npos;
      if (topIsPrefix || !fpHasPunct)
        res.autocomplete = top;
    }
  }

  // Génère les variantes du préfixe à distance d'édition 1 (transposition,
  // suppression, substitution clavier) et y ajoute les complétions exactes,
  // pénalisées — pour ne sortir que faute de mieux.
  template <class Score, class Offer>
  void fuzzyComplete(const std::string &fp, Score &&score, Offer &&offer) {
    size_t L = fp.size();
    if (L < 3 || L > 14)
      return;
    std::unordered_set<std::string> variants;
    auto addv = [&](const std::string &v) {
      if (v.size() >= 2 && v != fp)
        variants.insert(v);
    };
    auto punct = [](char c) { return c == '\'' || c == '-'; };
    for (size_t i = 0; i + 1 < L; i++) { // transpositions (jamais autour d'un '/-)
      if (punct(fp[i]) || punct(fp[i + 1]))
        continue;
      std::string v = fp;
      std::swap(v[i], v[i + 1]);
      addv(v);
    }
    for (size_t i = 0; i < L; i++) { // suppressions (lettre en trop, pas un '/-)
      if (punct(fp[i]))
        continue;
      std::string v = fp;
      v.erase(i, 1);
      addv(v);
    }
    const auto &adj = azerty(); // substitutions par adjacence
    for (size_t i = 0; i < L; i++) {
      auto it = adj.find(fp[i]);
      if (it == adj.end())
        continue;
      for (char nb : it->second) {
        std::string v = fp;
        v[i] = nb;
        addv(v);
      }
    }
    for (const auto &v : variants) {
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
        offer(words[tops[i].first], tops[i].second * FUZZY_PENALTY);
    }
  }

  // Mot-suivant : trigramme (w1 w2) + stupid-backoff vers le bigramme (w2),
  // l'apprentissage utilisateur étant placé devant.
  template <class Push>
  void predictNext(const std::vector<std::string> &context, int k,
                   Push &&push) {
    if (context.empty())
      return;
    const std::string prev = lowerKeep(context.back());

    // (1) bigrammes APPRIS pour ce contexte → priorité.
    auto ub = userBi.find(prev);
    if (ub != userBi.end()) {
      std::vector<std::pair<std::string, uint64_t>> v(ub->second.begin(),
                                                      ub->second.end());
      std::sort(v.begin(), v.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      for (auto &p : v)
        push(p.first);
    }

    // (2) modèle : stupid-backoff sur PROBABILITÉS (un bigramme fréquent ne doit
    //     pas écraser un bon trigramme) — P(mot|w1 w2), repli 0.4·P(mot|w2).
    std::unordered_map<uint32_t, double> agg;
    if (context.size() >= 2) {
      std::string key = lowerKeep(context[context.size() - 2]) + '\x01' + prev;
      auto t = trigram.find(key);
      if (t != trigram.end()) {
        double tot = double(trigramTot[key]);
        if (tot > 0)
          for (auto &pr : t->second)
            agg[pr.first] += double(pr.second) / tot;
      }
    }
    auto b = bigram.find(prev);
    if (b != bigram.end()) {
      double tot = double(bigramTot[prev]);
      if (tot > 0)
        for (auto &pr : b->second)
          agg[pr.first] += BO_BIGRAM * (double(pr.second) / tot);
    }

    std::vector<std::pair<uint32_t, double>> v(agg.begin(), agg.end());
    size_t kk = std::min<size_t>(size_t(k) * 2, v.size());
    std::partial_sort(v.begin(), v.begin() + kk, v.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < kk; i++)
      push(words[v[i].first]);
  }
};

// ------------------------------------------------------------------- main ----
int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <words.tsv> [socket-path]\n", argv[0]);
    fprintf(stderr, "  un 'bigrams.tsv' et 'trigrams.tsv' voisins sont chargés "
                    "s'ils existent.\n");
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
  model.loadBigrams(dir + "bigrams.tsv");
  model.loadTrigrams(dir + "trigrams.tsv");
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
          } else {
            std::vector<std::string> ctx =
                req.value("context", std::vector<std::string>{});
            std::string prefix = req.value("prefix", std::string{});
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
