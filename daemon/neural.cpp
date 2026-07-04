#include "neural.h"
#include "llama.h"
#include "ggml-backend.h"
#include "reform_prompts.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>

using clk = std::chrono::steady_clock;
static int elapsedMs(clk::time_point t0) {
  return (int)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() -
                                                                    t0)
      .count();
}

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

std::vector<NeuralPredictor::Cand>
NeuralPredictor::nextWords(const std::string &text, int k, int deadlineMs) {
  if (!ctx_ || k < 1) return {};
  std::lock_guard<std::mutex> lk(mu_);
  auto t0 = clk::now();
  // Trailing whitespace off: the model then predicts a " word" token, which is
  // exactly the word-initial shape we filter for below.
  std::string t = text;
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\t'))
    t.pop_back();
  if (t.empty()) return {};

  const llama_vocab *vo = (const llama_vocab *)vocab_;
  llama_context *c = (llama_context *)ctx_;
  llama_memory_t mem = (llama_memory_t)mem_;

  std::vector<llama_token> toks = tokenize_(vo, t, /*bos=*/true);
  if (toks.empty()) return {};

  // incremental KV: keep the longest common prefix, decode only the new tail
  int lcp = 0;
  while (lcp < (int)prev_.size() && lcp < (int)toks.size() && prev_[lcp] == toks[lcp]) ++lcp;
  if (lcp < (int)prev_.size()) llama_memory_seq_rm(mem, 0, lcp, -1);
  int n_new = (int)toks.size() - lcp;
  if (n_new > 0) {
    llama_batch b = llama_batch_get_one(toks.data() + lcp, n_new);
    if (llama_decode(c, b) != 0) {
      prev_.clear();
      logitsValid_ = false;
      llama_memory_clear(mem, true);
      return {};
    }
  } else if (n_new == 0 && !logitsValid_) {
    // cache warm but logits lost (fresh start edge) — nothing decoded, bail
    return {};
  }
  prev_.assign(toks.begin(), toks.end());

  if (n_new > 0) {
    float *logits = llama_get_logits_ith(c, -1);
    if (!logits) return {};
    // Save a COPY: expand() overwrites the context logits in llama_context,
    // and scoreFirstTokens (completion rerank) reads them on a later request.
    ctxLogits_.assign(logits, logits + n_vocab_);
    float mx = ctxLogits_[0];
    for (float l : ctxLogits_) mx = std::max(mx, l);
    double sum = 0.0;
    for (float l : ctxLogits_) sum += std::exp((double)(l - mx));
    ctxLse_ = mx + (float)std::log(sum);
    logitsValid_ = true;
  }
  if (deadlineMs > 0 && elapsedMs(t0) > deadlineMs) return {};

  const int N = k * 8;
  std::vector<int> idx(N, -1);
  std::vector<float> val(N, -1e30f);
  for (int v = 0; v < n_vocab_; ++v) {
    float l = ctxLogits_[v];
    if (l > val[N - 1]) {
      int j = N - 1;
      while (j > 0 && val[j - 1] < l) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; --j; }
      val[j] = l; idx[j] = v;
    }
  }
  std::vector<Cand> out;
  for (int i = 0; i < N && (int)out.size() < k; ++i) {
    if (idx[i] < 0) break;
    std::string p = piece_(vo, idx[i]);
    if (p.empty() || p[0] != ' ') continue;          // word-initial tokens only
    std::string w = p.substr(1);
    if (w.empty() || word_punct_(w[0]) || !valid_utf8(w)) continue; // skip BPE byte-fragments
    bool dup = false; for (auto &o : out) if (o.word == w) { dup = true; break; }
    if (!dup) out.push_back({w, std::exp(val[i] - ctxLse_), idx[i]});
  }
  return out;
}

