#include "neural.h"
#include "llama.h"
#include "ggml-backend.h"
#include <chrono>
#include <cmath>
#include <cstring>

using clk = std::chrono::steady_clock;
static int elapsedMs(clk::time_point t0) {
  return (int)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() -
                                                                    t0)
      .count();
}

static std::vector<llama_token> tokenize_(const llama_vocab *v, const std::string &t, bool bos) {
  int n = -llama_tokenize(v, t.c_str(), (int)t.size(), nullptr, 0, bos, false);
  std::vector<llama_token> toks(n > 0 ? n : 0);
  int got = llama_tokenize(v, t.c_str(), (int)t.size(), toks.data(), (int)toks.size(), bos, false);
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
