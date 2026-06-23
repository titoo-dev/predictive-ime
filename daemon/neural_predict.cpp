// neural_predict — isolation CLI for the neural next-word predictor (libllama).
// Loads a GGUF, reads context lines from stdin, prints top-K candidate next
// words + per-call latency. Validates the llama.cpp integration + the two core
// design levers on CPU WITHOUT touching the daemon/engine:
//   1. INCREMENTAL KV CACHE — only the tokens new since the previous context are
//      decoded (prefill is the bottleneck: ~22 t/s for Qwen3-4B). Across a
//      growing sentence, each prediction ingests just the new word.
//   2. TOP-K WORD EXPANSION — each of the top-K next-token candidates is greedily
//      expanded to a word boundary (KV rolled back between candidates).
//
// Build: g++ -std=c++17 -O2 -I<llama-cpp.dev>/include neural_predict.cpp \
//   -o neural_predict -L<llama-cpp>/lib -lllama -lggml -Wl,-rpath,<llama-cpp>/lib
// Run:   GGML_BACKEND_PATH=<llama-cpp>/bin ./neural_predict model.gguf [threads] [topk]
#include "llama.h"
#include "ggml-backend.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

static std::vector<llama_token> tokenize(const llama_vocab *vocab,
                                         const std::string &text, bool add_bos) {
    int n = -llama_tokenize(vocab, text.c_str(), (int)text.size(), nullptr, 0,
                            add_bos, /*parse_special=*/false);
    std::vector<llama_token> toks(n > 0 ? n : 0);
    int got = llama_tokenize(vocab, text.c_str(), (int)text.size(), toks.data(),
                             (int)toks.size(), add_bos, false);
    if (got < 0) toks.clear(); else toks.resize(got);
    return toks;
}

static std::string piece(const llama_vocab *vocab, llama_token t) {
    char buf[256];
    int n = llama_token_to_piece(vocab, t, buf, sizeof(buf), /*lstrip=*/0, /*special=*/false);
    return n < 0 ? std::string() : std::string(buf, n);
}

static bool is_word_punct(char c) {
    return c == '\n' || strchr(".,;:!?\"'()[]{}<>/\\|`~@#$%^&*+=", c) != nullptr;
}

// decode a single token at the current KV head; return logits of the new position
static float *decode1(llama_context *ctx, llama_token t) {
    llama_batch b = llama_batch_get_one(&t, 1);
    if (llama_decode(ctx, b) != 0) return nullptr;
    return llama_get_logits_ith(ctx, -1);
}

static int argmax(const float *l, int n) {
    int best = 0; float bv = l[0];
    for (int v = 1; v < n; ++v) if (l[v] > bv) { bv = l[v]; best = v; }
    return best;
}