std::vector<std::string> NeuralPredictor::reformulate(
    const std::string &sentence, int n, const std::string &mode, uint32_t nonce,
    const std::function<void(const std::vector<std::string> &)> &onVariant) {
  if (!ctx_ || sentence.empty()) return {};
  // Le worker async (nextWords/expand) partage ce ctx_ et sa KV : sans ce
  // verrou, générer une reformulation pendant qu'un mot-suivant décode = course
  // sur ctx_/prev_/logitsValid_. Contrat de la classe : toute méthode publique
  // prend mu_ (cf neural.h). reformulate n'appelle aucune autre → pas d'interblocage.
  std::lock_guard<std::mutex> lk(mu_);
  if (n < 1) n = 3;
  const llama_vocab *vo = (const llama_vocab *)vocab_;
  llama_context *c = (llama_context *)ctx_;
  llama_memory_t mem = (llama_memory_t)mem_;

  // On SUR-GÉNÈRE (n+2 lignes) puis on dédoublonne : l'échantillonnage produit
  // parfois deux lignes proches, mieux vaut avoir une marge.
  const int want = n + 2;

  // ÉPINGLAGE DE LANGUE + MODE : prompt partagé avec le backend Groq
  // (reform_prompts.h) → sémantique identique. translate inverse la langue.
  bool fr = reformIsFrench(sentence);
  std::string sys = reformSystemPrompt(mode, fr, want);
  // Bloc <think></think> PRÉ-REMPLI dans le tour assistant = méthode officielle
  // Qwen3 pour DÉSACTIVER le raisonnement → sortie directe (sinon le modèle
  // "pense" ~200 tokens avant de répondre = 10-15 s de génération).
  std::string prompt = "<|im_start|>system\n" + sys + "<|im_end|>\n" +
                       "<|im_start|>user\n" + sentence + "<|im_end|>\n" +
                       "<|im_start|>assistant\n<think>\n\n</think>\n\n";

  // génération : KV propre (et on invalide le cache incrémental de nextWords)
  llama_memory_clear(mem, true);
  prev_.clear();
  logitsValid_ = false;

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
  // nonce : varie à chaque « régénérer » (Ctrl+Alt+R re-pressé) → nouvelles
  // variantes même phrase/mode (sinon seed=hash(phrase) = déterministe).
  uint32_t seed = (seedEnv && *seedEnv)
                      ? (uint32_t)std::strtoul(seedEnv, nullptr, 10)
                      : (uint32_t)std::hash<std::string>{}(sentence) ^ nonce;
  std::mt19937 rng(seed);

  // token retour-à-la-ligne : sert à FORCER la variante suivante si le modèle
  // tente de finir (EOG) avant d'avoir produit n lignes (cf boucle).
  llama_token nlTok = -1;
  {
    std::vector<llama_token> nlv = tokenize_(vo, "\n", /*bos=*/false);
    if (!nlv.empty()) nlTok = nlv.back();
  }

  // casse-insensible (pour dédup + drop == source) : minuscule ASCII + trim
  auto norm = [](const std::string &x) {
    std::string r;
    r.reserve(x.size());
    for (char ch : x)
      if (!std::isspace((unsigned char)ch))
        r += (char)std::tolower((unsigned char)ch);
    return r;
  };
  const std::string srcN = norm(sentence);

  // Parsing INCRÉMENTAL, ligne par ligne (STREAMING) : chaque ligne complète
  // est nettoyée (numérotation/guillemets de tête), dédupliquée et — si elle
  // est acceptée — annoncée via onVariant avec la liste courante. La 1re
  // variante part vers l'UI pendant que le modèle génère encore les suivantes.
  std::vector<std::string> variants, seen;
  auto acceptLine = [&](const std::string &line) {
    if ((int)variants.size() >= n) return;
    // un éventuel marqueur de raisonnement résiduel n'est pas une variante
    if (line.find("<think>") != std::string::npos ||
        line.find("</think>") != std::string::npos)
      return;
    size_t a = line.find_first_not_of(" \t\r\"'-*•0123456789.)(");
    if (a == std::string::npos) return;
    size_t z = line.find_last_not_of(" \t\r\"");
    std::string v = line.substr(a, z - a + 1);
    if (v.empty() || !valid_utf8(v)) return;
    std::string vN = norm(v);
    if (vN == srcN) return; // identique à la phrase d'origine → inutile
    for (auto &s : seen)
      if (s == vN) return;
    seen.push_back(vN);
    variants.push_back(v);
    if (onVariant)
      onVariant(variants);
  };

  std::string lineBuf;
  const int maxTok = 256;
  int newlines = 0, gen = 0;
  for (int i = 0; i < maxTok && (int)variants.size() < n; ++i) {
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
    ++gen;
    for (char ch : p) {
      if (ch == '\n') {
        ++newlines;
        acceptLine(lineBuf);
        lineBuf.clear();
      } else {
        lineBuf.push_back(ch);
      }
    }
    if (newlines > want) break; // assez de lignes générées
    llama_batch bb = llama_batch_get_one(&best, 1);
    if (llama_decode(c, bb) != 0) break;
  }
  acceptLine(lineBuf); // dernière ligne sans \n final
  if (getenv("IME_DEBUG"))
    fprintf(stderr,
            "[reformulate] lang=%s temp=%.2f topP=%.2f gen=%d tok, nl=%d, %zu variantes\n",
            fr ? "fr" : "en", temp, topP, gen, newlines, variants.size());
  // la génération a pollué le KV → reset pour les prochains nextWords
  llama_memory_clear(mem, true);
  prev_.clear();
  logitsValid_ = false;
  return variants;
}

