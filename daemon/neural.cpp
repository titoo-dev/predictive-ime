#include "neural.h"
#include "llama.h"
#include "ggml-backend.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>

static std::vector<llama_token> tokenize_(const llama_vocab *v, const std::string &t,
                                          bool bos, bool special = false) {
  int n = -llama_tokenize(v, t.c_str(), (int)t.size(), nullptr, 0, bos, special);
  std::vector<llama_token> toks(n > 0 ? n : 0);
  int got = llama_tokenize(v, t.c_str(), (int)t.size(), toks.data(), (int)toks.size(), bos, special);
  if (got < 0) toks.clear(); else toks.resize(got);
  return toks;
}

static std::string piece_(const llama_vocab *v, llama_token t) {
  char buf[256];
  int n = llama_token_to_piece(v, t, buf, sizeof(buf), 0, false);
  return n < 0 ? std::string() : std::string(buf, n);
}

static bool word_punct_(char c) {
  return c == '\n' || std::strchr(".,;:!?\"'()[]{}<>/\\|`~@#$%^&*+=", c) != nullptr;
}

// Byte-level BPE can split a multi-byte char across tokens → a single token's
// piece may be INCOMPLETE UTF-8. Such a string crashes nlohmann::json::dump()
// (type_error.316) and would kill the daemon. Reject non-UTF-8 candidates.
static bool valid_utf8(const std::string &s) {
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

// Langue de la phrase (FR par défaut = langue primaire de l'utilisateur). Sert à
// ÉPINGLER la langue de sortie : sinon le modèle, instruit en français, TRADUISAIT
// les phrases anglaises en français. Heuristique : accents latins fréquents en FR
// → FR certain ; sinon vote des mots vides FR vs EN.
static bool isFrench(const std::string &s) {
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if ((unsigned char)s[i] == 0xC3) {
      unsigned char b = (unsigned char)s[i + 1];
      // é è ê ë à â ç ô ö ù û ü î ï (formes minuscules courantes)
      if (b == 0xA9 || b == 0xA8 || b == 0xAA || b == 0xAB || b == 0xA0 ||
          b == 0xA2 || b == 0xA7 || b == 0xB4 || b == 0xB6 || b == 0xB9 ||
          b == 0xBB || b == 0xBC || b == 0xAE || b == 0xAF)
        return true; // un accent FR → quasi-certain français
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
                             " ce ", " sur ", " au ", " du ", " ça ", nullptr};
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
  return count(fr) >= count(en); // égalité → FR (langue primaire)
}

// Échantillonnage température + top-k + top-p (nucleus) sur les logits. Le greedy
// (argmax) renvoyait des variantes IDENTIQUES — l'échantillonnage est ce qui les
// rend distinctes. Pré-filtre top-k (sélection partielle, comme nextWords) pour
// éviter de trier les ~150k tokens du vocab à chaque pas.
static llama_token sampleTok(const float *logits, int n_vocab, float temp,
                             float topP, int topK, std::mt19937 &rng) {
  if (topK > n_vocab) topK = n_vocab;
  std::vector<int> idx(topK, -1);
  std::vector<float> val(topK, -1e30f); // triés décroissants par insertion
  for (int v = 0; v < n_vocab; ++v) {
    float l = logits[v];
    if (l > val[topK - 1]) {
      int j = topK - 1;
      while (j > 0 && val[j - 1] < l) {
        val[j] = val[j - 1];
        idx[j] = idx[j - 1];
        --j;
      }
      val[j] = l;
      idx[j] = v;
    }
  }
  float invT = temp > 1e-4f ? 1.0f / temp : 1.0f;
  std::vector<double> p(topK);
  double sum = 0.0;
  for (int i = 0; i < topK; ++i) {
    p[i] = std::exp((double)(val[i] - val[0]) * invT);
    sum += p[i];
  }
  if (sum <= 0.0)
    return idx[0];
  double cum = 0.0;
  int cut = topK;
  for (int i = 0; i < topK; ++i) {
    cum += p[i];
    if (cum / sum >= topP) {
      cut = i + 1;
      break;
    }
  }
  double r = std::uniform_real_distribution<double>(0.0, cum)(rng);
  double acc = 0.0;
  for (int i = 0; i < cut; ++i) {
    acc += p[i];
    if (acc >= r)
      return idx[i];
  }
  return idx[cut - 1];
}

// Lecture d'un float depuis une variable d'env (réglage à chaud sans rebuild).
static float envF(const char *k, float def) {
  const char *v = getenv(k);
  if (!v || !*v)
    return def;
  char *end = nullptr;
  float f = std::strtof(v, &end);
  return end != v ? f : def;
}

bool NeuralPredictor::init(const std::string &modelPath, int threads, int nCtx,
                           const std::string &backendDir) {
  if (!backendDir.empty()) ggml_backend_load_all_from_path(backendDir.c_str());
  else ggml_backend_load_all();
  llama_backend_init();

  llama_model_params mp = llama_model_default_params();
  mp.n_gpu_layers = 0;
  llama_model *m = llama_model_load_from_file(modelPath.c_str(), mp);
  if (!m) return false;
  model_ = m;
  const llama_vocab *vo = llama_model_get_vocab(m);
  vocab_ = vo;

  llama_context_params cp = llama_context_default_params();
  cp.n_ctx = nCtx;
  cp.n_threads = threads;
  cp.n_threads_batch = threads;
  llama_context *c = llama_init_from_model(m, cp);
  if (!c) return false;
  ctx_ = c;
  mem_ = llama_get_memory(c);
  n_vocab_ = llama_vocab_n_tokens(vo);
  return true;
}

std::vector<std::string> NeuralPredictor::nextWords(const std::vector<std::string> &context, int k) {
  if (!ctx_ || k < 1) return {};
  std::string text;
  for (size_t i = 0; i < context.size(); ++i) { if (i) text += ' '; text += context[i]; }
  if (text.empty()) return {};

  const llama_vocab *vo = (const llama_vocab *)vocab_;
  llama_context *c = (llama_context *)ctx_;
  llama_memory_t mem = (llama_memory_t)mem_;

  std::vector<llama_token> toks = tokenize_(vo, text, /*bos=*/true);
  if (toks.empty()) return {};

  // incremental KV: keep the longest common prefix, decode only the new tail
  int lcp = 0;
  while (lcp < (int)prev_.size() && lcp < (int)toks.size() && prev_[lcp] == toks[lcp]) ++lcp;
  if (lcp < (int)prev_.size()) llama_memory_seq_rm(mem, 0, lcp, -1);
  int n_new = (int)toks.size() - lcp;
  if (n_new > 0) {
    llama_batch b = llama_batch_get_one(toks.data() + lcp, n_new);
    if (llama_decode(c, b) != 0) { prev_.clear(); llama_memory_clear(mem, true); return {}; }
  }
  prev_.assign(toks.begin(), toks.end());

  float *logits = llama_get_logits_ith(c, -1);
  if (!logits) return {};
  const int N = k * 8;
  std::vector<int> idx(N, -1);
  std::vector<float> val(N, -1e30f);
  for (int v = 0; v < n_vocab_; ++v) {
    float l = logits[v];
    if (l > val[N - 1]) {
      int j = N - 1;
      while (j > 0 && val[j - 1] < l) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; --j; }
      val[j] = l; idx[j] = v;
    }
  }
  std::vector<std::string> out;
  for (int i = 0; i < N && (int)out.size() < k; ++i) {
    if (idx[i] < 0) break;
    std::string p = piece_(vo, idx[i]);
    if (p.empty() || p[0] != ' ') continue;          // word-initial tokens only
    std::string w = p.substr(1);
    if (w.empty() || word_punct_(w[0]) || !valid_utf8(w)) continue; // skip BPE byte-fragments
    bool dup = false; for (auto &o : out) if (o == w) { dup = true; break; }
    if (!dup) out.push_back(w);
  }
  return out;
}

