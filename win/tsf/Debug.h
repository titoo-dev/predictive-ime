// Journal de diagnostic du text service.
//
// Un TIP tourne DANS le process de l'application : ni console, ni débogueur
// attachable facilement, et un échec (socket injoignable, plage refusée) se
// traduit juste par « rien ne s'affiche ». Sans trace, ces pannes-là ne se
// diagnostiquent pas.
//
// Activation : créer le fichier
//   %LOCALAPPDATA%\ime-predictord\debug
// (une variable d'environnement ne servirait à rien — on hérite de celui de
// l'application hôte, pas du terminal de l'utilisateur).
#pragma once

#include "../../core/os_compat.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace win {

// Drapeau ET journal vivent dans ipcDir() — le SEUL dossier où une application
// en bac à sable (Store, navigateur) peut écrire. Les y mettre ailleurs rendrait
// le journal vide précisément dans les applications qu'on cherche à diagnostiquer.
inline bool debugEnabled() {
  static int cached = -1;
  if (cached < 0) {
    std::string flag = oscompat::ipcDir() + "\\debug";
    cached = oscompat::fileMtime(flag) != 0 ? 1 : 0;
  }
  return cached == 1;
}

inline void dbg(const char *fmt, ...) {
  if (!debugEnabled())
    return;
  static std::string path = oscompat::ipcDir() + "\\predict-tsf.log";
  FILE *f = ::fopen(path.c_str(), "a");
  if (!f)
    return;
  SYSTEMTIME t;
  ::GetLocalTime(&t);
  // Le PID identifie l'application hôte : plusieurs process écrivent ici.
  ::fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, ::GetCurrentProcessId());
  va_list ap;
  va_start(ap, fmt);
  ::vfprintf(f, fmt, ap);
  va_end(ap);
  ::fputc('\n', f);
  ::fclose(f);
}

} // namespace win
