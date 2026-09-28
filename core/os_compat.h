// Couche de compatibilité OS du daemon — Linux (socket Unix + poll) ET
// Windows 10 1803+/11 (AF_UNIX NATIF via <afunix.h> + WSAPoll).
//
// Principe : AUCUN #ifdef dans predictord.cpp. Tout ce qui diverge entre les
// deux OS passe par les helpers ci-dessous, dont la version POSIX se réduit à
// l'appel système d'origine (diff Linux = zéro changement de comportement).
//
// Pourquoi AF_UNIX et pas un port TCP loopback sous Windows : le fichier de
// socket hérite des ACL NTFS de %LOCALAPPDATA% (donc du seul utilisateur),
// alors qu'un port loopback est joignable par TOUT process local — l'IME voit
// passer chaque mot tapé, ça ne s'expose pas.
//
// Divergences réelles, documentées là où elles comptent :
//   - le PIPE de réveil des workers devient une paire de sockets loopback :
//     WSAPoll ne surveille QUE des sockets, jamais un handle de pipe.
//   - Winsock ne renseigne pas errno : EAGAIN/EWOULDBLOCK → sockWouldBlock().
//   - SOCKET est un entier NON SIGNÉ : les sentinelles `fd < 0` deviennent
//     sockValid() / kBadSock.
#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h> // TOUJOURS avant windows.h
#include <afunix.h>   // sockaddr_un — Windows 10 1803+
#include <windows.h>
#include <ws2tcpip.h>

#include <direct.h>
#include <io.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

using ssize_t = std::ptrdiff_t; // absent du CRT MSVC
using sock_t = SOCKET;
using nfds_t = ULONG;
static constexpr sock_t kBadSock = INVALID_SOCKET;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // pas de SIGPIPE sous Windows
#endif

#else // ---------------------------------------------------------- POSIX ----

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using sock_t = int;
static constexpr sock_t kBadSock = -1;

#endif

