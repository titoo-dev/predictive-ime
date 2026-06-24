#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Neural next-word predictor backed by libllama (a GGUF model on CPU).
// Opaque (void* impl) so predictord.cpp need not include llama.h.
//
// Design (cf docs/superpowers/specs/2026-06-23-neural-llm-predictor-design.md):
//  - INCREMENTAL KV CACHE: only tokens new since the previous context are
//    decoded. Across a growing sentence each prediction ingests just the new
//    word (prefill is the bottleneck: ~22 t/s for Qwen3-4B).
//  - FAST CANDIDATES: the top-k word-initial next-tokens (whole common words are
//    single tokens) — no per-candidate decode. Validated ~120 ms incremental.
//  - Handles next-WORD only (empty prefix). Within-word completion stays with
//    the n-gram (fast + already good); the daemon decides which to call.
class NeuralPredictor {
public:
  NeuralPredictor() = default;
  ~NeuralPredictor();
  NeuralPredictor(const NeuralPredictor &) = delete;
  NeuralPredictor &operator=(const NeuralPredictor &) = delete;

  // Load a GGUF model. backendDir = directory holding ggml-cpu-*.so
  // (GGML_BACKEND_PATH); empty → rely on env / default search.
  bool init(const std::string &modelPath, int threads, int nCtx,
            const std::string &backendDir);
  bool ready() const { return ctx_ != nullptr; }

  // Up to k candidate next words for the already-typed context words.
  // Returns [] if not ready or context is empty.
  std::vector<std::string> nextWords(const std::vector<std::string> &context, int k);

  // Up to n rephrasings of `sentence` (instruct generation, on-demand — seconds,
  // NOT per-keystroke). Uses the Qwen3 chat template + greedy generation, parses
  // one variant per line. Returns [] if not ready. Resets the nextWords KV cache.
  // mode : rephrase|formal|simple|short|correct|translate (cf reform_prompts.h).
  // nonce : varie le seed pour « régénérer » (nouvelles variantes même phrase).
  std::vector<std::string> reformulate(const std::string &sentence, int n,
                                       const std::string &mode, uint32_t nonce);

private:
  void *model_ = nullptr;       // llama_model*
  void *ctx_ = nullptr;         // llama_context*
  const void *vocab_ = nullptr; // const llama_vocab*
  void *mem_ = nullptr;         // llama_memory_t
  int n_vocab_ = 0;
  std::vector<int> prev_;       // token ids currently materialized in the KV cache
};
