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
#include <chrono>
#include <cmath>
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

// threading : requis même sans neural (worker de reformulation, lignes
// différées du poll loop).
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef WITH_NEURAL
#include "neural.h"
#endif

#include "reformulate_http.h" // reformulation via API externe (Groq), repli local

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
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
  // Scores parallèles à candidates (complétion seulement) : nécessaires au
  // rerank neuronal opportuniste (E3). Vide sur les autres chemins.
  std::vector<double> scores;
  bool literalIsWord = false;
  // Mot que l'Espace doit auto-appliquer (haute confiance). "" → garder le
  // littéral. On n'y met une correction FLOUE que si le préfixe ne contient pas
  // d'apostrophe/trait d'union (sinon c'est une contraction qu'on ne mutile pas,
  // ex. "j'ai" qui ne doit jamais devenir "jail").
  std::string autocomplete;
  // Complétion haute-confiance TOUJOURS calculée (mêmes garde-fous), même
  // quand autoApply est off : l'engine l'affiche en texte fantôme et → la
  // committe EXPLICITEMENT. L'Espace, lui, n'applique que `autocomplete`.
  std::string ghost;
  // autocomplete est une RESTAURATION D'ACCENTS pure (fold-equal au tapé :
  // francais→français, oeuvre→œuvre, c'etait→c'était) : l'engine peut alors
  // l'appliquer même si le tapé est un vrai mot du corpus (literalIsWord).
  bool accentOnly = false;
};

// Réglages utilisateur — $XDG_CONFIG_HOME/ime-predictord/config.json,
// rechargé À CHAUD quand le fichier change (pas de redémarrage).
struct Config {
  bool autoApply = true;    // l'Espace peut-il remplacer ?
  double autoDom = 2.0;     // dominance top/2e exigée pour auto-appliquer
  int autoMinLen = 3;       // longueur mini du préfixe pour auto-appliquer
  // Restauration d'ACCENTS/ligatures sur Espace, même quand autoApply est
  double langBoost = 1.6;   // boost des mots de la langue active
  double agreeBoost = 2.0;  // boost d'accord nombre/genre (×si accord, ÷sinon)
  // CACHE DE RÉCENCE : boost des mots déjà présents dans le texte avant le
  // curseur (contexte LARGE `wide` — le document en cours). Le texte humain
  // se répète : un mot déjà employé (nom propre, vocabulaire du sujet) a une
  // forte probabilité de revenir. ≤1.0 = désactivé. Défaut calibré par sweep
  // (2026-07-04) : ~neutre sur le held-out PHRASES INDÉPENDANTES (qui ne
  // peut pas voir la répétition de document, le vrai bénéfice) et négatif
  // au-dessus de 2 — on reste doux ; monter si vos textes sont répétitifs.
  double recencyBoost = 1.3;
  // Agressivité des mots APPRIS : multiplicateur sur la confiance (cf USER_MIN).
  // Le score appris reste sur l'ÉCHELLE du modèle (plus de plancher 1e18) — ce
  // knob permet de remonter/descendre globalement les suggestions apprises.
  double learnedBoost = 1.0;
  // Fréquence effective PLANCHER d'un mot appris de confiance (échelle modèle,
  // amélioration A). Calibré pour le VRAI modèle (fréquences ~10^5–10^6) ; un
  // mot appris vaut au moins ce plancher × confiance. Réglable.
  double learnedFloor = 150000.0;
  // Rétrogradation d'un proclitique d'élision NU (« j' », « c' »…) quand on a
  // tapé pile ce proclitique : il est rarement le mot final voulu. ~25 car le
  // proclitique nu est bien plus fréquent que chaque forme pleine (j' = 2,98M,
  // j'aime = 144k) — il faut le diviser assez pour le faire passer dessous.
  double proclisisDemote = 25.0;
  bool multiWord = true;    // suggestion multi-mots dans le mot-suivant
  // Longueur MAXIMALE de la barre de mots (UX) : on n'affiche que ce nombre de
  // suggestions — le top-N le plus PERTINENT (le modèle, trié par score,
  // remonte les meilleures en tête, donc tronquer = garder les N meilleures).
  // C'est le levier d'expérience : 5 par défaut. La GRILLE emoji (préfixe ':')
  // n'est PAS concernée (c'est une grille, pas la barre — elle garde ses ≤24).
  int barWords = 5;
  // Restitution d'accent sur Espace, INDÉPENDANTE de autoApply : si le mot tapé
  // n'est qu'une version désaccentuée d'un mot du modèle (mêmes lettres pliées,
  // accents/ligatures en plus), appliquer la forme accentuée — on n'ajoute QUE
  // des accents, jamais on ne change/complète/corrige le mot. Sûr même quand
  // autoApply est false (faute d'accent ≠ choix de mot). false pour désactiver.
  bool accentRestore = true;
  // Homographe (la graphie NUE est AUSSI au modèle, ex. ou/où, mur/mûr, etre) :
  // restituer seulement si la forme accentuée DOMINE le littéral par ce facteur.
  // Baisser (ex. 2.0) = plus de restitutions ; monter = plus prudent.
  double accentDom = 4.0;
  // Langue active : "auto" = détection par vote du contexte, "fr"/"en" =
  // langue CHOISIE (déterministe, aucune détection), "off" = aucun boost.
  std::string lang = "auto";

  // --- Prédiction NEURONALE (libllama, opt-in). OFF par défaut → comportement
  // n-gram strictement inchangé. ON + neuralModel chargé au démarrage : les
  // candidats neuronaux passent EN TÊTE du mot-suivant (prefix vide) ; le n-gram
  // reste pour la complétion intra-mot, literalIsWord et autocomplete (sémantique
  // conservatrice d'auto-application). Voir docs/superpowers/specs/2026-06-23-…
  bool neural = false;
  std::string neuralModel; // chemin GGUF (vide → neural désactivé)
  int neuralThreads = 4;
  int neuralTopk = 6;
  // true → mot-suivant 100% NEURONAL (n-gram complètement écarté pour le
  // mot-suivant ; il ne sert plus que de fallback si le neural ne rend rien).
  // C'est l'option "remplacer le n-gram" : neural seul aux commandes.
  bool neuralOnly = false;

  // --- REFORMULATION (Ctrl+Alt+R sur une sélection). Source des variantes :
  // API externe (Groq) UNIQUEMENT — la qualité d'abord (le repli local via le
  // GGUF du mot-suivant produisait des variantes inutilisables). Échec →
  // l'engine affiche un panneau compact (clé manquante / API indisponible).
  // La clé n'est JAMAIS ici : $GROQ_API_KEY ou groq.key dans le DATA dir.
  std::string reformModel = "llama-3.3-70b-versatile";
  std::string reformBaseUrl = "https://api.groq.com/openai/v1/chat/completions";
  // Groq répond en < 2 s : 8 s couvrent les mauvais jours SANS immobiliser le
  // worker 20 s quand le réseau est coupé (l'engine abandonne à 12 s).
  int reformTimeoutMs = 8000;
  // Budget TEMPS de l'appel neuronal (prefill + expansions comprises) : passé
  // ce délai on rend ce qu'on a (l'engine a son propre timeout côté socket —
  // sans ce budget le daemon continuait à brûler du CPU pour une réponse que
  // plus personne n'attendait).
  int neuralBudgetMs = 180;
  // Fusion des scores (E4) : multiplicateur des probabilités neuronales sur
  // l'échelle du modèle n-gram. >1 → le neural mène (sa distribution est plus
  // étalée que les bigrammes KN très sûrs type « ne→pas »).
  double neuralBoost = 2.0;
  // Rerank neuronal de la COMPLÉTION (E3) : opportuniste — seulement quand le
  // cache KV est déjà chaud pour ce contexte (jamais de prefill sur le chemin
  // chaud de la frappe). rerankWeight = poids λ du log-prob neuronal dans le
  // mélange géométrique avec P_KN.
  bool neuralRerank = true;
  double rerankWeight = 0.4;
  // Deux phases ASYNCHRONES (E5) : quand l'engine envoie "async":true, la
  // réponse n-gram part TOUT DE SUITE (pending:true) et le neural tourne sur
  // un thread de travail — une 2e ligne {"refresh":true} suit sur la même
  // connexion. Plus aucun hitch après Espace.
  bool asyncNeural = true;
};

struct Model {
  std::vector<std::string> words; // tous les mots (forme d'affichage)
  std::vector<uint32_t> freq;     // fréquence par mot
  std::vector<uint8_t> lang;      // 0 = neutre/inconnu, 1 = fr, 2 = en
  // Morphologie (Lefff) par forme repliée : genre/nombre, pour l'accord.
  struct Morph { uint8_t g = 0; uint8_t n = 0; }; // g:1=m,2=f ; n:1=s,2=p ; 0=libre
  std::unordered_map<std::string, Morph> morph_;
  std::vector<std::string> fold;  // forme repliée (minuscule sans accent)
  std::vector<uint32_t> byFold;   // indices triés par forme repliée
  // Élisions/contractions indexées par repli SANS APOSTROPHE : « jai »→j'ai,
  // « dici »→d'ici, « cetait »→c'était. Petit sous-ensemble (mots contenant
  // une apostrophe), trié pour la recherche par préfixe.
  std::vector<std::pair<std::string, uint32_t>> elisions_;
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
  static constexpr double CH_APOS = 0.11;      // apostrophe oubliée (« jai »)
  // Élision dont le repli sans apostrophe est EXACTEMENT le tapé (« dici » ==
  // d'ici entier) : bien plus probable qu'une faute générique — sans ce boost
  // « ici » (très fréquent, canal lettre-en-trop) passait devant d'ici.
  static constexpr double CH_APOS_EXACT = 0.5;
  static constexpr double CH_SUBST = 0.10;     // voisin AZERTY
  static constexpr double CH_MISS = 0.09;      // lettre OUBLIÉE (omission)
  static constexpr double CH_EXTRA = 0.07;     // lettre en trop
  // Lettre en trop EN TÊTE de mot : rare comme faute de frappe — c'est bien
  // plus souvent une élision sans apostrophe (« dici », « cetait ») qu'un
  // « d » parasite devant « ici ».
  static constexpr double CH_EXTRA_HEAD = 0.02;
  static constexpr double CH_SPLIT = 0.10;     // espace oublié (« dela »)
  // Restauration d'accents : le candidat fold-equal doit peser au moins ça
  // face au meilleur candidat global — un mot-poubelle du corpus (« cétait »,
  // 259 occurrences) ne doit pas court-circuiter « c'était ».
  static constexpr double ACCENT_MIN_VS_TOP = 0.25;
  // (Les garde-fous d'auto-application — longueur mini, dominance — sont dans
  // Config : réglables à chaud via config.json.)
  // Un mot APPRIS hors vocabulaire doit avoir été vu >= 2 fois avant de passer
  // devant le modèle (sinon un seul commit d'un fragment pollue à vie).
  static constexpr uint64_t USER_MIN = 2;
  // Mots appris à l'ÉCHELLE du modèle (amélioration A) : un mot appris « de
  // confiance » est traité comme s'il avait au MOINS une fréquence effective
  // plancher (complétion : cfg.learnedFloor) / une probabilité de suiveur
  // plancher (mot-suivant : USER_BI_FLOOR), × la confiance (count/USER_MIN ×
  // cfg.learnedBoost). Plancher = visibilité garantie ; le count croissant le
  // fait monter jusqu'à dépasser les mots très fréquents. Plus de plancher
  // 1e18 : un mot appris rare ne coiffe plus « j'ai » (1,6M).
  static constexpr double USER_BI_FLOOR = 0.5;
  // Trigramme appris (contexte 2 mots) = signal personnel PLUS spécifique que le
  // bigramme → plancher de probabilité plus haut, il prime à confiance égale.
  static constexpr double USER_TRI_FLOOR = 0.7;
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