std::string NeuralPredictor::expand(const Cand &c, int maxExtra, int remainMs) {
  if (!ctx_ || c.token < 0) return {};
  std::lock_guard<std::mutex> lk(mu_);
  if (prev_.empty()) return {};
  auto t0 = clk::now();
  const llama_vocab *vo = (const llama_vocab *)vocab_;
  llama_context *cc = (llama_context *)ctx_;
  llama_memory_t mem = (llama_memory_t)mem_;

  std::string word = c.word;
  llama_token cur = (llama_token)c.token;
  bool boundary = false;
  int extra = 0;
  while (true) {
    llama_batch b = llama_batch_get_one(&cur, 1);
    if (llama_decode(cc, b) != 0) break;
    float *lg = llama_get_logits_ith(cc, -1);
    if (!lg) break;
    int best = 0;
    for (int v = 1; v < n_vocab_; ++v)
      if (lg[v] > lg[best]) best = v;
    std::string p = piece_(vo, best);
    // A following token that opens with space/punct/newline = the word ended.
    // Apostrophe is NOT a boundary: it's the French elision continuation
    // ("l" + "'école") — the very case expand() exists for.
    if (p.empty() || p[0] == ' ' || (word_punct_(p[0]) && p[0] != '\'')) {
      boundary = true;
      break;
    }
    if (extra >= maxExtra || (remainMs > 0 && elapsedMs(t0) > remainMs))
      break; // cap hit mid-word → unreliable
    word += p;
    cur = best;
    ++extra;
  }
  // drop the speculative tokens — the KV cache holds the context again
  llama_memory_seq_rm(mem, 0, (int)prev_.size(), -1);
  if (!boundary || !valid_utf8(word)) return {};
  return word;
}

bool NeuralPredictor::scoreFirstTokens(const std::string &text,
                                       const std::vector<std::string> &words,
                                       std::vector<float> &logprobs) {
  if (!ctx_) return false;
  // try_lock : le worker async est peut-être en plein décode — la complétion
  // ne l'attend JAMAIS (rerank opportuniste, le clavier passe devant).
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (!lk.owns_lock() || !logitsValid_) return false;
  const llama_vocab *vo = (const llama_vocab *)vocab_;
  std::string t = text;
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\t'))
    t.pop_back();
  if (t.empty()) return false;
  std::vector<llama_token> toks = tokenize_(vo, t, /*bos=*/true);
  if (toks.size() != prev_.size() ||
      !std::equal(toks.begin(), toks.end(), prev_.begin()))
    return false; // cold cache → opportunistic rerank declines, never decodes
  logprobs.clear();
  logprobs.reserve(words.size());
  for (const auto &w : words) {
    std::vector<llama_token> wt = tokenize_(vo, " " + w, /*bos=*/false);
    if (wt.empty() || wt[0] < 0 || wt[0] >= n_vocab_) {
      logprobs.push_back(-1e30f);
      continue;
    }
    logprobs.push_back(ctxLogits_[wt[0]] - ctxLse_);
  }
  return true;
}

NeuralPredictor::~NeuralPredictor() {
  if (ctx_) llama_free((llama_context *)ctx_);
  if (model_) llama_model_free((llama_model *)model_);
}