namespace oscompat {

inline bool sockValid(sock_t s) { return s != kBadSock; }

// Init de la pile réseau (WSAStartup) + neutralisation de SIGPIPE : un client
// qui ferme sans lire (les `learn` fire-and-forget de l'engine) ne doit pas
// TUER le daemon.
inline bool netInit() {
#ifdef _WIN32
  WSADATA wsa{};
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  ::signal(SIGPIPE, SIG_IGN);
  return true;
#endif
}

inline void sockNonBlock(sock_t s) {
#ifdef _WIN32
  u_long on = 1;
  ::ioctlsocket(s, FIONBIO, &on);
#else
  int fl = ::fcntl(s, F_GETFL);
  if (fl >= 0)
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

inline void sockBlocking(sock_t s) {
#ifdef _WIN32
  u_long off = 0;
  ::ioctlsocket(s, FIONBIO, &off);
#else
  int fl = ::fcntl(s, F_GETFL);
  if (fl >= 0)
    ::fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

// Borne les E/S d'une socket cliente. Sous Windows SO_RCVTIMEO/SO_SNDTIMEO
// prennent un DWORD de millisecondes, pas un struct timeval — passer un
// timeval y serait relu comme un nombre de ms absurde.
inline void sockTimeouts(sock_t s, int timeoutMs) {
#ifdef _WIN32
  DWORD ms = DWORD(timeoutMs < 0 ? 0 : timeoutMs);
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

inline ssize_t sockRead(sock_t s, void *buf, size_t n) {
#ifdef _WIN32
  return ::recv(s, (char *)buf, (int)n, 0);
#else
  return ::read(s, buf, n);
#endif
}

inline ssize_t sockWrite(sock_t s, const void *buf, size_t n) {
#ifdef _WIN32
  return ::send(s, (const char *)buf, (int)n, 0);
#else
  return ::send(s, buf, n, MSG_NOSIGNAL);
#endif
}

inline void sockClose(sock_t s) {
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

// « Rien à lire / plus de place en écriture » — PAS une erreur fatale. Winsock
// ne touche jamais errno : l'état vit dans WSAGetLastError().
inline bool sockWouldBlock() {
#ifdef _WIN32
  int e = ::WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

inline int sockPoll(pollfd *fds, nfds_t n, int timeoutMs); // défini plus bas

// Un connect() non bloquant qui n'est pas encore terminé.
inline bool sockConnectPending() {
#ifdef _WIN32
  int e = ::WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
  return errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

// Termine un connect() non bloquant : attend la writabilité (bornée), puis
// VÉRIFIE SO_ERROR — sans quoi une connexion refusée passerait pour établie.
//
// Indispensable sous Windows : là-bas un connect() AF_UNIX non bloquant rend
// TOUJOURS WSAEWOULDBLOCK et s'achève de façon asynchrone, alors que sous Linux
// il réussit du premier coup (pas de poignée de main sur un socket local).
inline bool sockFinishConnect(sock_t s, int timeoutMs) {
  pollfd p{};
  p.fd = s;
  p.events = POLLOUT;
  int r = sockPoll(&p, 1, timeoutMs);
  if (r <= 0 || !(p.revents & POLLOUT))
    return false;
  int soerr = 0;
#ifdef _WIN32
  int len = int(sizeof(soerr));
#else
  socklen_t len = socklen_t(sizeof(soerr));
#endif
  if (::getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0)
    return false;
  return soerr == 0;
}

// Code d'erreur de la dernière opération socket (diagnostic).
inline int lastSockError() {
#ifdef _WIN32
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

// Interruption par signal : on repart au tour suivant. Winsock n'a pas EINTR —
// une erreur y est toujours persistante (cf la pause du poll loop).
inline bool sockInterrupted() {
#ifdef _WIN32
  return false;
#else
  return errno == EINTR;
#endif
}

inline int sockPoll(pollfd *fds, nfds_t n, int timeoutMs) {
#ifdef _WIN32
  return ::WSAPoll(fds, n, timeoutMs);
#else
  return ::poll(fds, n, timeoutMs);
#endif
}

// Paire de réveil worker → poll loop.
//   POSIX  : le pipe historique.
//   Windows: une paire de sockets loopback, car WSAPoll ne sait PAS surveiller
//            un handle de pipe. Le listener est éphémère (fermé dès accept) et
//            on vérifie que le pair accepté est bien celui qu'on a connecté —
//            sinon un autre process local pourrait gagner la course.
inline bool wakePairCreate(sock_t out[2]) {
  out[0] = out[1] = kBadSock;
#ifdef _WIN32
  sock_t lis = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!sockValid(lis))
    return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0; // port éphémère
  int len = (int)sizeof(a);
  if (::bind(lis, (sockaddr *)&a, len) != 0 || ::listen(lis, 1) != 0 ||
      ::getsockname(lis, (sockaddr *)&a, &len) != 0) {
    sockClose(lis);
    return false;
  }
  sock_t cli = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!sockValid(cli)) {
    sockClose(lis);
    return false;
  }
  if (::connect(cli, (sockaddr *)&a, len) != 0) {
    sockClose(lis);
    sockClose(cli);
    return false;
  }
  sockaddr_in mine{};
  int mlen = (int)sizeof(mine);
  ::getsockname(cli, (sockaddr *)&mine, &mlen);
  sock_t srv = kBadSock;
  for (;;) {
    sockaddr_in peer{};
    int plen = (int)sizeof(peer);
    srv = ::accept(lis, (sockaddr *)&peer, &plen);
    if (!sockValid(srv))
      break;
    if (peer.sin_port == mine.sin_port &&
        peer.sin_addr.s_addr == mine.sin_addr.s_addr)
      break; // c'est bien NOTRE connexion
    sockClose(srv); // squatteur : on le jette et on réessaie
    srv = kBadSock;
  }
  sockClose(lis);
  if (!sockValid(srv)) {
    sockClose(cli);
    return false;
  }
  BOOL one = TRUE; // le réveil doit partir tout de suite, pas dans 40 ms
  ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
  out[0] = srv; // lecture (surveillé par WSAPoll)
  out[1] = cli; // écriture (les workers)
#else
  int p[2];
  if (::pipe(p) != 0)
    return false;
  out[0] = p[0];
  out[1] = p[1];
#endif
  sockNonBlock(out[0]);
  sockNonBlock(out[1]);
  return true;
}

// Un octet de réveil. POSIX : write() sur le pipe. Windows : send() socket.
inline void wakePairPost(sock_t w) {
#ifdef _WIN32
  ssize_t n = ::send(w, "x", 1, 0);
#else
  ssize_t n = ::write(w, "x", 1);
#endif
  (void)n;
}

inline ssize_t wakePairDrain(sock_t r, void *buf, size_t n) {
#ifdef _WIN32
  return ::recv(r, (char *)buf, (int)n, 0);
#else
  return ::read(r, buf, n);
#endif
}

// ------------------------------------------------------------------ console --

#ifdef _WIN32
// Ctrl+C, fermeture de la console, fin de session : TOUS ignorés. Un daemon ne
// meurt pas parce que le terminal qui l'a lancé se ferme.
inline BOOL WINAPI ignoreConsoleSignals(DWORD) { return TRUE; }
#endif

// Détache le daemon de la console qui l'a lancé.
//
// SANS ça, predictord.exe (application console) meurt en STATUS_CONTROL_C_EXIT
// (0xC000013A) dès que cette console disparaît — le daemon lancé par la tâche
// planifiée s'éteignait tout seul en cours de session.
//
// Le binaire Windows est donc lié en sous-système WINDOWS (cf CMakeLists) : pas
// de console propre du tout. Lancé DEPUIS un terminal on se rattache au sien
// pour garder les logs sous les yeux en dev ; sinon stderr part dans un journal.
inline void detachFromConsole(const std::string &logPath) {
#ifdef _WIN32
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE *f = nullptr;
    ::freopen_s(&f, "CONOUT$", "w", stderr);
    ::freopen_s(&f, "CONOUT$", "w", stdout);
  } else if (!logPath.empty()) {
    // freopen et NON freopen_s : la variante sécurisée ouvre en _SH_DENYRW, si
    // bien que personne ne peut lire le journal tant que le daemon tourne —
    // exactement ce qu'on veut faire (le suivre en direct). freopen partage.
    (void)::freopen(logPath.c_str(), "a", stderr);
  }
  ::SetConsoleCtrlHandler(ignoreConsoleSignals, TRUE);
#else
  (void)logPath; // Linux : systemd s'occupe du cycle de vie
#endif
}

// ----------------------------------------------------------------- fichiers --

inline long long fileMtime(const std::string &p) {
#ifdef _WIN32
  struct _stat64 st {};
  return ::_stat64(p.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
#else
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
#endif
}

inline void mkdirOne(const std::string &p) {
#ifdef _WIN32
  ::_mkdir(p.c_str());
#else
  ::mkdir(p.c_str(), 0755);
#endif
}

// Crée les parents manquants — un `~/.local/share` absent (ou un %APPDATA%
// redirigé) ne doit pas faire perdre silencieusement les mots appris.
inline void mkdirTree(const std::string &p) {
  for (size_t i = 1; i < p.size(); ++i)
    if (p[i] == '/' || p[i] == '\\')
      mkdirOne(p.substr(0, i));
  mkdirOne(p);
}

inline void unlinkFile(const std::string &p) {
#ifdef _WIN32
  ::_unlink(p.c_str());
#else
  ::unlink(p.c_str());
#endif
}

// Résout les liens symboliques : config.json est souvent un lien « stow » vers
// un dépôt de dotfiles — écrire sur le lien le remplacerait par un fichier
// ordinaire et casserait le stow. On écrit donc sur la CIBLE réelle.
inline std::string realPath(const std::string &p) {
#ifdef _WIN32
  HANDLE h = ::CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return p;
  char buf[MAX_PATH];
  DWORD n = ::GetFinalPathNameByHandleA(h, buf, MAX_PATH, FILE_NAME_NORMALIZED);
  ::CloseHandle(h);
  if (n == 0 || n >= MAX_PATH)
    return p;
  std::string out(buf, n);
  // GetFinalPathNameByHandle préfixe « \\?\ » — illisible dans les messages et
  // refusé par certaines API ; on le retire quand le chemin est simple.
  if (out.rfind("\\\\?\\", 0) == 0)
    out.erase(0, 4);
  return out;
#else
  char *rp = ::realpath(p.c_str(), nullptr);
  if (!rp)
    return p;
  std::string out = rp;
  ::free(rp);
  return out;
#endif
}

// Remplacement atomique : écrire un tmp puis renommer par-dessus. Sous Windows
// rename() ÉCHOUE si la cible existe — MoveFileEx/REPLACE_EXISTING est le seul
// équivalent atomique.
inline bool replaceFileAtomic(const std::string &path,
                              const std::string &content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out)
      return false;
    out << content;
    out.close();
    if (!out)
      return (unlinkFile(tmp), false);
  }
#ifdef _WIN32
  if (!::MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    unlinkFile(tmp);
    return false;
  }
#else
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    unlinkFile(tmp);
    return false;
  }
#endif
  return true;
}

// Le daemon recharge sur mtime à la SECONDE près : une réécriture dans la même
// seconde passerait inaperçue et la bascule de langue serait perdue. On pousse
// donc le mtime d'une seconde quand il n'a pas bougé.
inline void bumpMtimeIfUnchanged(const std::string &path, long long before) {
  if (fileMtime(path) > before)
    return;
#ifdef _WIN32
  HANDLE h = ::CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return;
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  u.QuadPart += 10000000ULL; // +1 s en unités de 100 ns
  ft.dwLowDateTime = u.LowPart;
  ft.dwHighDateTime = u.HighPart;
  ::SetFileTime(h, nullptr, nullptr, &ft);
  ::CloseHandle(h);
#else
  struct timespec ts[2] = {{0, UTIME_OMIT}, {time_t(before + 1), 0}};
  ::utimensat(AT_FDCWD, path.c_str(), ts, 0);
#endif
}

// ------------------------------------------------------------- emplacements --
//
// Linux  : XDG ($XDG_DATA_HOME, $XDG_CONFIG_HOME) — inchangé.
// Windows: %LOCALAPPDATA% (données apprises, socket — local à la machine, hors
//          profil itinérant) et %APPDATA% (config éditable, suit le profil).

namespace detail {
inline std::string env(const char *k) {
  const char *v = ::getenv(k);
  return v ? std::string(v) : std::string();
}
} // namespace detail

// Journaux d'apprentissage : user.log, user.tri.log, veto.log.
inline std::string dataDir() {
#ifdef _WIN32
  std::string base = detail::env("LOCALAPPDATA");
  if (base.empty())
    base = detail::env("TEMP");
  return base + "\\ime-predictord";
#else
  std::string xdg = detail::env("XDG_DATA_HOME");
  if (xdg.empty()) {
    std::string home = detail::env("HOME");
    xdg = (home.empty() ? std::string("/tmp") : home) + "/.local/share";
  }
  return xdg + "/ime-predictord";
#endif
}

// Réglages : config.json, dict.txt, snippets.tsv (rechargés à chaud).
inline std::string configDir() {
#ifdef _WIN32
  std::string base = detail::env("APPDATA");
  if (base.empty())
    base = detail::env("LOCALAPPDATA");
  return base + "\\ime-predictord";
#else
  std::string xdg = detail::env("XDG_CONFIG_HOME");
  if (xdg.empty()) {
    std::string home = detail::env("HOME");
    xdg = (home.empty() ? std::string("/tmp") : home) + "/.config";
  }
  return xdg + "/ime-predictord";
#endif
}

// Chemin du socket. sun_path fait 108 octets ET est en char* des DEUX côtés :
// sous Windows un %LOCALAPPDATA% non-ASCII (prénom accentué) ne survivrait pas
// à la conversion — on retombe alors sur le chemin court 8.3.
// Sous Windows le socket vit dans un SOUS-DOSSIER dédié (`ipc`), et pas à côté
// des journaux de mots appris. Raison : le text service est chargé dans des
// applications en bac à sable (Store, navigateurs), auxquelles il faut donner
// un droit d'accès explicite au socket — le donner sur le dossier parent
// exposerait aussi user.log, c'est-à-dire ce que l'utilisateur a tapé.
inline std::string ipcDir() {
#ifdef _WIN32
  return dataDir() + "\\ipc";
#else
  return dataDir();
#endif
}

inline std::string defaultSockPath() {
#ifdef _WIN32
  std::string dir = ipcDir();
  bool ascii = true;
  for (unsigned char c : dir)
    if (c > 0x7F)
      ascii = false;
  if (!ascii) {
    char shortBuf[MAX_PATH];
    DWORD n = ::GetShortPathNameA(dir.c_str(), shortBuf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
      dir.assign(shortBuf, n);
  }
  return dir + "\\predictord.sock";
#else
  return "/tmp/ime-predictord.sock";
#endif
}

// Dernier séparateur de chemin — Windows accepte les deux, et le modèle peut
// être passé en argv avec l'un ou l'autre.
inline std::string dirOf(const std::string &path) {
#ifdef _WIN32
  size_t i = path.find_last_of("/\\");
#else
  size_t i = path.find_last_of('/');
#endif
  return i == std::string::npos ? std::string() : path.substr(0, i + 1);
}

} // namespace oscompat