  // morph.tsv : "forme<TAB>genre(m|f|-)<TAB>nombre(s|p|-)<TAB>lemme" (Lefff).
  // Clé = forme repliée (lowerKeep) pour matcher les candidats du modèle.
  void loadMorph(const std::string &dir) {
    std::ifstream f(dir + "morph.tsv");
    std::string line;
    while (std::getline(f, line)) {
      size_t a = line.find('\t');
      if (a == std::string::npos)
        continue;
      size_t b = line.find('\t', a + 1);
      if (b == std::string::npos || b + 1 >= line.size())
        continue;
      Morph m;
      char gc = line[a + 1], nc = line[b + 1];
      m.g = gc == 'm' ? 1 : gc == 'f' ? 2 : 0;
      m.n = nc == 's' ? 1 : nc == 'p' ? 2 : 0;
      if (m.g || m.n)
        morph_[lowerKeep(line.substr(0, a))] = m;
    }
    fprintf(stderr, "[predictord] %zu formes morpho chargées\n", morph_.size());
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
  std::vector<uint32_t> topEmojis_;             // populaires (grille ':' vide)

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
    // populaires : top par poids max (le prior de popularité Unicode est déjà
    // dans les poids) — remplit la grille quand ':' est tapé seul.
    std::vector<std::pair<float, uint32_t>> byW(emojis_.size(),
                                                {0.f, 0u});
    for (const auto &ek : emojiKeys_) {
      byW[ek.eid].second = ek.eid;
      if (ek.w > byW[ek.eid].first)
        byW[ek.eid].first = ek.w;
    }
    std::sort(byW.begin(), byW.end(),
              [](auto &a, auto &b) { return a.first > b.first; });
    topEmojis_.clear();
    for (size_t i = 0; i < byW.size() && topEmojis_.size() < 32; i++)
      topEmojis_.push_back(byW[i].second);
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

  // Trie chaque liste de suiveurs par id de mot → le CtxScorer fait ses
  // lookups par DICHOTOMIE directement dans la liste, sans la recopier.
  // À appeler une fois après le chargement des n-grammes (les rechargements à
  // chaud — dict/config/snippets — ne touchent pas biAdj/triAdj).
  void indexNgrams() {
    auto byId = [](const std::pair<uint32_t, float> &a,
                   const std::pair<uint32_t, float> &b) {
      return a.first < b.first;
    };
    for (auto &kv : biAdj)
      std::sort(kv.second.begin(), kv.second.end(), byId);
    for (auto &kv : triAdj)
      std::sort(kv.second.begin(), kv.second.end(), byId);
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
    // index des élisions par repli SANS apostrophe (« jai » → j'ai)
    elisions_.clear();
    for (uint32_t i = 0; i < words.size(); i++) {
      if (fold[i].find('\'') == std::string::npos)
        continue;
      std::string k = fold[i];
      k.erase(std::remove(k.begin(), k.end(), '\''), k.end());
      if (k.size() >= 2)
        elisions_.push_back({std::move(k), i});
    }
    std::sort(elisions_.begin(), elisions_.end());
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
  // Trigrammes appris : clé "prev2\tprev1" (repliée) -> mot -> usage. Reconstruit
  // depuis la chaîne de commits (cf learn) ; journal séparé user.tri.log.
  std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>>
      userTri;
  std::string lastPrev_, lastWord_; // 2 derniers commits (repli), pour le tri.
  std::string userLog, userTriLog;

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

  // Journal trigramme : lignes "prev2\tprev1\tword" (explicite, pas de
  // reconstruction au replay — l'ordre du journal bigramme n'est pas garanti
  // après vieillissement).
  void loadUserTri(const std::string &path) {
    userTriLog = path;
    std::ifstream f(path);
    std::string line;
    size_t n = 0;
    while (std::getline(f, line)) {
      size_t t1 = line.find('\t');
      if (t1 == std::string::npos)
        continue;
      size_t t2 = line.find('\t', t1 + 1);
      if (t2 == std::string::npos)
        continue;
      std::string p2 = line.substr(0, t1), p1 = line.substr(t1 + 1, t2 - t1 - 1),
                  word = line.substr(t2 + 1);
      if (p2.empty() || p1.empty() || word.empty())
        continue;
      userTri[lowerKeep(p2) + "\t" + lowerKeep(p1)][word]++;
      n++;
    }
    if (n)
      fprintf(stderr, "[predictord] %zu trigrammes utilisateur rejoués\n", n);
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
    // Trigramme : reconstruit depuis la chaîne de commits. Le commit précédent
    // était (lastPrev_ -> lastWord_) ; si le présent enchaîne dessus
    // (prev == lastWord_) et qu'il y a bien un mot AVANT prev (lastPrev_), alors
    // (lastPrev_, prev) -> word est un trigramme observé.
    if (!lastPrev_.empty() && !lastWord_.empty() &&
        lowerKeep(prev) == lowerKeep(lastWord_)) {
      std::string key = lowerKeep(lastPrev_) + "\t" + lowerKeep(prev);
      userTri[key][word]++;
      if (!userTriLog.empty()) {
        std::ofstream f(userTriLog, std::ios::app);
        f << lastPrev_ << '\t' << prev << '\t' << word << '\n';
      }
    }
    lastPrev_ = prev;
    lastWord_ = word;
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
    // Trigrammes : même déclin ×3/4, et journal trigramme recompacté.
    for (auto &kv : userTri)
      for (auto it = kv.second.begin(); it != kv.second.end();)
        if ((it->second = it->second * 3 / 4) == 0)
          it = kv.second.erase(it);
        else
          ++it;
    if (!userTriLog.empty()) {
      std::ofstream ft(userTriLog, std::ios::trunc);
      for (auto &kv : userTri) {
        size_t sep = kv.first.find('\t'); // clé = "prev2\tprev1"
        std::string p2 = kv.first.substr(0, sep), p1 = kv.first.substr(sep + 1);
        for (auto &p : kv.second)
          for (uint64_t i = 0; i < p.second; i++)
            ft << p2 << '\t' << p1 << '\t' << p.first << '\n';
      }
    }
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

  // Confiance d'un mot appris (amélioration A) : >= 1 dès le seuil de confiance
  // (count == USER_MIN), croît linéairement avec l'usage, modulée par le knob
  // cfg.learnedBoost. Multiplie le prior/probabilité plancher du mot appris.
  double learnedConf(uint64_t count) const {
    return (double(count) / double(USER_MIN)) * cfg.learnedBoost;
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
          fresh.accentRestore = j.value("accentRestore", fresh.accentRestore);
          fresh.accentDom = j.value("accentDom", fresh.accentDom);
          fresh.barWords = std::max(1, std::min(8, j.value("barWords",
                                                           fresh.barWords)));
          fresh.langBoost = j.value("langBoost", fresh.langBoost);
          fresh.agreeBoost = j.value("agreeBoost", fresh.agreeBoost);
          fresh.recencyBoost = j.value("recencyBoost", fresh.recencyBoost);
          fresh.learnedBoost = j.value("learnedBoost", fresh.learnedBoost);
          fresh.learnedFloor = j.value("learnedFloor", fresh.learnedFloor);
          fresh.proclisisDemote =
              j.value("proclisisDemote", fresh.proclisisDemote);
          fresh.multiWord = j.value("multiWord", fresh.multiWord);
          fresh.lang = j.value("lang", fresh.lang);
          if (fresh.lang != "auto" && fresh.lang != "fr" &&
              fresh.lang != "en" && fresh.lang != "off") {
            fprintf(stderr, "[predictord] lang inconnue '%s' → auto\n",
                    fresh.lang.c_str());
            fresh.lang = "auto";
          }
          fresh.neural = j.value("neural", fresh.neural);
          fresh.neuralModel = j.value("neuralModel", fresh.neuralModel);
          fresh.neuralThreads = j.value("neuralThreads", fresh.neuralThreads);
          fresh.neuralTopk = j.value("neuralTopk", fresh.neuralTopk);
          fresh.neuralOnly = j.value("neuralOnly", fresh.neuralOnly);
          fresh.reformModel = j.value("reformModel", fresh.reformModel);
          fresh.reformBaseUrl = j.value("reformBaseUrl", fresh.reformBaseUrl);
          fresh.reformTimeoutMs = j.value("reformTimeoutMs", fresh.reformTimeoutMs);
          fresh.neuralBudgetMs = j.value("neuralBudgetMs", fresh.neuralBudgetMs);
          fresh.neuralBoost = j.value("neuralBoost", fresh.neuralBoost);
          fresh.neuralRerank = j.value("neuralRerank", fresh.neuralRerank);
          fresh.rerankWeight = j.value("rerankWeight", fresh.rerankWeight);
          fresh.asyncNeural = j.value("asyncNeural", fresh.asyncNeural);
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

  // Oublie un mot appris (userUni + bigrammes + trigrammes) et réécrit les
  // journaux correspondants.
  size_t forget(const std::string &word) {
    size_t removed = userUni.erase(word);
    for (auto &kv : userBi)
      removed += kv.second.erase(word);
    for (auto &kv : userTri)
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
    // Journal trigramme : retire les lignes dont le mot final == word.
    if (!userTriLog.empty()) {
      std::ifstream in(userTriLog);
      std::string line, keep;
      while (std::getline(in, line)) {
        size_t t = line.rfind('\t');
        if (t == std::string::npos || line.substr(t + 1) != word)
          keep += line + '\n';
      }
      in.close();
      std::ofstream out(userTriLog, std::ios::trunc);
      out << keep;
    }
    return removed;
  }

  // Canal APOSTROPHE OUBLIÉE pour une clé sans apostrophe. Deux sources :
  //  - les élisions du VOCABULAIRE, indexées par repli sans apostrophe
  //    (« jai » → j'ai) — correspondance exacte du mot entier = boost
  //    CH_APOS_EXACT (bien plus probable qu'une faute générique) ;
  //  - la SYNTHÈSE productive proclitique+'+mot (« temmener » → t' + emmener
  //    → t'emmener, absent du vocabulaire) — l'élision française est
  //    productive, le vocab ne liste pas toutes les formes. Initiale
  //    vocalique exigée (t'emmener ✓, t'porte ✗). `synth` l'active (on la
  //    coupe quand le préfixe a déjà des correspondances exactes : taper
  //    « les » ne doit pas faire surgir « l'esprit »).
  template <class Score, class Offer>
  void elisionOffers(const std::string &key, double ch, bool synth,
                     Score &&score, Offer &&offer) {
    auto [el, eh] = elisionPrefixRange(key);
    for (auto it = el; it != eh; ++it)
      offer(words[it->second],
            score(it->second) *
                (it->first == key ? ch * (CH_APOS_EXACT / CH_APOS) : ch));
    if (!synth)
      return;
    size_t plen = 0;
    if (key.size() >= 3 && std::strchr("jcdlmnst", key[0]))
      plen = 1;
    else if (key.size() >= 4 && key[0] == 'q' && key[1] == 'u')
      plen = 2;
    if (!plen || std::string("aeiouyh").find(key[plen]) == std::string::npos)
      return;
    const std::string rest = key.substr(plen);
    auto [lo, hi] = foldedPrefixRange(rest);
    std::vector<std::pair<uint32_t, double>> tops;
    for (auto it = lo; it != hi; ++it)
      tops.push_back({*it, score(*it)});
    std::partial_sort(tops.begin(),
                      tops.begin() + std::min<size_t>(3, tops.size()),
                      tops.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(3, tops.size()); i++)
      offer(key.substr(0, plen) + "'" + words[tops[i].first],
            tops[i].second * ch);
  }

  // Borne [lo,hi) des ÉLISIONS dont le repli sans apostrophe commence par
  // `fp` (« jai » → j'ai, « jav » → j'avais…).
  std::pair<std::vector<std::pair<std::string, uint32_t>>::const_iterator,
            std::vector<std::pair<std::string, uint32_t>>::const_iterator>
  elisionPrefixRange(const std::string &fp) const {
    auto lo = std::lower_bound(
        elisions_.begin(), elisions_.end(), fp,
        [](const auto &a, const std::string &p) { return a.first < p; });
    auto hi = lo;
    while (hi != elisions_.end() &&
           hi->first.compare(0, fp.size(), fp) == 0)
      ++hi;
    return {lo, hi};
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
  // Langue active : choisie par l'utilisateur (cfg.lang, cf preferences) ou,
  // en mode "auto" seulement, détectée par vote des mots du contexte.
  // 0 = neutre/aucun boost, 1 = fr, 2 = en.
  uint8_t ctxLang(const std::vector<std::string> &context) const {
    if (cfg.lang == "fr")
      return 1;
    if (cfg.lang == "en")
      return 2;
    if (cfg.lang == "off")
      return 0;
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
  // Langue CHOISIE (cfg.lang == "fr"/"en", pas "auto") = mode STRICT : la langue
  // opposée est EXCLUE (facteur 0 → les appelants jettent les scores <= 0). Ça
  // supprime non seulement les mots anglais courants (the/you/to…) mais aussi
  // les tokens de contraction anglaise à apostrophe initiale ('ai, 'am, 'all…)
  // que la complétion fuzzy faisait remonter sur les élisions françaises (j'ai).
  // En mode "auto" (détection par contexte), on se contente de pénaliser.
  double langFactor(uint8_t ctxL, uint32_t wid) const {
    if (!ctxL || wid >= lang.size() || !lang[wid])
      return 1.0;
    if (lang[wid] == ctxL)
      return cfg.langBoost;
    if (cfg.lang == "fr" || cfg.lang == "en")
      return 0.0; // langue choisie → exclusion stricte de l'autre langue
    return 1.0 / cfg.langBoost;
  }

  // Mot APPRIS à écarter en mode langue stricte : les candidats appris
  // (userUni/userBi) sont offerts avec un score énorme qui court-circuite
  // langFactor. S'ils sont connus du modèle DANS la langue exclue (ex. "to",
  // "be" tapés autrefois en anglais puis appris), on les écarte aussi. Un mot
  // appris hors-modèle (neutre, sans étiquette) est conservé.
  bool langExcludedWord(const std::string &w, uint8_t ctxL) const {
    auto it = id_.find(lowerKeep(w));
    return it != id_.end() && langFactor(ctxL, it->second) == 0.0;
  }

  // --- CACHE DE RÉCENCE (façon cache-LM Gboard) --------------------------
  // Les mots déjà présents dans le texte avant le curseur (`wide`, ~240 car.
  // via SurroundingText) ont une forte probabilité de revenir — noms propres,
  // vocabulaire du sujet en cours. setRecency() est appelé AVANT chaque
  // predict() (thread principal uniquement) ; recFactor() multiplie le score
  // dans le même pipeline que langue/accord. Le DERNIER mot du contexte est
  // exclu : on ne pousse pas la répétition immédiate (« le le »).
  std::unordered_set<uint32_t> recent_;
  void setRecency(const std::string &wide,
                  const std::vector<std::string> &ctx) {
    recent_.clear();
    if (cfg.recencyBoost <= 1.0 || wide.empty())
      return;
    const std::string low = lowerKeep(wide);
    std::string cur;
    auto flush = [&] {
      if (cur.size() >= 2) { // 1 lettre = bruit
        auto it = id_.find(cur);
        if (it != id_.end())
          recent_.insert(it->second);
      }
      cur.clear();
    };
    for (unsigned char c : low) {
      // lettre ASCII, octet UTF-8 (accents), apostrophe ou trait d'union =
      // caractère de mot (même découpe que TOK côté build) ; le reste sépare.
      if ((c >= 'a' && c <= 'z') || c >= 0x80 || c == '\'' || c == '-')
        cur.push_back(char(c));
      else
        flush();
    }
    flush();
    if (!ctx.empty()) {
      auto it = id_.find(lowerKeep(ctx.back()));
      if (it != id_.end())
        recent_.erase(it->second);
    }
  }
  double recFactor(uint32_t wid) const {
    return recent_.count(wid) ? cfg.recencyBoost : 1.0;
  }

  // Forme = proclitique d'élision NU (proclitique + apostrophe, rien après) :
  // « j' », « c' », « qu' »… Rarement le mot final voulu (amélioration B) ; on
  // les rétrograde quand l'utilisateur a tapé pile ce proclitique.
  static bool isBareProcliticFold(const std::string &f) {
    static const std::unordered_set<std::string> P = {
        "j'", "c'", "qu'", "d'", "n'", "s'", "t'", "m'", "l'"};
    return P.count(f) > 0;
  }

  // ------ Accord grammatical (nombre/genre) ------------------------------
  struct Agree { uint8_t g = 0; uint8_t n = 0; }; // 0 = libre sur cette dimension

  // Déterminant gouverneur → contrainte de genre/nombre qu'il impose au SN.
  Agree determiner(const std::string &w) const {
    static const std::unordered_map<std::string, Agree> D = {
        {"les", {0, 2}},      {"des", {0, 2}},     {"mes", {0, 2}},
        {"tes", {0, 2}},      {"ses", {0, 2}},     {"nos", {0, 2}},
        {"vos", {0, 2}},      {"leurs", {0, 2}},   {"ces", {0, 2}},
        {"aux", {0, 2}},      {"quelques", {0, 2}},{"plusieurs", {0, 2}},
        {"certains", {1, 2}}, {"certaines", {2, 2}},
        {"deux", {0, 2}},     {"trois", {0, 2}},   {"quatre", {0, 2}},
        {"cinq", {0, 2}},     {"six", {0, 2}},     {"sept", {0, 2}},
        {"huit", {0, 2}},     {"neuf", {0, 2}},    {"dix", {0, 2}},
        {"le", {1, 1}},       {"un", {1, 1}},      {"ce", {1, 1}},
        {"cet", {1, 1}},      {"mon", {1, 1}},     {"ton", {1, 1}},
        {"son", {1, 1}},      {"la", {2, 1}},      {"une", {2, 1}},
        {"cette", {2, 1}},    {"ma", {2, 1}},      {"ta", {2, 1}},
        {"sa", {2, 1}},       {"l'", {0, 1}},
    };
    auto it = D.find(w);
    return it == D.end() ? Agree{0, 0} : it->second;
  }

  // Mot qui ROMPT le groupe nominal (nouvelle proposition) → pas d'accord à
  // travers. Best-effort : conjonctions (le candidat côté morph_ filtre déjà
  // les non-noms/adjectifs, ce qui rattrape les verbes).
  bool npBreaker(const std::string &w) const {
    static const std::unordered_set<std::string> C = {"et",  "ou",  "mais",
                                                       "donc", "car", "ni",
                                                       "or",  "que", "qui"};
    return C.count(w) > 0;
  }

  // Contrainte d'accord imposée par le contexte : on remonte jusqu'au
  // déterminant gouverneur le plus proche (≤4 mots, à travers les adjectifs),
  // en s'arrêtant à un briseur de SN.
  Agree agreementOf(const std::vector<std::string> &context) const {
    int steps = 0;
    for (auto it = context.rbegin(); it != context.rend() && steps < 4;
         ++it, ++steps) {
      std::string w = lowerKeep(*it);
      if (npBreaker(w))
        break;
      Agree a = determiner(w);
      if (a.g || a.n)
        return a;
    }
    return {0, 0};
  }

  // Facteur d'accord : ×agreeBoost si la forme s'accorde, ÷agreeBoost si elle
  // est connue ET désaccordée. Neutre (1.0) hors-lexique ou dimension libre.
  // Jamais 0 → ne supprime jamais un candidat (réordonne seulement).
  double agreeFactor(const Agree &want, uint32_t wid) const {
    if ((!want.g && !want.n) || wid >= words.size())
      return 1.0;
    auto it = morph_.find(lowerKeep(words[wid]));
    if (it == morph_.end())
      return 1.0;
    const Morph &m = it->second;
    double f = 1.0;
    if (want.n && m.n)
      f *= (m.n == want.n) ? cfg.agreeBoost : 1.0 / cfg.agreeBoost;
    if (want.g && m.g)
      f *= (m.g == want.g) ? cfg.agreeBoost : 1.0 / cfg.agreeBoost;
    return f;
  }

  // P1(w) : prior unigramme = mélange continuation KN + fréquence brute.
  double p1(uint32_t w) const {
    double pc = w < pcont.size() ? pcont[w] : 0.0;
    double pf = (double(freq[w]) + 1.0) / freqTot_;
    return UNI_MIX * pc + (1.0 - UNI_MIX) * pf;
  }

  // Évaluateur P_KN(w | contexte) pour UNE requête : pointe DIRECTEMENT sur
  // les listes de suiveurs (triées par id de mot, cf indexNgrams), lookup par
  // dichotomie. L'ancienne version recopiait chaque liste en hash map à
  // chaque requête — pour un contexte fréquent (« de », « <s> »...) c'était
  // des dizaines de milliers d'insertions par frappe : 1,3-4 ms mesurées sur
  // le chemin mot-suivant, payées de façon synchrone par l'engine.
  struct CtxScorer {
    using Adj = std::vector<std::pair<uint32_t, float>>;
    const Model *m = nullptr;
    const Adj *bi = nullptr, *tri = nullptr; // suiveurs observés (ou nullptr)
    double g2 = 1.0, g3 = 1.0;
    bool hasV = false, hasUV = false;

    static const float *find(const Adj *a, uint32_t w) {
      if (!a)
        return nullptr;
      auto it = std::lower_bound(a->begin(), a->end(), w,
                                 [](const std::pair<uint32_t, float> &p,
                                    uint32_t x) { return p.first < x; });
      return it != a->end() && it->first == w ? &it->second : nullptr;
    }

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
          bi = &a->second;
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
              tri = &t->second;
            }
            auto tbo = m->triBo.find(key);
            if (tbo != m->triBo.end())
              g3 = tbo->second;
          }
        }
      }
    }

    double pBi(uint32_t w) const {
      if (const float *p = find(bi, w))
        return *p;
      return g2 * m->p1(w);
    }
    double operator()(uint32_t w) const {
      if (const float *p = find(tri, w))
        return *p;
      return (hasUV ? g3 : 1.0) * pBi(w);
    }
    // Poids de repli appliqué à un prior unigramme P1(w) pour un mot NON observé
    // dans ce contexte (ni trigramme ni bigramme) : (γ3?)·γ2. Sert à placer un
    // mot APPRIS hors-contexte sur la même échelle que les candidats du modèle.
    double backoff() const { return (hasUV ? g3 : 1.0) * g2; }
  };

  // ------------------------------------------------------------- stats -----
  // Fenêtre sur la boîte noire : ce que le modèle sait, ce qu'il a appris.
  json stats() const {
    json j;
    j["ok"] = true;
    j["lang"] = cfg.lang; // langue active (préférences)
    j["morph"] = morph_.size(); // formes morphologiques chargées (accord)
    j["vocab"] = words.size();
    j["bigramContexts"] = biAdj.size();
    j["trigramContexts"] = triAdj.size();
    j["emojis"] = emojis_.size();
    j["snippets"] = snips_.size();
    j["userWords"] = userUni.size();
    j["userTrigrams"] = userTri.size();
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
  // neuralCands : candidats (mot, probabilité softmax) du prédicteur neuronal,
  // FUSIONNÉS au mot-suivant sur l'échelle des scores (E4) — vide sans neural.
  Result predict(const std::vector<std::string> &context,
                 const std::string &prefix, int k = 6,
                 const std::vector<std::pair<std::string, double>>
                     &neuralCands = {}) {
    Result res;
    // mode emoji : la barre devient une GRILLE (3×8) → 24 candidats.
    if (!prefix.empty() && prefix[0] == ':')
      k = 24;
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string &w) {
      if ((int)res.candidates.size() < k && seen.insert(w).second) {
        res.candidates.push_back(w);
        return true;
      }
      return false;
    };

    if (!prefix.empty() && prefix[0] == ':')
      emojiSearch(prefix.substr(1), k, push, res);
    else if (!prefix.empty())
      completePrefix(context, prefix, k, push, res);
    else
      predictNext(context, k, push, res, neuralCands);
    return res;
  }

  // Un mot que le daemon connaît déjà : vocabulaire du modèle (dict perso
  // compris) ou appris. Sert à repérer les FRAGMENTS BPE du neural (« l » de
  // « l'école ») qui méritent une expansion multi-token (E2).
  bool isKnownWord(const std::string &w) const {
    return caseWords.count(lowerKeep(w)) > 0 || userUni.count(w) > 0;
  }

  // Candidat neuronal suspect de n'être qu'un DÉBUT de mot : inconnu de tout
  // lexique, ou lettre seule qui n'est pas un vrai mot français/anglais isolé
  // (« l » « d » « j » traînent dans le vocab OpenSubtitles en artefacts de
  // tokenisation — on ne s'y fie pas).
  bool looksFragment(const std::string &w) const {
    if (!isKnownWord(w))
      return true;
    if (w.size() > 2)
      return false;
    static const std::unordered_set<std::string> kSingles = {
        "a", "à", "y", "ô", "i"}; // vrais mots d'une lettre (fr + « I » anglais)
    std::string f = lowerKeep(w);
    return decodeUtf8(f).size() == 1 && !kSingles.count(f);
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
        push(p.first); // tes favoris d'abord…
      for (uint32_t eid : topEmojis_)
        push(emojis_[eid]); // …puis les populaires (remplit la grille)
      return; // pas d'autocomplete : Espace après ':' garde le littéral
    }
    std::unordered_map<uint32_t, double> bestPer;
    auto scan = [&](const std::string &p, double mult) {
      auto lo = std::lower_bound(
          emojiKeys_.begin(), emojiKeys_.end(), p,
          [](const EmojiKey &a, const std::string &pre) { return a.key < pre; });
      for (auto it = lo;
           it != emojiKeys_.end() && it->key.compare(0, p.size(), p) == 0;
           ++it) {
        double s = (it->w + (it->key.size() == p.size() ? 2.0 : 0.0) -
                    0.05 * double(it->key.size() - p.size())) *
                   mult;
        auto u = userUni.find(emojis_[it->eid]);
        if (u != userUni.end())
          s += 10.0 + double(u->second);
        auto [b, fresh] = bestPer.try_emplace(it->eid, s);
        if (!fresh && s > b->second)
          b->second = s;
      }
    };
    scan(q, 1.0);
    // Tolérance aux FAUTES : rien en préfixe exact et requête assez longue →
    // on réessaie les transpositions (« ceour » → cœur) et les suppressions
    // d'un caractère (« coeurr » → cœur), score pénalisé — même esprit que
    // le canal noisy des mots, en beaucoup plus simple.
    if (bestPer.empty() && q.size() >= 3) {
      for (size_t i = 0; i + 1 < q.size(); i++) {
        std::string t = q;
        std::swap(t[i], t[i + 1]);
        if (t != q)
          scan(t, 0.5);
      }
      for (size_t i = 0; i < q.size(); i++)
        scan(q.substr(0, i) + q.substr(i + 1), 0.5);
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
    const Agree want = agreementOf(context); // contrainte d'accord du SN
    auto scoreOf = [&](uint32_t wid) -> double {
      double s = hasCtx ? ctxScore(wid) : (double(freq[wid]) + 1.0) / freqTot_;
      s *= langFactor(ctxL, wid) * agreeFactor(want, wid) * recFactor(wid);
      // B : rétrograder un proclitique d'élision NU quand on a tapé pile ce
      //     proclitique — « j' » propose j'ai/j'aime avant le « j' » nu.
      if (fold[wid] == fp && isBareProcliticFold(fold[wid]))
        s /= cfg.proclisisDemote;
      return s;
    };

    std::vector<std::pair<std::string, double>> ranked;
    std::unordered_map<std::string, size_t> have;
    auto offer = [&](const std::string &w, double s) {
      if (s <= 0.0)
        return; // exclusion stricte de langue (langFactor → 0)
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

    // (1) mots APPRIS (de confiance) dont le repli commence par le préfixe.
    //     Score sur l'ÉCHELLE DU MODÈLE (amélioration A) : fréquence effective
    //     = max(freq réelle, USER_FLOOR_FREQ) × confiance, convertie en prior
    //     unigramme et repliée dans le contexte courant — ils CONCURRENCENT les
    //     candidats du modèle au lieu de les court-circuiter (plus de 1e18). Un
    //     mot appris rare passe devant les mots ordinaires, jamais devant un mot
    //     massivement plus fréquent (« j'ai ») ; très appris, il finit devant.
    for (auto &kv : userUni) {
      if (!userTrusted(kv.first, kv.second) ||
          foldStr(kv.first).compare(0, fp.size(), fp) != 0 ||
          langExcludedWord(kv.first, ctxL))
        continue;
      auto idit = id_.find(lowerKeep(kv.first));
      double realFreq = idit != id_.end() ? double(freq[idit->second]) : 0.0;
      double effFreq =
          std::max(realFreq, cfg.learnedFloor) * learnedConf(kv.second);
      double prior = (effFreq + 1.0) / freqTot_;
      double s = hasCtx ? ctxScore.backoff() * prior : prior;
      if (idit != id_.end())
        s *= langFactor(ctxL, idit->second) * agreeFactor(want, idit->second) *
             recFactor(idit->second);
      offer(kv.first, s);
    }

    // (2) correspondances exactes du modèle.
    size_t exact = 0;
    for (auto it = lo; it != hi; ++it, ++exact)
      offer(words[*it], scoreOf(*it));

    // (2bis) APOSTROPHE OUBLIÉE : « jai »→j'ai, « dici »→d'ici, « cetait »→
    //        c'était — élisions du vocab + synthèse proclitique (t'emmener).
    //        Synthèse seulement sans correspondance exacte (pas de bruit
    //        « l'esprit » en tapant « les »).
    if (fp.size() >= 2 && fp.find('\'') == std::string::npos)
      elisionOffers(fp, CH_APOS, /*synth=*/exact == 0, scoreOf, offer);

    // (3) autocorrection noisy-channel (edit-distance 1) si l'exact est maigre.
    if (exact < size_t(k))
      fuzzyComplete(fp, scoreOf, offer);

    // (3bis) ESPACE OUBLIÉ : le préfixe se coupe en deux vrais mots dont le
    //        bigramme est OBSERVÉ (« dela » → « de la ») → l'expression est
    //        offerte, jamais auto-appliquée (garde-fou espace plus bas).
    //        Moitiés >= 2 lettres, jamais à travers apostrophe/trait d'union.
    if (fp.size() >= 4 && fp.find_first_of("'-") == std::string::npos) {
      for (size_t s = 2; s + 2 <= fp.size(); s++) {
        const std::string f1 = fp.substr(0, s), f2 = fp.substr(s);
        auto [l1, h1] = foldedPrefixRange(f1);
        for (auto it1 = l1; it1 != h1 && fold[*it1] == f1; ++it1) {
          auto a = biAdj.find(*it1);
          if (a == biAdj.end())
            continue;
          auto [l2, h2] = foldedPrefixRange(f2);
          for (auto it2 = l2; it2 != h2 && fold[*it2] == f2; ++it2)
            if (const float *p = CtxScorer::find(&a->second, *it2))
              offer(words[*it1] + " " + words[*it2],
                    scoreOf(*it1) * double(*p) * CH_SPLIT);
        }
      }
    }

    std::partial_sort(
        ranked.begin(),
        ranked.begin() + std::min<size_t>(k, ranked.size()), ranked.end(),
        [](auto &a, auto &b) { return a.second > b.second; });
    for (auto &p : ranked)
      if (push(p.first))
        res.scores.push_back(p.second); // parallèle à candidates (rerank E3)

    // GHOST — complétion haute confiance, TOUJOURS calculée (les candidats
    // restent affichés, on bride uniquement le remplacement automatique) :
    //  - préfixe assez long (un sigle de 2 lettres « az » ne devient pas
    //    « aziz ») ;
    //  - le top doit DOMINER le 2e candidat (ambigu → on garde le littéral,
    //    Tab choisit) — sauf expansion FORCÉE (snippet) ; un mot appris suit
    //    désormais la même règle de dominance (échelle modèle, amélioration A) ;
    //  - une correction FLOUE ne raccourcit jamais la frappe (« pcq » ne
    //    devient pas « pc ») et jamais à travers une apostrophe/trait d'union
    //    (on ne mutile pas une contraction, « j'ai » ≠ jail).
    // L'Espace ne l'applique que si autoApply (cf plus bas) ; sinon elle
    // reste un texte fantôme que → committe explicitement.
    if (!snippetExact.empty()) {
      res.ghost = snippetExact; // déclencheur explicite → toujours
    } else if (!res.candidates.empty() && fp.size() >= size_t(cfg.autoMinLen)) {
      const std::string &top = res.candidates.front();
      const std::string ftop = foldStr(top);
      bool topIsPrefix = ftop.compare(0, fp.size(), fp) == 0;
      bool fpHasPunct = fp.find_first_of("'-;") != std::string::npos;
      bool isForced = ranked.size() >= 1 && ranked[0].second >= 1e18; // snippet
      bool dominant =
          isForced || ranked.size() < 2 ||
          ranked[0].second >= cfg.autoDom * std::max(ranked[1].second, 1e-300);
      // jamais d'auto-application d'une expression À ESPACE (« de la ») : la
      // coupure espace-oublié se choisit explicitement (Tab), l'Espace garde
      // le littéral.
      bool fuzzyOk = ftop.size() >= fp.size() && !fpHasPunct &&
                     ftop.find(' ') == std::string::npos;
      if (dominant && (topIsPrefix || fuzzyOk))
        res.ghost = top;
    }

    // RESTAURATION D'ACCENTS : meilleure forme FOLD-EQUAL ≠ tapé — n'ajoute
    // que des accents/ligatures, jamais un autre mot, y compris à travers les
    // élisions (c'etait→c'était : même repli). Si la graphie brute existe
    // AUSSI dans le corpus (francais, garcon…), la forme accentuée doit la
    // dominer ×accentDom (faute d'accent probable vs vrai homographe) ; les
    // ligatures œ/æ sont restaurées sans seuil (oe/ae n'est jamais voulu).
    std::string accentWord;
    if (fp.size() >= 2) {
      const std::string typedLower = lowerKeep(prefix);
      double typedFreq = 0.0;
      auto tid = id_.find(typedLower);
      if (tid != id_.end())
        typedFreq = double(freq[tid->second]);
      uint32_t best = 0xFFFFFFFFu;
      double bestS = 0.0;
      for (auto it = lo; it != hi && fold[*it] == fp; ++it) {
        if (lowerKeep(words[*it]) == typedLower)
          continue; // le tapé lui-même
        double s = scoreOf(*it);
        if (s > bestS) {
          bestS = s;
          best = *it;
        }
      }
      if (best != 0xFFFFFFFFu) {
        std::string k = lowerKeep(words[best]);
        for (const auto &[lig, plain] :
             {std::pair<const char *, const char *>{"œ", "oe"},
              {"æ", "ae"}}) {
          size_t p;
          while ((p = k.find(lig)) != std::string::npos)
            k.replace(p, strlen(lig), plain);
        }
        bool ligOnly = (k == typedLower);
        bool dominant =
            typedFreq <= 0.0 ||
            double(freq[best]) >= cfg.accentDom * typedFreq;
        // GARDE ANTI-POUBELLE : la forme accentuée doit peser face au meilleur
        // candidat global — « cétait » (259 occurrences de bruit corpus) ne
        // court-circuite pas « c'était » (canal élision, largement devant).
        // Si le candidat accentué EST le top du classement, il est crédible
        // par définition — sans ce court-circuit, un mot APPRIS (score
        // plancher learnedFloor bien au-dessus de l'échelle modèle) se
        // comparait à lui-même et tuait sa propre restauration (« français »
        // appris une fois → « francais » ne se corrigeait plus).
        bool credible =
            ranked.empty() || words[best] == ranked[0].first ||
            bestS >= ACCENT_MIN_VS_TOP * ranked[0].second;
        if ((ligOnly || dominant) && credible)
          accentWord = words[best];
      }
    }

    // L'Espace applique : la complétion complète (autoApply), sinon la seule
    // restauration d'accents (accentRestore) — sinon rien, littéral gardé.
    if (cfg.autoApply) {
      // le garde apostrophe bloque les restaurations d'élision (c'était) dans
      // le ghost — l'accent fold-equal, sûr par construction, le complète.
      // Ghost == le littéral lui-même (graphie brute plus fréquente au corpus,
      // ex. « coeur » vs « cœur ») : sans intérêt pour l'Espace → on retombe
      // sur la restauration d'accents (la ligature doit gagner).
      bool ghostIsLiteral =
          !res.ghost.empty() && lowerKeep(res.ghost) == lowerKeep(prefix);
      res.autocomplete =
          (!res.ghost.empty() && !ghostIsLiteral) ? res.ghost : accentWord;
      // RESTAURATION pure = ne diffère du tapé que par accents et/ou
      // APOSTROPHES (« jai » → j'ai : mêmes lettres). L'engine peut alors
      // appliquer même si le tapé traîne dans le vocab comme bruit de corpus
      // (« jai » y est) — une restauration ne change jamais le mot.
      auto stripApos = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
        return s;
      };
      res.accentOnly = !res.autocomplete.empty() &&
                       stripApos(foldStr(res.autocomplete)) == stripApos(fp);
    } else if (cfg.accentRestore && !accentWord.empty()) {
      res.autocomplete = accentWord;
      res.accentOnly = true;
    }
    // RESTITUTION D'ACCENT (indépendante de autoApply) : si le mot tapé n'est
    // qu'une version DÉSACCENTUÉE d'un mot du modèle (même forme pliée, accents/
    // ligatures en plus), appliquer la forme accentuée sur Espace — on n'ajoute
    // QUE des accents, donc sûr même quand autoApply=false. Homographe (la
    // graphie nue est AUSSI au modèle, ex. ou/où, mur/mûr) : restituer seulement
    // si l'accentuée DOMINE le littéral par cfg.accentDom. On marque alors
    // literalIsWord=false : le littéral nu n'est pas la bonne graphie, l'engine
    // l'applique sur Espace (même chemin que l'autocomplétion emoji).
    if (cfg.accentRestore) {
      const std::string litLower = lowerKeep(prefix);
      double litScore = -1.0; // score du littéral nu s'il figure parmi les candidats
      for (auto &p : ranked)
        if (lowerKeep(p.first) == litLower) { litScore = p.second; break; }
      for (auto &p : ranked) {
        if (foldStr(p.first) != fp) continue;     // mêmes lettres seulement (pas une complétion)
        if (lowerKeep(p.first) == litLower) break; // top fold-égal = le littéral → graphie déjà bonne
        if (litScore >= 0.0 && p.second < cfg.accentDom * litScore) break; // homographe pas dominant
        res.autocomplete = p.first; // forme accentuée
        res.literalIsWord = false;  // le littéral désaccentué n'est pas la bonne graphie
        break;
      }
    }
    // VETO : remplacement déjà refusé par un revert → plus jamais auto.
    if (!res.autocomplete.empty()) {
      auto vIt = veto_.find(fp);
      if (vIt != veto_.end() && vIt->second.count(res.autocomplete)) {
        res.autocomplete.clear();
        res.accentOnly = false;
      }
    }

    // Suggestion emoji : le mot tapé est exactement un mot-clé emoji
    // ("coeur", "fire"…) → l'emoji s'ajoute en DERNIÈRE position (jamais en
    // tête, jamais auto-appliqué — il faut le choisir explicitement).
    if (fp.size() >= 3) {
      auto e = emojiExact_.find(fp);
      if (e != emojiExact_.end()) {
        const std::string &emo = emojis_[e->second];
        if ((int)res.candidates.size() >= k) {
          res.candidates.back() = emo;
          if (!res.scores.empty())
            res.scores.back() = 0.0; // jamais reranké au-dessus des mots
        } else {
          res.candidates.push_back(emo);
          res.scores.push_back(0.0);
        }
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
      addv(v, i == 0 ? CH_EXTRA_HEAD : CH_EXTRA);
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
    // Lettre OUBLIÉE : insérer chaque lettre à chaque position (« bonjur » →
    // « bonjour »). ~26·L variantes de plus — chaque lookup reste en µs, et le
    // fuzzy ne tourne déjà que quand les correspondances exactes sont maigres.
    for (size_t i = 0; i <= L; i++)
      for (char c = 'a'; c <= 'z'; c++) {
        std::string v = fp;
        v.insert(v.begin() + i, c);
        addv(v, CH_MISS);
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
      // ÉLISIONS composées : la variante corrigée peut aussi être une élision
      // sans apostrophe — « temener » → (insert m) → « temmener » → t'emmener
      // (synthèse t'+emmener). Deux fautes cumulées → canaux multipliés.
      // Synthèse seulement si la variante GARDE la 1re lettre tapée : une
      // substitution du proclitique lui-même fabriquait du bruit (« dici » →
      // variante « sici » → « s'ici », qui n'existe pas).
      if (v.find('\'') == std::string::npos)
        elisionOffers(v, ch * CH_APOS, /*synth=*/v[0] == fp[0], score, offer);
    }
  }

  // Mot-suivant : P_KN(w|u,v) sur l'union des suiveurs observés (trigramme +
  // bigramme) + le pool des meilleurs P1 (contexte inconnu → mots probables).
  // Contexte VIDE = début de phrase → contexte synthétique "<s>" (le builder
  // compte les bigrammes d'amorce). L'apprentissage utilisateur passe devant.
  template <class Push>
  void predictNext(const std::vector<std::string> &context, int k,
                   Push &&push, Result &res,
                   const std::vector<std::pair<std::string, double>>
                       &neuralCands = {}) {
    std::vector<std::string> ctx = context;
    if (ctx.empty())
      ctx.push_back("<s>");
    const std::string prev = lowerKeep(ctx.back());
    const uint8_t ctxL = ctxLang(ctx); // langue active (filtre strict ci-dessous)
    const Agree want = agreementOf(ctx); // contrainte d'accord (mot-suivant)

    // (1) bigrammes APPRIS (de confiance), sur l'ÉCHELLE DU MODÈLE (amélioration
    //     A) : score = max(P_KN(w|ctx), USER_BI_FLOOR) × confiance. Plus de
    //     priorité absolue — ils sont FUSIONNÉS au tri du modèle plus bas. Un
    //     bigramme appris faible ne coiffe plus un suiveur très probable
    //     (ne→pas .60) mais reste devant les suiveurs moyens.
    CtxScorer ctxScore;
    ctxScore.init(*this, ctx);
    std::vector<std::pair<std::string, double>> learned; // (mot appris, score)

    // (1a) trigrammes APPRIS (contexte 2 mots) : signal personnel plus spécifique
    //      → plancher plus haut (USER_TRI_FLOOR). Fusionnés comme les bigrammes ;
    //      à mot égal, le max gagne → le trigramme prime sur le bigramme.
    if (ctx.size() >= 2) {
      std::string triKey = lowerKeep(ctx[ctx.size() - 2]) + "\t" + prev;
      auto ut = userTri.find(triKey);
      if (ut != userTri.end())
        for (auto &p : ut->second) {
          if (!userTrusted(p.first, p.second) || langExcludedWord(p.first, ctxL))
            continue;
          auto idit = id_.find(lowerKeep(p.first));
          double ms = idit != id_.end() ? ctxScore(idit->second) : 0.0;
          double s = std::max(ms, USER_TRI_FLOOR) * learnedConf(p.second);
          if (idit != id_.end())
            s *= langFactor(ctxL, idit->second) *
                 agreeFactor(want, idit->second) * recFactor(idit->second);
          if (s > 0.0)
            learned.push_back({p.first, s});
        }
    }

    auto ub = userBi.find(prev);
    if (ub != userBi.end())
      for (auto &p : ub->second) {
        if (!userTrusted(p.first, p.second) || langExcludedWord(p.first, ctxL))
          continue;
        auto idit = id_.find(lowerKeep(p.first));
        double ms = idit != id_.end() ? ctxScore(idit->second) : 0.0;
        double s = std::max(ms, USER_BI_FLOOR) * learnedConf(p.second);
        if (idit != id_.end())
          s *= langFactor(ctxL, idit->second) * agreeFactor(want, idit->second) *
               recFactor(idit->second);
        if (s > 0.0)
          learned.push_back({p.first, s});
      }

    // (2) modèle : score exact P_KN(w|ctx) × boost de langue sur les suiveurs
    //     OBSERVÉS (tri ∪ bi), itérés SUR PLACE — les p stockés sont déjà les
    //     probabilités finales, pas besoin de repasser par operator().
    std::vector<std::pair<uint32_t, double>> v;
    v.reserve((ctxScore.tri ? ctxScore.tri->size() : 0) +
              (ctxScore.bi ? ctxScore.bi->size() : 0) + topUni_.size());
    if (ctxScore.tri)
      for (auto &pr : *ctxScore.tri)
        v.push_back({pr.first, pr.second * langFactor(ctxL, pr.first) *
                                   agreeFactor(want, pr.first) *
                                   recFactor(pr.first)});
    if (ctxScore.bi)
      for (auto &pr : *ctxScore.bi)
        if (!CtxScorer::find(ctxScore.tri, pr.first)) // déjà via trigramme
          v.push_back({pr.first, (ctxScore.hasUV ? ctxScore.g3 : 1.0) *
                                     pr.second * langFactor(ctxL, pr.first) *
                                     agreeFactor(want, pr.first) *
                                     recFactor(pr.first)});
    if (v.size() < size_t(k)) // contexte inconnu → pool des meilleurs P1
      for (uint32_t w : topUni_)
        if (!CtxScorer::find(ctxScore.tri, w) &&
            !CtxScorer::find(ctxScore.bi, w))
          v.push_back({w, ctxScore(w) * langFactor(ctxL, w) *
                              agreeFactor(want, w) * recFactor(w)});
    // Exclusion stricte de langue : jeter les suiveurs au facteur 0 (langue
    // opposée quand lang=fr/en) plutôt que de les laisser au fond du tri.
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const auto &p) { return p.second <= 0.0; }),
            v.end());
    size_t kk = std::min<size_t>(size_t(k) * 2, v.size());
    std::partial_sort(v.begin(), v.begin() + kk, v.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });

