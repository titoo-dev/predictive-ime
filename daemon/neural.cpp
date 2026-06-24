#include "neural.h"
#include "llama.h"
#include "ggml-backend.h"
#include <cstring>
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

  // Prompt chat Qwen3. "/no_think" coupe le mode raisonnement → sortie directe.
  std::string sys = "Reformule la phrase en " + std::to_string(n) +
                    " variantes naturelles, une par ligne, meme langue, "
                    "sans numero ni commentaire.";
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

  // Génération greedy, BORNÉE : on s'arrête à EOG, ou dès qu'on a assez de lignes
  // (n variantes), ou au plafond de tokens. Évite les générations qui s'emballent.
  std::string out;
  const int maxTok = 160;
  int newlines = 0, gen = 0;
  for (int i = 0; i < maxTok; ++i) {
    float *lg = llama_get_logits_ith(c, -1);
    if (!lg) break;
    int best = 0; float bv = lg[0];
    for (int v = 1; v < n_vocab_; ++v) if (lg[v] > bv) { bv = lg[v]; best = v; }
    if (llama_vocab_is_eog(vo, best)) break;
    std::string p = piece_(vo, best);
    out += p;
    ++gen;
    for (char ch : p) if (ch == '\n') ++newlines;
    if (newlines > n) break; // n lignes complètes obtenues → assez
    llama_batch bb = llama_batch_get_one(&best, 1);
    if (llama_decode(c, bb) != 0) break;
  }
  if (getenv("IME_DEBUG"))
    fprintf(stderr, "[reformulate] gen=%d tok, nl=%d, raw=\"%.300s\"\n", gen, newlines, out.c_str());
  // la génération a pollué le KV → reset pour les prochains nextWords
  llama_memory_clear(mem, true);
  prev_.clear();

  // retire un éventuel bloc de raisonnement <think>...</think>
  size_t te = out.rfind("</think>");
  if (te != std::string::npos) out = out.substr(te + 8);

  // une variante par ligne (trim ponctuation/numérotation de tête, UTF-8 valide)
  std::vector<std::string> variants;
  std::stringstream ss(out);
  std::string line;
  while (std::getline(ss, line) && (int)variants.size() < n) {
    size_t a = line.find_first_not_of(" \t\r\"'-*•0123456789.)(");
    if (a == std::string::npos) continue;
    size_t z = line.find_last_not_of(" \t\r\"");
    std::string v = line.substr(a, z - a + 1);
    if (!v.empty() && valid_utf8(v)) variants.push_back(v);
  }
  return variants;
}

NeuralPredictor::~NeuralPredictor() {
  if (ctx_) llama_free((llama_context *)ctx_);
  if (model_) llama_model_free((llama_model *)model_);
}
