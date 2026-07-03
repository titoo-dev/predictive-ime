#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Neural next-word predictor backed by libllama (a GGUF model on CPU).
// Opaque (void* impl) so predictord.cpp need not include llama.h.
//
// Design (cf docs/superpowers/specs/2026-06-23-neural-llm-predictor-design.md):
//  - INCREMENTAL KV CACHE: only tokens new since the previous context are
//    decoded. Across a growing sentence each prediction ingests just the new
//    word (prefill is the bottleneck: ~22 t/s for Qwen3-4B).
//  - RAW TEXT context (not word lists): punctuation + casing reach the model,
//    and previous sentences count — its measured edge REQUIRES long context.
//  - FAST CANDIDATES: the top-k word-initial next-tokens with their softmax
//    probability (comparable to the n-gram's P(w|ctx) for score fusion).
//  - expand(): bounded greedy decode to finish a multi-token word (French
//    elisions "l'école", accented words) — speculative tokens are removed
//    from the KV cache afterwards.
//  - scoreFirstTokens(): log-probs of candidate words' first token from the
//    SAVED context logits (rerank of n-gram completions). Only answers when
//    the KV cache is already warm for that exact text — never prefills on the
//    completion hot path.
class NeuralPredictor {
public:
  struct Cand {
    std::string word;
    float prob = 0.f; // softmax over the full vocab (first token)
    int token = -1;   // first-token id (for expand())
  };

  NeuralPredictor() = default;
  ~NeuralPredictor();
  NeuralPredictor(const NeuralPredictor &) = delete;
  NeuralPredictor &operator=(const NeuralPredictor &) = delete;

  // Load a GGUF model. backendDir = directory holding ggml-cpu-*.so
  // (GGML_BACKEND_PATH); empty → rely on env / default search.
  bool init(const std::string &modelPath, int threads, int nCtx,
            const std::string &backendDir);
  bool ready() const { return ctx_ != nullptr; }

  // Up to k candidate next words for the raw text before the cursor.
  // deadlineMs bounds the WHOLE call (prefill included): past it, returns
  // whatever is available (possibly empty → caller falls back to n-gram).
  std::vector<Cand> nextWords(const std::string &text, int k,
                              int deadlineMs);

  // Finish a (possibly partial) word candidate: greedily decode from its
  // first token until a word boundary, up to maxExtra tokens and remainMs.
  // Returns the full word ("" if it could not be completed reliably).
  // Restores the KV cache to the context state before returning.
  std::string expand(const Cand &c, int maxExtra, int remainMs);

  // Log-probabilities of each word's FIRST token under the saved context
  // logits. Only valid if the cache is warm for exactly `text` (same tokens
  // as the last nextWords call): returns false otherwise, never decodes.
  // OPPORTUNISTIC also wrt threading: if the worker thread is mid-decode
  // (async two-phase), declines immediately instead of blocking the keyboard.
  bool scoreFirstTokens(const std::string &text,
                        const std::vector<std::string> &words,
                        std::vector<float> &logprobs);

private:
  // One llama_context, two callers (main loop sync/rerank + async worker):
  // every public method takes the lock; scoreFirstTokens only try_locks.
  std::mutex mu_;
  void *model_ = nullptr;       // llama_model*
  void *ctx_ = nullptr;         // llama_context*
  const void *vocab_ = nullptr; // const llama_vocab*
  void *mem_ = nullptr;         // llama_memory_t
  int n_vocab_ = 0;
  std::vector<int> prev_;       // token ids currently materialized in the KV cache
  std::vector<float> ctxLogits_; // copy of the context logits (survives expand)
  float ctxLse_ = 0.f;           // log-sum-exp of ctxLogits_ (softmax denom)
  bool logitsValid_ = false;
};