    // (2.5) NEURAL (E4) : les candidats neuronaux passent par le MÊME pipeline
    //     multiplicatif que tout le monde — langue stricte (exclusion), accord
    //     grammatical, puis fusion par score avec le modèle et l'appris. Fini le
    //     préfixage brut qui court-circuitait langFactor/agreeFactor/apprentissage.
    std::vector<std::pair<std::string, double>> nsc;
    for (const auto &[w, p] : neuralCands) {
      double f = 1.0;
      auto idit = id_.find(lowerKeep(w));
      if (idit != id_.end())
        f = langFactor(ctxL, idit->second) * agreeFactor(want, idit->second) *
            recFactor(idit->second);
      double s = p * cfg.neuralBoost * f;
      if (s > 0.0)
        nsc.push_back({w, s});
    }

    // neuralOnly = mot-suivant 100% neuronal (n-gram/appris écartés) — mais
    // toujours filtré langue + accord, et trié par score.
    if (cfg.neuralOnly && !nsc.empty()) {
      std::sort(nsc.begin(), nsc.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      for (auto &p : nsc)
        push(p.first);
      return;
    }

    // (3) FUSION appris + modèle + neural : on ne passe aux chaînes que pour le
    //     PETIT haut de liste (kk) plus les bigrammes appris (poignée) — le hot
    //     path entier (itération des milliers de suiveurs) reste intact.
    //     Déduplique par mot en gardant le meilleur score, puis retri global.
    std::vector<std::pair<std::string, double>> cand;
    std::unordered_map<std::string, size_t> at;
    auto add = [&](const std::string &w, double s) {
      auto [it, fresh] = at.try_emplace(w, cand.size());
      if (fresh)
        cand.push_back({w, s});
      else if (s > cand[it->second].second)
        cand[it->second].second = s;
    };
    for (size_t i = 0; i < kk; i++)
      add(words[v[i].first], v[i].second);
    for (auto &p : learned)
      add(p.first, p.second);
    for (auto &p : nsc)
      add(p.first, p.second);
    size_t ck = std::min<size_t>(size_t(k) * 2, cand.size());
    std::partial_sort(cand.begin(), cand.begin() + ck, cand.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });

    // (4) MULTI-MOTS : si le meilleur candidat (mot du modèle) a une
    //     continuation très sûre (P >= MULTI_MIN), proposer l'expression
    //     entière en FIN de barre (« sais pas ») — sans déplacer le top.
    std::string phrase;
    if (cfg.multiWord && ck > 0) {
      auto topId = id_.find(lowerKeep(cand[0].first));
      if (topId != id_.end()) {
        std::vector<std::string> c2(ctx);
        c2.push_back(words[topId->second]);
        if (c2.size() > 2)
          c2.erase(c2.begin(), c2.end() - 2);
        CtxScorer s2;
        s2.init(*this, c2);
        uint32_t best = 0;
        double bp = 0;
        if (s2.tri)
          for (auto &pr : *s2.tri)
            if (pr.second > bp) {
              bp = pr.second;
              best = pr.first;
            }
        if (s2.bi)
          for (auto &pr : *s2.bi)
            if (pr.second > bp) {
              bp = pr.second;
              best = pr.first;
            }
        if (bp >= MULTI_MIN)
          phrase = cand[0].first + ' ' + words[best];
      }
    }
    for (size_t i = 0; i < ck; i++)
      push(cand[i].first);
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
  model.loadMorph(dir);
  model.indexNgrams();
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
  model.loadUserTri(userDir + "/user.tri.log");
  model.loadVeto(userDir + "/veto.log");