std::vector<std::string> NeuralPredictor::reformulate(const std::string &sentence, int n) {
  if (!ctx_ || sentence.empty()) return {};
  if (n < 1) n = 3;
  const llama_vocab *vo = (const llama_vocab *)vocab_;
  llama_context *c = (llama_context *)ctx_;
  llama_memory_t mem = (llama_memory_t)mem_;

  // On SUR-GÉNÈRE (n+2 lignes) puis on dédoublonne : l'échantillonnage produit
  // parfois deux lignes proches, mieux vaut avoir une marge.
  const int want = n + 2;

  // ÉPINGLAGE DE LANGUE : le prompt rédigé DANS la langue cible ancre la sortie
  // (un prompt français faisait traduire les phrases anglaises en français).
  bool fr = isFrench(sentence);
  std::string sys =
      fr ? ("Tu reformules. Réécris la phrase de l'utilisateur en " +
            std::to_string(want) +
            " reformulations différentes, une par ligne. Garde EXACTEMENT le "
            "même sens et la même langue (français) — ne traduis pas. Conserve la "
            "ponctuation finale (. ? !) de la phrase. Varie la formulation. Pas de "
            "numéro, pas de commentaire, pas de guillemets.")
         : ("You rephrase text. Rewrite the user's sentence as " +
            std::to_string(want) +
            " different paraphrases, one per line. Keep EXACTLY the same meaning "
            "and the same language (English) — do not translate. Keep the "
            "sentence's final punctuation (. ? !). Vary the wording. No "
            "numbering, no commentary, no quotes.");
  // Bloc <think></think> PRÉ-REMPLI dans le tour assistant = méthode officielle
  // Qwen3 pour DÉSACTIVER le raisonnement → sortie directe (sinon le modèle
  // "pense" ~200 tokens avant de répondre = 10-15 s de génération).
  std::string prompt = "<|im_start|>system\n" + sys + "<|im_end|>\n" +
                       "<|im_start|>user\n" + sentence + "<|im_end|>\n" +
                       "<|im_start|>assistant\n<think>\n\n</think>\n\n";

  // génération : KV propre (et on invalide le cache incrémental de nextWords)
  llama_memory_clear(mem, true);
  prev_.clear();

  std::vector<llama_token> toks = tokenize_(vo, prompt, /*bos=*/true, /*special=*/true);
  if (toks.empty()) return {};
  {
    llama_batch b = llama_batch_get_one(toks.data(), (int)toks.size());
    if (llama_decode(c, b) != 0) { llama_memory_clear(mem, true); return {}; }
  }

  // Décodage par ÉCHANTILLONNAGE (≠ greedy) — c'est la clé de variantes
  // DISTINCTES. Réglages à chaud par env (tuning). Seed = hash(phrase) → même
  // phrase ⇒ mêmes variantes (déterministe, testable) ; override IME_REFORM_SEED.
  float temp = envF("IME_REFORM_TEMP", 0.8f);
  float topP = envF("IME_REFORM_TOPP", 0.92f);
  int topK = (int)envF("IME_REFORM_TOPK", 60.0f);
  const char *seedEnv = getenv("IME_REFORM_SEED");
  uint32_t seed = (seedEnv && *seedEnv)
                      ? (uint32_t)std::strtoul(seedEnv, nullptr, 10)
                      : (uint32_t)std::hash<std::string>{}(sentence);
  std::mt19937 rng(seed);

  // token retour-à-la-ligne : sert à FORCER la variante suivante si le modèle
  // tente de finir (EOG) avant d'avoir produit n lignes (cf boucle).
  llama_token nlTok = -1;
  {
    std::vector<llama_token> nlv = tokenize_(vo, "\n", /*bos=*/false);
    if (!nlv.empty()) nlTok = nlv.back();
  }

  std::string out;
  const int maxTok = 256;
  int newlines = 0, gen = 0;
  for (int i = 0; i < maxTok; ++i) {
    float *lg = llama_get_logits_ith(c, -1);
    if (!lg) break;
    llama_token best = sampleTok(lg, n_vocab_, temp, topP, topK, rng);
    if (llama_vocab_is_eog(vo, best)) {
      // EOG prématuré : le modèle s'arrête souvent après UNE ligne. Tant qu'on
      // n'a pas n lignes complètes, on remplace la fin par un saut de ligne pour
      // le pousser à produire la variante suivante (sinon on renvoyait 1 seule).
      if (newlines >= n || nlTok < 0) break;
      best = nlTok;
    }
    std::string p = piece_(vo, best);
    out += p;
    ++gen;
    for (char ch : p) if (ch == '\n') ++newlines;
    if (newlines > want) break; // assez de lignes générées
    llama_batch bb = llama_batch_get_one(&best, 1);
    if (llama_decode(c, bb) != 0) break;
  }
  if (getenv("IME_DEBUG"))
    fprintf(stderr,
            "[reformulate] lang=%s temp=%.2f topP=%.2f gen=%d tok, nl=%d, raw=\"%.300s\"\n",
            fr ? "fr" : "en", temp, topP, gen, newlines, out.c_str());
  // la génération a pollué le KV → reset pour les prochains nextWords
  llama_memory_clear(mem, true);
  prev_.clear();

  // retire un éventuel bloc de raisonnement <think>...</think>
  size_t te = out.rfind("</think>");
  if (te != std::string::npos) out = out.substr(te + 8);

  // casse-insensible (pour dédup + drop == source) : minuscule ASCII + trim
  auto norm = [](const std::string &x) {
    std::string r;
    r.reserve(x.size());
    for (char ch : x)
      if (!std::isspace((unsigned char)ch))
        r += (char)std::tolower((unsigned char)ch);
    return r;
  };
  std::string srcN = norm(sentence);

  // une variante par ligne : trim numérotation/ponctuation de tête, drop les
  // doublons (casse-insensible) et la variante identique à la source.
  std::vector<std::string> variants;
  std::vector<std::string> seen;
  std::stringstream ss(out);
  std::string line;
  while (std::getline(ss, line) && (int)variants.size() < n) {
    size_t a = line.find_first_not_of(" \t\r\"'-*•0123456789.)(");
    if (a == std::string::npos) continue;
    size_t z = line.find_last_not_of(" \t\r\"");
    std::string v = line.substr(a, z - a + 1);
    if (v.empty() || !valid_utf8(v)) continue;
    std::string vN = norm(v);
    if (vN == srcN) continue; // identique à la phrase d'origine → inutile
    bool dup = false;
    for (auto &s : seen) if (s == vN) { dup = true; break; }
    if (dup) continue;
    seen.push_back(vN);
    variants.push_back(v);
  }
  return variants;
}

NeuralPredictor::~NeuralPredictor() {
  if (ctx_) llama_free((llama_context *)ctx_);
  if (model_) llama_model_free((llama_model *)model_);
}