// expand a chosen first token into a full word (greedy, forward); caller rolls back KV
static std::string expand_word(llama_context *ctx, const llama_vocab *vocab,
                               int n_vocab, llama_token first, int max_tok) {
    std::string w;
    llama_token cur = first;
    for (int step = 0; step < max_tok; ++step) {
        if (llama_vocab_is_eog(vocab, cur)) break;
        std::string p = piece(vocab, cur);
        bool starts_word = (!p.empty() && p[0] == ' ');
        if (step > 0 && starts_word) break;            // next word begins
        std::string add = starts_word ? p.substr(1) : p;
        if (!add.empty() && is_word_punct(add[0])) { if (step == 0) w += add; break; }
        w += add;
        float *lg = decode1(ctx, cur);
        if (!lg) break;
        cur = argmax(lg, n_vocab);
    }
    return w;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s model.gguf [threads] [topk]\n", argv[0]); return 2; }
    const char *model_path = argv[1];
    int n_threads = argc > 2 ? atoi(argv[2]) : 4;
    int topk = argc > 3 ? atoi(argv[3]) : 6;
    if (topk < 1) topk = 1;

    if (const char *bdir = getenv("GGML_BACKEND_PATH")) ggml_backend_load_all_from_path(bdir);
    else ggml_backend_load_all();
    llama_backend_init();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "FATAL: cannot load model %s\n", model_path); return 1; }
    const llama_vocab *vocab = llama_model_get_vocab(model);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 2048;
    cp.n_threads = n_threads;
    cp.n_threads_batch = n_threads;
    llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "FATAL: cannot create context\n"); return 1; }
    llama_memory_t mem = llama_get_memory(ctx);
    const int n_vocab = llama_vocab_n_tokens(vocab);
    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };

    fprintf(stderr, "[ready] threads=%d topk=%d n_vocab=%d\n", n_threads, topk, n_vocab);

    std::vector<llama_token> prev; // tokens currently materialized in the KV cache
    char inbuf[8192];
    while (fgets(inbuf, sizeof(inbuf), stdin)) {
        std::string text(inbuf);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.empty()) continue;

        std::vector<llama_token> toks = tokenize(vocab, text, /*add_bos=*/true);
        if (toks.empty()) { printf("(empty)\n"); fflush(stdout); continue; }

        // --- incremental KV: keep the longest common prefix, decode only the rest ---
        int lcp = 0;
        while (lcp < (int)prev.size() && lcp < (int)toks.size() && prev[lcp] == toks[lcp]) ++lcp;
        if (lcp < (int)prev.size()) llama_memory_seq_rm(mem, 0, lcp, -1); // drop diverged tail

        auto t0 = std::chrono::steady_clock::now();
        int n_new = (int)toks.size() - lcp;
        if (n_new > 0) {
            llama_batch b = llama_batch_get_one(toks.data() + lcp, n_new);
            if (llama_decode(ctx, b) != 0) { printf("(decode failed)\n"); fflush(stdout); prev.clear(); llama_memory_clear(mem, true); continue; }
        }
        auto t1 = std::chrono::steady_clock::now();
        prev = toks;
        const int base = (int)toks.size(); // KV head after the context

        // top-N next-token candidates from the context's last-position logits
        float *logits = llama_get_logits_ith(ctx, -1);
        const int N = topk * 8;
        std::vector<int> idx(N, -1);
        std::vector<float> val(N, -1e30f);
        for (int v = 0; v < n_vocab; ++v) {
            float l = logits[v];
            if (l > val[N - 1]) {
                int j = N - 1;
                while (j > 0 && val[j - 1] < l) { val[j] = val[j - 1]; idx[j] = idx[j - 1]; --j; }
                val[j] = l; idx[j] = v;
            }
        }
        // FAST PATH: word-initial tokens (piece starts with space) are already
        // whole words for common vocab — strip + dedupe, ZERO extra decode.
        std::vector<std::string> cands;
        llama_token top_tok = -1;
        for (int k = 0; k < N && (int)cands.size() < topk; ++k) {
            if (idx[k] < 0) break;
            std::string p = piece(vocab, idx[k]);
            if (p.empty() || p[0] != ' ') continue; // skip sub-word continuations
            std::string w = p.substr(1);
            if (w.empty() || is_word_punct(w[0])) continue;
            bool dup = false; for (auto &c : cands) if (c == w) { dup = true; break; }
            if (!dup) { cands.push_back(w); if (top_tok < 0) top_tok = idx[k]; }
        }
        auto t2 = std::chrono::steady_clock::now(); // candidates ready (fast path)

        // expand only the #1 candidate to a full word (multi-token words), 1 decode chain
        std::string top1 = cands.empty() ? "" : cands[0];
        if (top_tok >= 0) { top1 = expand_word(ctx, vocab, n_vocab, top_tok, 8); llama_memory_seq_rm(mem, 0, base, -1); }
        auto t3 = std::chrono::steady_clock::now();

        printf("ctx=\"%s\"  [+%d tok, lcp=%d]\n", text.c_str(), n_new, lcp);
        printf("  candidates:");
        for (auto &w : cands) printf(" \"%s\"", w.c_str());
        printf("\n  top1(expanded)=\"%s\"\n", top1.c_str());
        printf("  prefill=%.1f ms  cand-scan=%.1f ms  | FAST total=%.1f ms  (+expand1=%.1f ms → %.1f ms)\n",
               ms(t1 - t0), ms(t2 - t1), ms(t2 - t0), ms(t3 - t2), ms(t3 - t0));
        fflush(stdout);
    }

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
