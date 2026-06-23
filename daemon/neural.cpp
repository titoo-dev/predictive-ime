#include "neural.h"
#include "llama.h"
#include "ggml-backend.h"
#include <cstring>

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
    if (w.empty() || word_punct_(w[0])) continue;
    bool dup = false; for (auto &o : out) if (o == w) { dup = true; break; }
    if (!dup) out.push_back(w);
  }
  return out;
}

NeuralPredictor::~NeuralPredictor() {
  if (ctx_) llama_free((llama_context *)ctx_);
  if (model_) llama_model_free((llama_model *)model_);
}