  // config/dictionnaire perso/snippets ($XDG_CONFIG_HOME/ime-predictord),
  // rechargés à chaud sur mtime à chaque requête.
  const char *xdgc = getenv("XDG_CONFIG_HOME");
  model.cfgDir_ = (xdgc ? std::string(xdgc)
                        : std::string(home ? home : "/tmp") + "/.config") +
                  "/ime-predictord";
  model.maybeReload();

#ifdef WITH_NEURAL
  // Prédicteur neuronal chargé UNE fois au démarrage si activé en config. Le
  // modèle (~Go) ne se recharge pas à chaud ; l'usage par requête revérifie
  // model.cfg.neural (toggle OFF possible à chaud, ON nécessite un restart).
  NeuralPredictor neural;
  // neuralForced : le service (module NixOS) a fourni IME_NEURAL_MODEL → intention
  // explicite d'activer le neural, SANS exiger neural:true dans le config.json perso.
  bool neuralForced = false;
  {
    // Chemin du GGUF : config.json (neuralModel) sinon env IME_NEURAL_MODEL — ce
    // dernier laisse le service systemd fournir le modèle SANS hardcoder un
    // store-path dans la config perso (déploiement propre via le module NixOS).
    const char *envModel = getenv("IME_NEURAL_MODEL");
    neuralForced = (envModel != nullptr && envModel[0] != '\0');
    std::string nmodel = !model.cfg.neuralModel.empty()
                             ? model.cfg.neuralModel
                             : (neuralForced ? std::string(envModel) : std::string());
    if ((model.cfg.neural || neuralForced) && !nmodel.empty()) {
      const char *bdir = getenv("GGML_BACKEND_PATH");
      if (neural.init(nmodel, model.cfg.neuralThreads, 2048, bdir ? bdir : ""))
        fprintf(stderr, "[predictord] neural ON: %s (threads=%d, topk=%d)\n",
                nmodel.c_str(), model.cfg.neuralThreads, model.cfg.neuralTopk);
      else
        fprintf(stderr, "[predictord] neural model load FAILED (%s) — n-gram seul\n",
                nmodel.c_str());
    }
  }
#endif

