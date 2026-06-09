// Track B — "spike" latence du cerveau n-gram (CPU-only, i5-1335U).
//
// But: prouver qu'une prédiction du mot suivant par n-gram (stupid-backoff,
// structure hashmap) tient TRÈS en dessous du budget ~30 ms/frappe, à une
// échelle de vocabulaire réaliste (clavier prédictif). On n'a pas besoin d'un
// vrai corpus : la latence de requête est une propriété de la structure de
// données, pas du contenu. On génère donc un modèle trigramme synthétique de
// taille réaliste (vocab ~50k, ~2M trigrammes, distribution zipfienne) et on
// chronométre la requête "top-k mots suivants".
//
// Build:  g++ -O2 -std=c++17 bench_ngram.cpp -o bench_ngram
// Run:    ./bench_ngram
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using u32 = uint32_t;
using u64 = uint64_t;

// Contexte = 2 mots précédents, encodés sur 64 bits.
static inline u64 ctx_key(u32 a, u32 b) { return (u64(a) << 32) | b; }

int main() {
  const u32 VOCAB = 50000;       // taille de vocabulaire réaliste (FR+EN)
  const u64 N_TRIGRAMS = 2000000; // ~2M trigrammes observés
  const int TOPK = 6;             // candidats affichés (page fcitx5)
  const int N_QUERIES = 200000;   // requêtes chronométrées

  std::mt19937 rng(12345);
  // Distribution zipfienne approchée: les contextes/mots fréquents reviennent.
  auto zipf = [&](u32 n) {
    // échantillon biaisé vers les petits indices (mots fréquents)
    double u = std::generate_canonical<double, 24>(rng);
    return u32(double(n) * (u * u)); // carré => biais vers 0
  };

  // Modèle: ctx(w1,w2) -> (w3 -> count)
  std::unordered_map<u64, std::unordered_map<u32, u32>> model;
  model.reserve(1 << 21);

  printf("Construction du modèle trigramme synthétique...\n");
  printf("  vocab=%u, trigrammes=%llu\n", VOCAB, (unsigned long long)N_TRIGRAMS);
  auto t0 = Clock::now();
  for (u64 i = 0; i < N_TRIGRAMS; ++i) {
    u32 w1 = zipf(VOCAB), w2 = zipf(VOCAB), w3 = zipf(VOCAB);
    model[ctx_key(w1, w2)][w3]++;
  }
  // Contextes "chauds" réalistes: des contextes fréquents (ex: "je suis", "il
  // y") ont des centaines de continuations. On force HOT contextes (indices
  // bas, donc tirés souvent par zipf) avec ~600 candidats chacun → c'est le
  // pire cas du partial_sort qu'on veut mesurer honnêtement.
  const u32 HOT = 2000, HOT_CANDS = 600;
  for (u32 c = 0; c < HOT; ++c) {
    u32 w1 = c % 50, w2 = (c / 50) % 50; // paires d'indices bas
    auto &m = model[ctx_key(w1, w2)];
    for (u32 j = 0; j < HOT_CANDS; ++j) m[zipf(VOCAB)] += (HOT_CANDS - j);
  }
  auto t1 = Clock::now();
  double build_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  size_t n_ctx = model.size();
  // estimation grossière mémoire (paires w3->count + overhead)
  size_t entries = 0;
  for (auto &kv : model) entries += kv.second.size();
  double approx_mb =
      double(n_ctx * 56 + entries * 16) / (1024.0 * 1024.0);
  printf("  contextes distincts=%zu, entrées w3=%zu\n", n_ctx, entries);
  printf("  construit en %.0f ms, ~%.0f Mo (estimé)\n\n", build_ms, approx_mb);

  // Pré-tirer des contextes de requête (mélange fréquents/rares).
  std::vector<u64> queries;
  queries.reserve(N_QUERIES);
  for (int i = 0; i < N_QUERIES; ++i)
    queries.push_back(ctx_key(zipf(VOCAB), zipf(VOCAB)));

  // Fonction de prédiction: top-k mots suivants par count (stupid-backoff:
  // si le contexte trigramme est vide, on retomberait sur le bigramme — ici on
  // mesure le cas trigramme, le plus coûteux).
  std::vector<std::pair<u32, u32>> scratch;
  auto predict = [&](u64 ctx, std::vector<u32> &out) {
    out.clear();
    auto it = model.find(ctx);
    if (it == model.end()) return; // backoff (non mesuré ici)
    scratch.assign(it->second.begin(), it->second.end());
    int k = std::min<int>(TOPK, (int)scratch.size());
    std::partial_sort(
        scratch.begin(), scratch.begin() + k, scratch.end(),
        [](auto &a, auto &b) { return a.second > b.second; });
    for (int i = 0; i < k; ++i) out.push_back(scratch[i].first);
  };

  // Chronométrage.
  std::vector<double> lat;
  lat.reserve(N_QUERIES);
  std::vector<u32> out;
  u64 hits = 0, produced = 0;
  for (u64 q : queries) {
    auto a = Clock::now();
    predict(q, out);
    auto b = Clock::now();
    lat.push_back(std::chrono::duration<double, std::micro>(b - a).count());
    if (!out.empty()) { hits++; produced += out.size(); }
  }

  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) { return lat[size_t(p * (lat.size() - 1))]; };
  double sum = 0;
  for (double x : lat) sum += x;

  printf("Latence de prédiction top-%d (sur %d requêtes):\n", TOPK, N_QUERIES);
  printf("  moyenne : %.2f µs\n", sum / lat.size());
  printf("  p50     : %.2f µs\n", pct(0.50));
  printf("  p99     : %.2f µs\n", pct(0.99));
  printf("  p99.9   : %.2f µs\n", pct(0.999));
  printf("  max     : %.2f µs\n", lat.back());
  printf("  contextes trouvés: %.1f%% (le reste = backoff bigramme)\n",
         100.0 * hits / N_QUERIES);
  printf("\nBudget clavier ~30000 µs/frappe → marge = %.0fx (p99)\n",
         30000.0 / pct(0.99));
  return 0;
}
