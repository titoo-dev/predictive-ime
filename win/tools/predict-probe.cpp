// Sonde le daemon en passant par le VRAI client C++ (core/daemon_client.h).
//
// Pourquoi cet outil existe : probe-daemon.ps1 parle au daemon via .NET, dont
// le connect() est BLOQUANT. Il validait donc le daemon, jamais le client que
// le text service utilise réellement — et c'est précisément là que se cachait
// le bug qui empêchait toute prédiction sous Windows (connect() non bloquant
// sur AF_UNIX rendant WSAEWOULDBLOCK). Ce binaire exerce le même code que la
// DLL, dans un process où l'on peut voir les erreurs.
//
//   predict-probe [prefixe] [mot-de-contexte...]
//   predict-probe            → requête mot-suivant (préfixe vide)
#include "../../core/daemon_client.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (!oscompat::netInit()) {
    std::fprintf(stderr, "init reseau impossible\n");
    return 1;
  }

  std::string prefix = argc > 1 ? argv[1] : std::string{};
  std::vector<std::string> ctx;
  for (int i = 2; i < argc; i++)
    ctx.push_back(argv[i]);

  std::printf("socket   : %s\n", core::daemonSocketPath().c_str());

  sock_t fd = core::connectDaemon();
  if (!oscompat::sockValid(fd)) {
    std::printf("connect  : ECHEC (err=%d)\n", core::lastConnectError());
    return 2;
  }
  oscompat::sockClose(fd);
  std::printf("connect  : OK\n");

  auto r = core::queryDaemon(ctx, prefix);
  std::printf("contexte : ");
  for (const auto &w : ctx)
    std::printf("%s ", w.c_str());
  std::printf("\nprefixe  : '%s'\n", prefix.c_str());
  std::printf("candidats: %d\n", int(r.candidates.size()));
  for (const auto &c : r.candidates)
    std::printf("  - %s\n", c.c_str());
  std::printf("auto='%s' literalIsWord=%d\n",
              r.autocomplete.c_str(), int(r.literalIsWord));
  return r.candidates.empty() ? 3 : 0;
}