  // ---- LIGNES DIFFÉRÉES : les workers (reformulation, refresh neural)
  // déposent ici des réponses toutes prêtes ; le pipe réveille poll() et le
  // thread principal les recopie dans le out-buffer du client visé (flush au
  // POLLOUT). Un client parti entre-temps = ligne jetée. ----
  struct DeferredLine {
    uint64_t client;
    std::string line;
  };
  std::mutex dMu;
  std::vector<DeferredLine> dReady;
  int wakePipe[2] = {-1, -1};
  if (pipe(wakePipe) == 0) {
    fcntl(wakePipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe[1], F_SETFL, O_NONBLOCK);
  }
  auto postLine = [&](uint64_t client, std::string line) {
    {
      std::lock_guard<std::mutex> lk(dMu);
      dReady.push_back({client, std::move(line)});
    }
    ssize_t n = write(wakePipe[1], "x", 1);
    (void)n;
  };

  // ---- REFORMULATION sur worker (A1) : l'appel Groq (secondes de réseau) ou
  // la génération locale ne doivent JAMAIS bloquer le poll loop — pendant une
  // reformulation, la complétion continue pour toutes les applis. File FIFO ;
  // un nouveau job du MÊME client remplace celui encore en file (cycling de
  // modes = seule la dernière demande compte). Le worker ne touche jamais
  // Model : il reçoit un instantané de la config dans le job.
  // CACHE LRU (A3), partagé thread principal/worker sous rjMu : redemander un
  // (texte, mode, nonce, n) déjà généré répond immédiatement, sans worker —
  // revenir sur un mode déjà visité est instantané et gratuit.
  struct ReformJob {
    uint64_t client = 0;
    std::string sentence, mode, baseUrl, model, cfgDir;
    int n = 3, timeoutMs = 8000;
    uint32_t nonce = 0;
    // VALIDATION de clé (panneau « fournir la clé ») : appel Groq minimal
    // (1 variante d'une phrase fixe) juste pour vérifier présence + validité.
    bool checkOnly = false;
  };
  struct ReformHit {
    std::string key;
    std::vector<std::string> variants;
    std::string source;
  };
  std::mutex rjMu; // garde la file ET le cache
  std::condition_variable rjCv;
  std::deque<ReformJob> rjQueue;
  std::deque<ReformHit> reformCache;
  auto reformKey = [](const std::string &s, const std::string &mode,
                      uint32_t nonce, int n) {
    return s + '\x1f' + mode + '\x1f' + std::to_string(nonce) + '\x1f' +
           std::to_string(n);
  };
  std::thread rjWorker([&] {
    for (;;) {
      ReformJob job;
      {
        std::unique_lock<std::mutex> lk(rjMu);
        rjCv.wait(lk, [&] { return !rjQueue.empty(); });
        job = std::move(rjQueue.front());
        rjQueue.pop_front();
      }
      // GROQ SEULEMENT : la qualité d'abord — le repli local (GGUF du
      // mot-suivant) produisait des variantes inutilisables. En cas d'échec,
      // `error` dit pourquoi et l'engine l'affiche en panneau compact
      // (no_key/auth → inviter à configurer la clé ; network/http → message).
      std::string kind = "ok";
      std::vector<std::string> variants = reformulateHttp(
          job.sentence, job.n, job.baseUrl, job.model, job.cfgDir,
          job.timeoutMs, job.mode, job.nonce, &kind);
      if (job.checkOnly) {
        // Validation de clé : seule la CAUSE compte, pas les variantes.
        json resp;
        resp["reformCheck"] = true;
        resp["keyPresent"] = kind != "no_key";
        // « empty » = HTTP 200 sans variante exploitable → la clé est bonne.
        resp["keyValid"] = kind == "ok" || kind == "empty";
        resp["error"] = kind;
        postLine(job.client, resp.dump() + "\n");
        continue;
      }
      std::string source = variants.empty() ? "none" : "groq";
      json resp;
      resp["variants"] = variants;
      resp["source"] = source;
      resp["error"] = kind;
      std::string out;
      try {
        out = resp.dump() + "\n";
      } catch (...) {
        out = "{\"variants\":[]}\n";
      }
      postLine(job.client, std::move(out));
      if (!variants.empty()) { // jamais les échecs (réseau coupé ≠ définitif)
        std::lock_guard<std::mutex> lk(rjMu);
        reformCache.push_front(
            {reformKey(job.sentence, job.mode, job.nonce, job.n), variants,
             source});
        if (reformCache.size() > 8)
          reformCache.pop_back();
      }
    }
  });
  rjWorker.detach(); // vit avec le process (le daemon s'arrête par signal)

#ifdef WITH_NEURAL
  // ---- Deux phases ASYNC (E5) : un thread de travail pour le neural. ----
  // Le thread ne touche JAMAIS Model (appris/config/lexique mutent sur le
  // thread principal) : il ne parle qu'à NeuralPredictor (verrouillé en
  // interne). Un seul job en attente (le clavier n'a qu'un curseur) — un
  // nouveau job remplace l'ancien. Résultats repostés au thread principal via
  // le pipe de réveil partagé, la FUSION (predict) reste donc mono-thread.
  struct NeuralJob {
    uint64_t client = 0;
    std::string wide;
    std::vector<std::string> ctx;
    int topk = 6;
    int budgetMs = 180;
  };
  struct NeuralDone {
    uint64_t client;
    std::string wide; // re-sert au cache de récence lors de la fusion
    std::vector<std::string> ctx;
    std::vector<std::pair<std::string, double>> cands;
  };
  std::mutex njMu;
  std::condition_variable njCv;
  NeuralJob njPending;
  bool njHas = false, njQuit = false;
  std::vector<NeuralDone> njDone;
  std::thread njWorker([&] {
    for (;;) {
      NeuralJob job;
      {
        std::unique_lock<std::mutex> lk(njMu);
        njCv.wait(lk, [&] { return njHas || njQuit; });
        if (njQuit)
          return;
        job = njPending;
        njHas = false;
      }
      auto t0 = std::chrono::steady_clock::now();
      auto nc = neural.nextWords(job.wide, job.topk, job.budgetMs);
      std::vector<std::pair<std::string, double>> out;
      for (const auto &c : nc) {
        std::string w = c.word;
        // Heuristique SANS lexique (le lexique vit sur l'autre thread) : une
        // ou deux lettres, ou finale en apostrophe = fragment BPE probable
        // (« l » de « l'école ») → expansion. Couvre les élisions observées.
        bool fragmentish =
            w.size() <= 2 || w.back() == '\'';
        if (fragmentish) {
          int remain =
              job.budgetMs -
              (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
          w = remain > 60 ? neural.expand(c, 3, remain) : std::string{};
        }
        if (!w.empty())
          out.push_back({w, (double)c.prob});
      }
      {
        std::lock_guard<std::mutex> lk(njMu);
        njDone.push_back({job.client, job.wide, job.ctx, std::move(out)});
      }
      ssize_t n = write(wakePipe[1], "x", 1);
      (void)n;
    }
  });
#endif

  std::string sockpath = argc > 2 ? argv[2] : "/tmp/ime-predictord.sock";
  unlink(sockpath.c_str());
  int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockpath.c_str(), sizeof(addr.sun_path) - 1);
  if (bind(srv, (sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }
  listen(srv, 16);
  fprintf(stderr, "[predictord] écoute sur %s\n", sockpath.c_str());

  // Traite UNE ligne de protocole, renvoie la réponse ('\n' terminée).
  // clientSeq : identité stable du client (le refresh async lui est adressé).
  auto handleLine = [&](const std::string &line,
                        uint64_t clientSeq) -> std::string {
    (void)clientSeq;
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
      } else if (req.contains("reformCheck")) {
        // VALIDATION de clé Groq (panneau « fournir la clé » + dialogue
        // ime-preferences --groq-key) : appel minimal sur le worker, réponse
        // différée {"reformCheck":true,"keyPresent":…,"keyValid":…,"error":…}.
        model.maybeReload();
        {
          std::lock_guard<std::mutex> lk(rjMu);
          ReformJob job;
          job.client = clientSeq;
          job.checkOnly = true;
          job.sentence = "Bonjour tout le monde."; // phrase fixe, n=1 : minimal
          job.mode = "rephrase";
          job.n = 1;
          job.baseUrl = model.cfg.reformBaseUrl;
          job.model = model.cfg.reformModel;
          job.cfgDir = model.cfgDir_;
          job.timeoutMs = std::min(model.cfg.reformTimeoutMs, 6000);
          rjQueue.push_back(std::move(job));
        }
        rjCv.notify_one();
        return std::string{}; // réponse différée (postLine du worker)
      } else if (req.contains("reformulate")) {
        // Reformulation à la demande (sélection → variantes), traitée sur le
        // WORKER : Groq (secondes de réseau) ou génération locale ne bloquent
        // jamais le poll loop — la complétion continue pendant ce temps. La
        // réponse part en DIFFÉRÉ sur la même connexion (postLine), précédée
        // de lignes partial:true en streaming local. Cache LRU : un (texte,
        // mode, nonce, n) déjà généré répond immédiatement (cycling de modes).
        std::string sentence = req.value("reformulate", std::string{});
        int n = req.value("n", 3);
        std::string mode = req.value("mode", std::string{"rephrase"});
        uint32_t nonce = (uint32_t)req.value("nonce", 0);
        model.maybeReload(); // baseUrl/model/timeout à chaud
        std::vector<uint64_t> superseded;
        {
          std::lock_guard<std::mutex> lk(rjMu);
          const std::string key = reformKey(sentence, mode, nonce, n);
          for (auto it = reformCache.begin(); it != reformCache.end(); ++it)
            if (it->key == key) {
              resp["variants"] = it->variants;
              resp["source"] = it->source;
              ReformHit hit = *it; // move-to-front (LRU)
              reformCache.erase(it);
              reformCache.push_front(std::move(hit));
              try {
                return resp.dump() + "\n";
              } catch (...) {
                return std::string("{\"variants\":[]}\n");
              }
            }
          ReformJob job;
          job.client = clientSeq;
          job.sentence = sentence;
          job.mode = mode;
          job.n = n;
          job.nonce = nonce;
          job.baseUrl = model.cfg.reformBaseUrl;
          job.model = model.cfg.reformModel;
          job.cfgDir = model.cfgDir_;
          job.timeoutMs = model.cfg.reformTimeoutMs;
          // Un job encore en file pour la MÊME phrase est périmé (l'engine
          // ouvre une connexion par demande : cycler les modes = plusieurs
          // clients, seule la dernière demande compte). L'évincé reçoit une
          // réponse « superseded » immédiate — son thread engine se termine
          // au lieu d'attendre son timeout ; le compteur de génération côté
          // engine la jette de toute façon.
          for (auto it = rjQueue.begin(); it != rjQueue.end();) {
            if (!it->checkOnly && it->sentence == sentence) {
              superseded.push_back(it->client);
              it = rjQueue.erase(it);
            } else {
              ++it;
            }
          }
          rjQueue.push_back(std::move(job));
        }
        for (uint64_t c : superseded)
          postLine(c,
                   "{\"variants\":[],\"source\":\"none\",\"error\":"
                   "\"superseded\"}\n");
        rjCv.notify_one();
        return std::string{}; // réponse différée (postLine du worker)
      } else {
        std::vector<std::string> ctx =
            req.value("context", std::vector<std::string>{});
        std::string prefix = req.value("prefix", std::string{});
        model.maybeReload(); // config/dict/snippets à chaud (mtime)
        // Contexte LARGE (E1) : texte brut avant le curseur, phrases
        // précédentes comprises — réservé au neural (le n-gram garde le
        // contexte borné à la phrase). Repli : les mots du contexte joints.
        std::string wide = req.value("wide", std::string{});
        if (wide.empty())
          for (const auto &w : ctx) {
            if (!wide.empty())
              wide += ' ';
            wide += w;
          }
#ifdef WITH_NEURAL
        // Mot-suivant neuronal (E1/E2/E4) : candidats scorés sur le contexte
        // large, fragments BPE complétés (« l » → « l'école »), puis FUSION
        // par score dans predict() (langue/accord/appris respectés).
        bool neuralWants = neural.ready() &&
                           (model.cfg.neural || neuralForced) &&
                           prefix.empty() && !wide.empty();
        // Deux phases (E5) : l'engine a demandé "async" → n-gram tout de
        // suite (pending:true), neural sur le thread de travail, refresh sur
        // la même connexion dès qu'il aboutit.
        bool deferred = neuralWants && model.cfg.asyncNeural &&
                        req.value("async", false) && wakePipe[1] >= 0;
        if (deferred) {
          {
            std::lock_guard<std::mutex> lk(njMu);
            njPending = {clientSeq, wide, ctx, model.cfg.neuralTopk,
                         model.cfg.neuralBudgetMs};
            njHas = true; // remplace un éventuel job périmé (latest-only)
          }
          njCv.notify_one();
          resp["pending"] = true;
        }
        std::vector<std::pair<std::string, double>> neuralCands;
        if (neuralWants && !deferred) {
          auto t0 = std::chrono::steady_clock::now();
          auto nc =
              neural.nextWords(wide, model.cfg.neuralTopk, model.cfg.neuralBudgetMs);
          for (const auto &c : nc) {
            std::string w = c.word;
            if (model.looksFragment(w)) {
              int remain =
                  model.cfg.neuralBudgetMs -
                  (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
              // ~1 décde minimum ; sous ça on jette le fragment plutôt que
              // d'offrir un demi-mot.
              w = remain > 60 ? neural.expand(c, 3, remain) : std::string{};
            }
            if (!w.empty())
              neuralCands.push_back({w, (double)c.prob});
          }
        }
        model.setRecency(wide, ctx);
        Result r = model.predict(ctx, prefix, model.cfg.barWords, neuralCands);
        // Rerank neuronal de la complétion (E3) — opportuniste : seulement si
        // le cache KV est déjà chaud pour ce contexte exact (le mot-suivant
        // vient de tourner dessus). Mélange géométrique log-linéaire.
        if (neural.ready() && (model.cfg.neural || neuralForced) &&
            model.cfg.neuralRerank && !prefix.empty() && prefix[0] != ':' &&
            !wide.empty() && r.candidates.size() >= 2 &&
            r.scores.size() == r.candidates.size()) {
          std::vector<float> lp;
          if (neural.scoreFirstTokens(wide, r.candidates, lp)) {
            const double lam = model.cfg.rerankWeight;
            std::vector<size_t> ord(r.candidates.size());
            std::vector<double> blend(r.candidates.size());
            for (size_t i = 0; i < ord.size(); i++) {
              ord[i] = i;
              blend[i] = (1.0 - lam) * std::log(std::max(r.scores[i], 1e-300)) +
                         lam * (double)lp[i];
            }
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
              return blend[a] > blend[b];
            });
            std::vector<std::string> rc;
            rc.reserve(ord.size());
            for (size_t i : ord)
              rc.push_back(r.candidates[i]);
            r.candidates.swap(rc);
            // autocomplete inchangé : la sémantique d'auto-application reste
            // 100% n-gram (conservatrice) — le rerank ne réordonne que la barre.
          }
        }
#else
        model.setRecency(wide, ctx);
        Result r = model.predict(ctx, prefix, model.cfg.barWords);
#endif
        // Plafond DUR de la barre de mots : seul le top-N (cfg.barWords) le plus
        // pertinent est montré. predict() le respecte déjà côté n-gram ; on le
        // RÉ-applique ICI, après la fusion neuronale — qui empile neuralTopk +
        // n-gram et peut dépasser N. La liste arrive déjà triée par pertinence
        // (n-gram par score, neural en tête), donc tronquer = garder les N
        // meilleures. La GRILLE emoji (préfixe ':') est EXEMPTÉE : c'est une
        // grille, pas la barre — elle conserve ses ≤24 candidats.
        bool emojiGrid = !prefix.empty() && prefix[0] == ':';
        if (!emojiGrid && (int)r.candidates.size() > model.cfg.barWords)
          r.candidates.resize(model.cfg.barWords);
        resp["candidates"] = r.candidates;
        resp["literalIsWord"] = r.literalIsWord;
        resp["autocomplete"] = r.autocomplete;
        resp["ghost"] = r.ghost;
        resp["accentOnly"] = r.accentOnly;
      }
    } catch (const std::exception &e) {
      resp["candidates"] = json::array();
      resp["error"] = e.what();
    }
    // dump() PEUT lever (ex. chaîne UTF-8 incomplète d'un candidat) — hors du try
    // ci-dessus : on le borne ici pour qu'aucune réponse ne puisse tuer le daemon.
    try {
      return resp.dump() + "\n";
    } catch (...) {
      return std::string("{\"candidates\":[]}\n");
    }
  };

  // Boucle poll() mono-thread MULTI-CLIENTS : un client lent ou resté ouvert
  // (un `nc -U` interactif, un process suspendu) ne bloque plus les autres.
  // L'ancienne boucle accept→read servait UNE connexion jusqu'à sa fermeture :
  // l'engine — synchrone sur le thread clavier de fcitx — attendait derrière.
  struct Client {
    int fd;
    uint64_t seq; // identité stable (l'fd peut être réutilisé après close)
    std::string in, out;
  };
  std::vector<Client> clients;
  std::vector<pollfd> pfds;
  uint64_t clientSeq = 0;
  for (;;) {
    pfds.clear();
    pfds.push_back({srv, POLLIN, 0});
    // fd 1 du tableau : le pipe de réveil des workers (reformulation, neural).
    pfds.push_back({wakePipe[0], POLLIN, 0});
    constexpr size_t kFixed = 2;
    for (auto &cl : clients)
      pfds.push_back(
          {cl.fd, short(POLLIN | (cl.out.empty() ? 0 : POLLOUT)), 0});
    if (poll(pfds.data(), nfds_t(pfds.size()), -1) < 0)
      continue;
    size_t nOld = clients.size();
    if (pfds[0].revents & POLLIN)
      for (;;) {
        int c = accept(srv, nullptr, nullptr);
        if (c < 0)
          break; // EAGAIN : plus personne en attente
        fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
        clients.push_back({c, ++clientSeq, {}, {}}); // servi au prochain tour
      }
    if (pfds[1].revents & POLLIN) {
      char sink[64];
      while (read(wakePipe[0], sink, sizeof(sink)) > 0)
        ;
      // Lignes différées (reformulation — A1/A2) : recopiées telles quelles
      // dans le out-buffer du client visé (flush au POLLOUT du même tour ou
      // du suivant). Client parti = ligne jetée.
      std::vector<DeferredLine> lines;
      {
        std::lock_guard<std::mutex> lk(dMu);
        lines.swap(dReady);
      }
      for (auto &dl : lines)
        for (auto &cl : clients)
          if (cl.seq == dl.client && cl.fd >= 0) {
            cl.out += dl.line;
            break;
          }
#ifdef WITH_NEURAL
      std::vector<NeuralDone> ready;
      {
        std::lock_guard<std::mutex> lk(njMu);
        ready.swap(njDone);
      }
      // FUSION sur le thread principal (accès Model exclusif) puis refresh au
      // client d'origine — s'il est parti entre-temps, on jette.
      for (auto &d : ready) {
        Client *dst = nullptr;
        for (auto &cl : clients)
          if (cl.seq == d.client && cl.fd >= 0) {
            dst = &cl;
            break;
          }
        if (!dst)
          continue;
        model.setRecency(d.wide, d.ctx);
        Result r = model.predict(d.ctx, "", model.cfg.barWords, d.cands);
        json resp;
        resp["candidates"] = r.candidates;
        resp["refresh"] = true;
        try {
          dst->out += resp.dump() + "\n";
        } catch (...) {
        }
        // le flush partira au POLLOUT du prochain tour (out non vide).
      }
#endif
    }
    for (size_t i = 0; i < nOld; i++) {
      Client &cl = clients[i];
      short ev = pfds[i + kFixed].revents;
      if (!ev)
        continue;
      bool eof = false, drop = false;
      if (ev & (POLLIN | POLLHUP | POLLERR)) {
        char tmp[4096];
        for (;;) {
          ssize_t n = read(cl.fd, tmp, sizeof(tmp));
          if (n > 0) {
            cl.in.append(tmp, n);
            if (cl.in.size() > (64u << 10)) { // ligne sans fin : abus
              drop = true;
              break;
            }
            continue;
          }
          if (n == 0)
            eof = true;
          else if (errno != EAGAIN && errno != EWOULDBLOCK)
            drop = true;
          break;
        }
      }
      size_t nl;
      while (!drop && (nl = cl.in.find('\n')) != std::string::npos) {
        std::string line = cl.in.substr(0, nl);
        cl.in.erase(0, nl + 1);
        cl.out += handleLine(line, cl.seq);
      }
      // flush non bloquant ; le reste partira sur POLLOUT. EPIPE = client
      // parti sans lire (learn fire-and-forget de l'engine) : on jette.
      while (!drop && !cl.out.empty()) {
        ssize_t n = send(cl.fd, cl.out.data(), cl.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
          cl.out.erase(0, size_t(n));
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          break;
        drop = true;
        break;
      }
      if (cl.out.size() > (1u << 20))
        drop = true; // lecteur trop lent : on ne tamponne pas à l'infini
      if (drop || (eof && cl.out.empty())) {
        close(cl.fd);
        cl.fd = -1;
      }
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [](const Client &c) { return c.fd < 0; }),
                  clients.end());
  }
}
