// neural_predict — isolation/diagnostic CLI for NeuralPredictor (libllama).
// Reads context lines from stdin, prints top-k next-word candidates + latency.
// Proves the llama.cpp integration on CPU WITHOUT touching the daemon/engine.
//
//   GGML_BACKEND_PATH=<llama-cpp>/bin ./neural_predict model.gguf [threads] [topk]
#include "neural.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s model.gguf [threads] [topk]\n", argv[0]); return 2; }
  int n_threads = argc > 2 ? atoi(argv[2]) : 4;
  int topk = argc > 3 ? atoi(argv[3]) : 6;
  const char *bdir = getenv("GGML_BACKEND_PATH");

  NeuralPredictor np;
  if (!np.init(argv[1], n_threads, /*nCtx=*/2048, bdir ? bdir : "")) {
    fprintf(stderr, "FATAL: cannot load model %s\n", argv[1]);
    return 1;
  }
  fprintf(stderr, "[ready] threads=%d topk=%d\n", n_threads, topk);

  auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
  char inbuf[8192];
  while (fgets(inbuf, sizeof(inbuf), stdin)) {
    std::string line(inbuf);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) continue;
    std::vector<std::string> words;
    std::istringstream iss(line);
    for (std::string w; iss >> w;) words.push_back(w);
    if (words.empty()) continue;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cands = np.nextWords(words, topk);
    auto t1 = std::chrono::steady_clock::now();

    printf("ctx=\"%s\"\n  candidates:", line.c_str());
    for (auto &c : cands) printf(" \"%s\"", c.c_str());
    printf("\n  latency=%.1f ms\n", ms(t1 - t0));
    fflush(stdout);
  }
  return 0;
}
