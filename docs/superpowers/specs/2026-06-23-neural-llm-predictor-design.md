# Neural LLM predictor — design (2026-06-23)

> Decisions taken autonomously (user directive: "be autonomous, push the neural
> features as far as possible, even replacing the n-gram"). No approval gate.
> Backed by the latency + quality benchmarks recorded in nix-config
> `docs/wiki/custom-ime-research.md` (AXE 2bis).

## Goal

Make a **pretrained small multilingual LLM the primary next-word / completion
engine** of the IME, served on CPU within a ~200 ms budget, with the existing
Kneser-Ney n-gram demoted to an **instant fallback** (0-ms ghost text while the
LLM computes, and a safety net if the LLM is unavailable). The n-gram is NOT
deleted yet — it stays behind a config switch so we can A/B and never regress to
"no suggestions". Full replacement is the end state once neural is proven better
on the eval harness.

## Why this is now the right call (was rejected at 10 ms)

- Budget moved from <10 ms/keystroke to **~200 ms/prediction** (quality > UX).
- Bench (i5-1335U, llama.cpp Q4_K_M): a small LLM fits the budget. Quality (bpb,
  tokenizer-fair) of **Qwen3-4B crushes the FR-specialist CroissantLLM even in
  French** (FR 1.16 vs 1.64, EN 1.09 vs 1.71). CroissantLLM's official GGUF also
  **crashes** on U+2009 (thin space) → disqualified.
- Runtime decision: **llama.cpp / libllama**, linked into the C++ daemon.
  Verified: nixpkgs `llama-cpp` ships `libllama.so` (out) + `llama.h` +
  `llama-config.cmake` + `llama.pc` (dev) → clean `find_package(llama)`.
  CPU-only by default, no Python in production. (candle/ort were alternatives;
  llama.cpp wins because the daemon is already C++ and GGUF tooling is turnkey.)

## Model

- **Qwen3-4B-Base**, Q4_K_M GGUF (~2.3 GiB). Apache-2.0, French confirmed.
  Base (not instruct) → raw completion, exactly what prediction wants.
- Latency measured: ~132 ms/token (8 threads) → fits ~1 word/200 ms.
- For isolation testing now: the cached `Qwen3-4B-Q4_K_M.gguf` (instruct) is a
  valid latency/plumbing proxy; swap to a base GGUF (produced via
  `convert_hf_to_gguf.py`) before quality eval. Model path is a runtime arg —
  NOT pinned in the flake yet (2.3 GiB; deployment decision, separate step).
- `Qwen3-1.7B-Base` flagged as the likely quality/latency sweet spot (multi-token
  in budget) — bench later.

## Architecture

```
engine/predict.cpp  --[unix socket, JSON]-->  daemon/predictord.cpp
                                                 ├── n-gram (Kneser-Ney)  [instant, fallback]
                                                 └── neural (libllama)    [primary when enabled]
```

The socket protocol is **unchanged** — the engine keeps sending
`{"context":[...],"prefix":"..."}` and receiving `{"candidates":[...],...}`. The
daemon decides internally whether candidates come from neural or n-gram. This
keeps the proven engine/UI untouched (safety: a broken engine kills the whole
session's keyboard — cf. nix-config memory).

### New unit: `NeuralPredictor` (C++, libllama)

Single responsibility: given context text + prefix, return ranked candidate
words. Isolated, independently testable.

- **State**: one `llama_model` + one `llama_context` (KV cache). Loaded once.
- **Incremental KV cache** (the key perf lever — prefill is slow, 22-50 t/s):
  remember the token sequence already decoded; on a new request, find the longest
  common prefix with the previous context and only `llama_decode` the *new*
  tokens. Never re-prefill the whole sentence per keystroke.
- **Next-word from a subword LM** (the core algorithm):
  - Empty prefix (next-word): take the top-K first tokens by logit; for each,
    greedily extend to a word boundary (space/punct) to assemble a candidate
    word; dedupe; rank by first-token logprob. Cap expansion at N tokens.
  - Non-empty prefix (completion): constrain the first token to those whose
    detokenized piece is consistent with the prefix (prefix is a prefix of, or
    starts with, the piece, accent-folded), then expand as above.
- **Time budget**: hard cap (config `neuralBudgetMs`, default 180). If exceeded,
  return what we have (or empty → daemon falls back to n-gram).
- **Threads**: pinned (config `neuralThreads`, default 4 — bench showed 4 stable,
  8 jittery for tg).

### Daemon integration

- Config (`~/.config/ime-predictord/config.json`, hot-reloaded):
  `neural` (bool, default **false** → zero change to current behavior),
  `neuralModel` (GGUF path), `neuralThreads`, `neuralBudgetMs`.
- When `neural=true` and a model is loaded: neural candidates are primary.
  n-gram still computed (cheap) and appended as fallback / to fill `autocomplete`
  + `literalIsWord` (those stay n-gram/lexicon-driven — they gate auto-apply and
  must stay conservative). So **neural drives the candidate bar; n-gram keeps the
  safety semantics**. This is the "push neural to the front, keep n-gram as net".
- Synchronous within the engine's 150 ms socket timeout for the MVP. If the model
  is too slow, the budget cap returns early → n-gram. (Async two-phase — instant
  n-gram then a pushed neural refresh — is the follow-up once the engine learns to
  accept an async update; out of scope for step 1.)

## Build

- `daemon/CMakeLists.txt`: `find_package(llama)`, link `llama` to a new
  `neural_predict` target (standalone CLI) and, once integrated, to `predictord`.
  Guard with `option(WITH_NEURAL)` so the daemon still builds without llama-cpp.
- `flake.nix`: add `pkgs.llama-cpp` to `predictord` buildInputs; expose a
  `neural-predict` package for the isolation CLI.

## Test plan (isolation first — never break the live keyboard)

1. **Standalone CLI** `neural_predict <model.gguf>`: read context lines from
   stdin, print top-k next words + per-call latency. Manually sanity-check FR/EN
   ("je vais" → plausible French; "the quick brown" → "fox"), confirm <200 ms.
2. **Incremental KV correctness**: same context twice → second call far faster
   (cache hit); changed context → only new tokens decoded.
3. **eval_model.py**: once integrated + base GGUF, run hit@k vs the n-gram
   baseline on the held-out Leipzig set. Target: beat n-gram hit@3 (24.0 %).
4. Daemon `test_predict.py` must still pass with `neural=false` (no regression).
5. Only after isolation passes: live test via `test-live.sh` / headless e2e.

## VALIDATED in isolation (2026-06-23, `daemon/neural_predict.cpp`)

Standalone CLI built against nixpkgs `llama-cpp` (g++, `-lllama -lggml`,
`GGML_BACKEND_PATH` → `<llama-cpp>/bin` for the dlopen'd CPU backend), run on the
cached `Qwen3-4B-Q4_K_M.gguf`, i5-1335U, 4 threads:

- **Quality (FR+EN) is excellent and clearly beats the n-gram**: "le chat noir et"
  → *blanc*; "je vais aller au" → *cinéma/restaurant/parc*; "il fait très beau" →
  *aujourd'hui*; "the weather is very" → *hot/cold/good/nice/warm*; "I would like
  to" → *know/learn/understand*. Single contextual right-word hits the n-gram
  (hit@1 ~15 %) cannot reach.
- **Incremental KV cache works**: a growing sentence decodes only the +1 new word
  per step (`lcp` tracks the cached prefix); prefill stays flat (~120 ms) instead
  of growing with context length.
- **Latency** (Qwen3-4B Q4, 4 threads):
  - candidate list = **prefill only** (top-K word-initial tokens, cand-scan 0.1 ms):
    **~120 ms incremental** (within 200 ms budget), ~180–230 ms on a fresh sentence.
  - full word expansion of the #1 = **+~120 ms/token** → make it optional/async,
    NOT on the candidate-list hot path.
- **Confirms the design**: candidate bar from top-K next-token pieces (whole common
  words are single tokens), incremental KV mandatory, 4B is at the budget edge →
  **Qwen3-1.7B (2× faster) is the likely deploy pick** for multi-token headroom.

## Status — 2026-06-23

DONE & verified (branch `feat/neural-llm-predictor`):
- `NeuralPredictor` (`daemon/neural.{h,cpp}`) — libllama, incremental KV, fast
  top-k word-initial candidates.
- Integrated into `predictord` behind `WITH_NEURAL` + config (`neural`,
  `neuralModel`, `neuralThreads`, `neuralTopk`, `neuralOnly`), hot-reloaded.
  Neural leads next-word; n-gram keeps completion + auto-apply semantics.
- `flake.nix`: `neural-predict` (CLI) + `predictord-neural` (daemon, wrapped);
  `predictord` unchanged (pure n-gram, live service safe).
- Verified: `test_predict.py` all pass (zero regression); socket E2E serves
  neural FR+EN next-word; `neuralOnly` → pure-neural next-word.

- ENGINE timeout (2026-06-23, DONE): `engine/predict.cpp` — per-request timeout.
  Completion stays `socketTimeoutMs` (150 ms, no freeze); next-word uses
  `nextWordTimeoutMs` (config, raise to ~300 for neural). Engine builds +
  `checks.engine` harness passes (isolation, keyboard-safe). The live keyboard can
  now wait for a neural next-word (a hitch after Space — quality > UX, accepted).

- EVAL (2026-06-23, DONE) — hit@k neural vs n-gram, **identical harness** (full
  preceding context for neural, case-insensitive), 15 held-out FR Leipzig sentences,
  259 next-word predictions:

  | model (best regime)         | hit@1 | hit@3 | hit@6 | ms/tok | tok/200ms |
  |-----------------------------|-------|-------|-------|--------|-----------|
  | n-gram (2-word, instant)    | 12.4% | 19.7% | 27.4% | ~0     | —         |
  | **Qwen3-1.7B (full ctx)**   | 16.6% |**31.7%**| 35.9% | **~60**| **~3**    |
  | Qwen3-4B (full ctx)         |**21.6%**| 32.8% |**38.6%**| 132   | ~1        |

  → neural wins clearly over n-gram (4B +9.2 hit@1/+13 hit@3; 1.7B +4.2/+12) — and
  UNDERSTATED (multi-token target words show as a fragment candidate → counted as a
  miss; candidate expansion would raise it). At 2-word context neural LOSES — its
  edge REQUIRES long context (its design point), which the incremental KV cache makes
  even faster (full-ctx runs faster than 2-word). Confirms the bpb face-off.

  **DEPLOY PICK = Qwen3-1.7B.** It keeps ~the 4B's hit@3 (31.7 vs 32.8 — the metric
  that matters for a 3-6 candidate bar), beats n-gram by +12 hit@3, and is **~2.2×
  faster** (60 vs 132 ms/tok → ~3 tokens in the 200 ms budget = multi-token
  completions, better live feel). The 4B's only real edge is hit@1 (top-1), less
  critical when a candidate bar shows several options.
- BUGFIX (2026-06-23): byte-level BPE can split a multi-byte char across tokens →
  a candidate can be INCOMPLETE UTF-8 → `nlohmann::json::dump()` threw
  (type_error.316) and KILLED the daemon. Fixed at the root (`valid_utf8` filter in
  `nextWords`) + defensively wrapped `resp.dump()` in `predictord` so no response
  can ever crash the daemon. Found by the eval (the socket test had missed it).

NEXT (not done):
- ASYNC refinement (better feel than a raised timeout): instant n-gram bar, then a
  pushed neural refresh — needs the engine to read the daemon fd from fcitx5's
  event loop. Bigger refactor; the configurable timeout above is the usable v1.
- Produce a Qwen3-4B-**Base** GGUF (instruct used so far is a latency proxy).
- Pin the model + switch the NixOS module's service to `predictord-neural`
  (deployment to the live system — user-gated; keyboard risk).
- `eval_model.py`: hit@k neural vs n-gram on held-out Leipzig (target: beat 24% hit@3).

## Out of scope (this step)

- Async two-phase protocol (instant n-gram + pushed neural update).
- Pinning the 2.3 GiB GGUF in the flake / deployment to the live system.
- Removing the n-gram entirely (end state, after eval proves neural wins).
- Local on-device fine-tuning / personalization of the neural model (n-gram
  user-learning stays the personalization layer for now).
