// Panneau d'administration de Predict (Windows).
//
// Ce que c'est : un exécutable Win32 natif, sans dépendance (contrôles communs
// v6, /MT), ouvert depuis le menu Démarrer, l'icône de la barre des tâches
// (« Réglages… ») ou la fin de l'installeur. Il édite les MÊMES fichiers que le
// daemon et le service texte lisent à chaud :
//   %APPDATA%\ime-predictord\config.json   — réglages (relus sur mtime)
//   %LOCALAPPDATA%\ime-predictord\groq.key — clé API de reformulation
// et pilote la tâche de session « ime-predictord » (démarrer/arrêter).
//
// Règles :
//   - config.json est relu, fusionné (les clés inconnues sont CONSERVÉES) et
//     réécrit atomiquement — jamais de perte de réglage édité à la main ;
//   - la clé API n'apparaît JAMAIS dans config.json ni dans le journal ;
//   - rien n'exige l'élévation : tout est per-user.
#include "../../core/daemon_client.h"
#include "../../core/os_compat.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr wchar_t kMainClassTitle[] = L"Predict — Administration";
constexpr wchar_t kMutexName[] = L"Local\\predictive-ime.predict-admin";
constexpr wchar_t kTaskName[] = L"ime-predictord";
constexpr wchar_t kProjectUrl[] = L"https://github.com/titoo-dev/predictive-ime";
constexpr wchar_t kGroqKeysUrl[] = L"https://console.groq.com/keys";
constexpr wchar_t kClsidKey[] =
    L"CLSID\\{5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F10}\\InprocServer32";
constexpr UINT WM_APP_KEYRESULT = WM_APP + 1;  // wParam: valid, lParam: wchar_t* (owned)
constexpr UINT WM_APP_RELAYOUT = WM_APP + 2;
constexpr UINT WM_APP_GOTOPAGE = WM_APP + 3;  // wParam: index d'onglet
constexpr UINT_PTR kStatusTimer = 1;

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HWND g_tab = nullptr;
HWND g_pages[4] = {};
json g_cfg = json::object();
bool g_dirty = false;
bool g_loading = false;  // vrai pendant fillPages : ignorer les notifications

// ------------------------------------------------------------ conversions --

std::wstring wide(const std::string &s) {
  if (s.empty())
    return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string utf8(const std::wstring &w) {
  if (w.empty())
    return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0,
                                nullptr, nullptr);
  std::string s(size_t(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n,
                        nullptr, nullptr);
  return s;
}

std::string trim(std::string s) {
  auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && sp(s.back()))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && sp(s[i]))
    ++i;
  return s.substr(i);
}

std::wstring fmtDouble(double v) {
  wchar_t buf[64];
  if (std::fabs(v - std::round(v)) < 1e-9)
    swprintf(buf, 64, L"%.0f", v);
  else
    swprintf(buf, 64, L"%g", v);
  return buf;
}

// ------------------------------------------------------- accès aux widgets --

std::wstring getText(HWND dlg, int id) {
  HWND h = ::GetDlgItem(dlg, id);
  int n = ::GetWindowTextLengthW(h);
  std::wstring w(size_t(n) + 1, L'\0');
  ::GetWindowTextW(h, w.data(), n + 1);
  w.resize(size_t(n));
  return w;
}
void setText(HWND dlg, int id, const std::wstring &w) {
  ::SetWindowTextW(::GetDlgItem(dlg, id), w.c_str());
}
bool getCheck(HWND dlg, int id) {
  return ::IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}
void setCheck(HWND dlg, int id, bool on) {
  ::CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED);
}
// Trace opt-in (PREDICT_ADMIN_TRACE=1) dans %LOCALAPPDATA%\ime-predictord\
// predict-admin.log — le seul moyen de voir ce que fait une fenêtre figée.
void trace(const char *fmt, ...) {
  static int enabled = -1;
  if (enabled < 0) {
    char buf[8];
    enabled = ::GetEnvironmentVariableA("PREDICT_ADMIN_TRACE", buf, 8) > 0 ? 1 : 0;
  }
  if (!enabled)
    return;
  std::string path = oscompat::dataDir() + "\\predict-admin.log";
  FILE *f = std::fopen(path.c_str(), "ab");
  if (!f)
    return;
  std::fprintf(f, "%lu ", ::GetTickCount());
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

// Désactiver le contrôle qui A le focus (le bouton qu'on vient de cliquer)
// laisse le gestionnaire de dialogue sans cible : dans un dialogue enfant
// (DS_CONTROL) sous un onglet, sa recherche du prochain arrêt de tabulation
// peut boucler sans fin. On déplace donc le focus AVANT de désactiver.
void setEnabled(HWND dlg, int id, bool on) {
  HWND h = ::GetDlgItem(dlg, id);
  if (!h)
    return;
  if (!on && ::GetFocus() == h) {
    HWND next = ::GetNextDlgTabItem(dlg, h, FALSE);
    if (!next || next == h || !::IsWindowEnabled(next))
      next = g_tab;
    if (next)
      ::SetFocus(next);
  }
  ::EnableWindow(h, on ? TRUE : FALSE);
}

// Nombre optionnel : champ vide → pas de clé (valeur par défaut du daemon).
bool getNumber(HWND dlg, int id, double &out) {
  std::wstring w = getText(dlg, id);
  std::string s = trim(utf8(w));
  if (s.empty())
    return false;
  for (char &c : s)
    if (c == ',')
      c = '.';
  char *end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

void setNumber(HWND dlg, int id, const char *key) {
  auto it = g_cfg.find(key);
  if (it != g_cfg.end() && it->is_number())
    setText(dlg, id, fmtDouble(it->get<double>()));
  else
    setText(dlg, id, L"");
}

void setStatus(const std::wstring &w) {
  if (g_main)
    setText(g_main, IDC_STATUS, w);
}

void markDirty() {
  if (g_loading)
    return;
  g_dirty = true;
  if (g_main)
    setEnabled(g_main, IDC_APPLY, true);
}

// ------------------------------------------------------------- emplacements --

std::string cfgDir() { return oscompat::configDir(); }
std::string dataDir() { return oscompat::dataDir(); }
std::string configPath() { return cfgDir() + "\\config.json"; }
std::string keyPath() { return dataDir() + "\\groq.key"; }

std::wstring exeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  size_t k = p.find_last_of(L'\\');
  return k == std::wstring::npos ? p : p.substr(0, k);
}

bool fileExists(const std::wstring &p) {
  DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void openExternal(const std::wstring &target) {
  ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Ouvre un fichier texte dans l'éditeur associé, en le créant vide s'il
// n'existe pas (dict.txt, snippets.tsv) — sinon ShellExecute échoue en silence.
void openTextFile(const std::string &path, const char *seed) {
  oscompat::mkdirTree(cfgDir());
  if (!fileExists(wide(path))) {
    std::ofstream f(path, std::ios::binary);
    if (seed)
      f << seed;
  }
  openExternal(wide(path));
}

// Lance un utilitaire système (schtasks, taskkill) sans fenêtre et attend son
// code de retour. -1 si CreateProcess échoue.
int runHidden(const std::wstring &cmdline, DWORD timeoutMs = 15000) {
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = cmdline;  // CreateProcess veut un buffer modifiable
  if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return -1;
  ::WaitForSingleObject(pi.hProcess, timeoutMs);
  DWORD code = DWORD(-1);
  ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return int(code);
}

// ---------------------------------------------------------------- config --

void loadConfig() {
  g_cfg = json::object();
  std::ifstream f(configPath());
  if (!f)
    return;
  try {
    json j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
    if (j.is_object())
      g_cfg = std::move(j);
  } catch (...) {
    // Fichier invalide : on repart des défauts, mais on le signale — l'utilisateur
    // saura que « Appliquer » va le réécrire proprement.
    setStatus(L"config.json illisible : il sera réécrit à l'enregistrement.");
  }
}

bool saveConfig() {
  oscompat::mkdirTree(cfgDir());
  std::string path = oscompat::realPath(configPath());
  std::string text = g_cfg.dump(2) + "\n";
  return oscompat::replaceFileAtomic(path, text);
}

// ------------------------------------------------------------------- clé --

std::string readKeyFile() {
  std::ifstream f(keyPath(), std::ios::binary);
  if (!f)
    return {};
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return trim(s);
}

bool writeKeyFile(const std::string &key) {
  oscompat::mkdirTree(dataDir());
  return oscompat::replaceFileAtomic(keyPath(), key + "\n");
}

bool envKeySet() {
  wchar_t buf[8];
  return ::GetEnvironmentVariableW(L"GROQ_API_KEY", buf, 8) > 0;
}

std::wstring maskKey(const std::string &k) {
  if (k.size() <= 8)
    return L"••••";
  return wide(k.substr(0, 4)) + L"…" + wide(k.substr(k.size() - 4));
}

// ----------------------------------------------------------------- daemon --

bool daemonRunning() {
  sock_t fd = core::connectDaemon(400);
  if (!oscompat::sockValid(fd))
    return false;
  oscompat::sockClose(fd);
  return true;
}

// Où trouver predictord.exe et le modèle quand la tâche planifiée n'existe pas
// (installation depuis les sources sans setup, ou tâche supprimée).
bool findDaemonBinary(std::wstring &exe, std::wstring &words) {
  std::wstring app = exeDir();
  std::wstring data = wide(dataDir());
  for (const std::wstring &e : {app + L"\\predictord.exe", data + L"\\bin\\predictord.exe"})
    if (fileExists(e)) {
      exe = e;
      break;
    }
  for (const std::wstring &w : {data + L"\\model\\words.tsv", app + L"\\model\\words.tsv"})
    if (fileExists(w)) {
      words = w;
      break;
    }
  return !exe.empty() && !words.empty();
}

bool waitDaemon(bool wantRunning, DWORD ms) {
  DWORD t0 = ::GetTickCount();
  while (::GetTickCount() - t0 < ms) {
    if (daemonRunning() == wantRunning)
      return true;
    ::Sleep(300);
  }
  return daemonRunning() == wantRunning;
}

bool startDaemon(std::wstring &why) {
  // La tâche de session est le chemin normal : même environnement qu'à
  // l'ouverture de session, redémarrage automatique en cas de plantage.
  int rc = runHidden(std::wstring(L"schtasks.exe /Run /TN \"") + kTaskName + L"\"");
  if (rc == 0)
    return waitDaemon(true, 20000) || (why = L"la tâche a démarré mais le daemon ne répond pas (voir le journal)", false);
  std::wstring exe, words;
  if (!findDaemonBinary(exe, words)) {
    why = L"tâche « ime-predictord » absente et predictord.exe/words.tsv introuvables — relancez setup-windows.ps1";
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe + L"\" \"" + words + L"\"";
  std::wstring cwd = exe.substr(0, exe.find_last_of(L'\\'));
  if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, cwd.c_str(),
                        &si, &pi)) {
    why = L"lancement direct de predictord.exe impossible";
    return false;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  if (waitDaemon(true, 20000))
    return true;
  why = L"predictord.exe lancé mais ne répond pas (voir le journal)";
  return false;
}

bool stopDaemon(std::wstring &why) {
  runHidden(std::wstring(L"schtasks.exe /End /TN \"") + kTaskName + L"\"");
  runHidden(L"taskkill.exe /F /IM predictord.exe");
  if (waitDaemon(false, 8000))
    return true;
  why = L"predictord.exe tourne toujours";
  return false;
}

// -------------------------------------------------------------------- IME --

std::wstring registeredTsfPath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD cb = sizeof buf;
  if (::RegGetValueW(HKEY_CLASSES_ROOT, kClsidKey, nullptr, RRF_RT_REG_SZ, nullptr, buf,
                     &cb) != ERROR_SUCCESS)
    return {};
  return buf;
}

std::wstring ownVersion() {
  wchar_t path[MAX_PATH];
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  DWORD dummy = 0;
  DWORD sz = ::GetFileVersionInfoSizeW(path, &dummy);
  if (!sz)
    return L"?";
  std::vector<char> buf(sz);
  if (!::GetFileVersionInfoW(path, 0, sz, buf.data()))
    return L"?";
  VS_FIXEDFILEINFO *ffi = nullptr;
  UINT len = 0;
  if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&ffi), &len) || !ffi)
    return L"?";
  wchar_t out[64];
  swprintf(out, 64, L"%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
           HIWORD(ffi->dwFileVersionLS));
  return out;
}

// ===================================================================== pages

enum Page { kGeneral = 0, kReform, kAdvanced, kService };

// Liste des clés « Avancé » : identifiant de champ ↔ clé JSON. Un champ vide
// efface la clé (le daemon reprend alors sa valeur par défaut).
struct NumField {
  int id;
  const char *key;
  bool integer;
};
const NumField kAdvancedFields[] = {
    {IDC_AGREE, "agreeBoost", false},         {IDC_RECENCY, "recencyBoost", false},
    {IDC_LEARNED, "learnedBoost", false},     {IDC_LEARNEDFLOOR, "learnedFloor", true},
    {IDC_PROCLISIS, "proclisisDemote", false}, {IDC_AUTODOM, "autoDom", false},
    {IDC_SOCKTIMEOUT, "socketTimeoutMs", true}, {IDC_NEXTTIMEOUT, "nextWordTimeoutMs", true},
};

const wchar_t *kLangValues[] = {L"auto", L"fr", L"en", L"off"};
const wchar_t *kLangLabels[] = {L"Automatique (vote du contexte)", L"Français", L"English",
                                L"Libre (aucun boost de langue)"};

void fillGeneral(HWND d) {
  HWND lang = ::GetDlgItem(d, IDC_LANG);
  if (::SendMessageW(lang, CB_GETCOUNT, 0, 0) == 0)
    for (const wchar_t *l : kLangLabels)
      ::SendMessageW(lang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(l));
  std::string cur = g_cfg.value("lang", std::string("auto"));
  int sel = 0;
  for (int i = 0; i < 4; ++i)
    if (utf8(kLangValues[i]) == cur)
      sel = i;
  ::SendMessageW(lang, CB_SETCURSEL, WPARAM(sel), 0);

  HWND bw = ::GetDlgItem(d, IDC_BARWORDS);
  if (::SendMessageW(bw, CB_GETCOUNT, 0, 0) == 0)
    for (int i = 1; i <= 8; ++i) {
      wchar_t s[4];
      swprintf(s, 4, L"%d", i);
      ::SendMessageW(bw, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
  int words = g_cfg.value("barWords", 5);
  words = (std::max)(1, (std::min)(8, words));
  ::SendMessageW(bw, CB_SETCURSEL, WPARAM(words - 1), 0);

  setCheck(d, IDC_NEXTWORDBAR, g_cfg.value("nextWordBar", true));
  setCheck(d, IDC_MULTIWORD, g_cfg.value("multiWord", true));
  setCheck(d, IDC_ESCAPEFORWARD, g_cfg.value("escapeForward", true));
  setCheck(d, IDC_AUTOAPPLY, g_cfg.value("autoApply", true));
  setCheck(d, IDC_ACCENTRESTORE, g_cfg.value("accentRestore", true));
  setCheck(d, IDC_FRENCHSPACING, g_cfg.value("frenchSpacing", false));
  setCheck(d, IDC_AUTOCAP, g_cfg.value("autoCapitalize", false));

  std::string excl;
  for (const auto &e : g_cfg.value("nextWordBarExclude", json::array()))
    if (e.is_string())
      excl += (excl.empty() ? "" : ", ") + e.get<std::string>();
  setText(d, IDC_EXCLUDE, wide(excl));
}

void collectGeneral(HWND d) {
  int sel = int(::SendMessageW(::GetDlgItem(d, IDC_LANG), CB_GETCURSEL, 0, 0));
  g_cfg["lang"] = utf8(kLangValues[(std::max)(0, (std::min)(3, sel))]);
  int bw = int(::SendMessageW(::GetDlgItem(d, IDC_BARWORDS), CB_GETCURSEL, 0, 0)) + 1;
  g_cfg["barWords"] = (std::max)(1, (std::min)(8, bw));
  g_cfg["nextWordBar"] = getCheck(d, IDC_NEXTWORDBAR);
  g_cfg["multiWord"] = getCheck(d, IDC_MULTIWORD);
  g_cfg["escapeForward"] = getCheck(d, IDC_ESCAPEFORWARD);
  g_cfg["autoApply"] = getCheck(d, IDC_AUTOAPPLY);
  g_cfg["accentRestore"] = getCheck(d, IDC_ACCENTRESTORE);
  g_cfg["frenchSpacing"] = getCheck(d, IDC_FRENCHSPACING);
  g_cfg["autoCapitalize"] = getCheck(d, IDC_AUTOCAP);

  json excl = json::array();
  std::string raw = utf8(getText(d, IDC_EXCLUDE));
  size_t start = 0;
  while (start <= raw.size()) {
    size_t comma = raw.find(',', start);
    std::string item = trim(raw.substr(start, comma == std::string::npos ? std::string::npos
                                                                         : comma - start));
    if (!item.empty())
      excl.push_back(item);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  if (excl.empty())
    g_cfg.erase("nextWordBarExclude");
  else
    g_cfg["nextWordBarExclude"] = excl;
}

void refreshKeyStatus(HWND d) {
  std::string k = readKeyFile();
  std::wstring s;
  if (envKeySet())
    s = L"La variable d'environnement GROQ_API_KEY est définie : elle a priorité sur le fichier.";
  else if (k.empty())
    s = L"Aucune clé enregistrée — la reformulation affichera « fournir une clé ».";
  else
    s = L"Clé enregistrée (" + maskKey(k) + L") dans " + wide(keyPath());
  setText(d, IDC_KEYSTATUS, s);
  setEnabled(d, IDC_DELKEY, !k.empty());
}

void fillReform(HWND d) {
  setText(d, IDC_APIKEY, wide(readKeyFile()));
  refreshKeyStatus(d);
  setText(d, IDC_BASEURL,
          wide(g_cfg.value("reformBaseUrl",
                           std::string("https://api.groq.com/openai/v1/chat/completions"))));
  setText(d, IDC_MODEL, wide(g_cfg.value("reformModel", std::string("llama-3.3-70b-versatile"))));
  setText(d, IDC_TIMEOUT, fmtDouble(g_cfg.value("reformTimeoutMs", 8000)));
  HWND rc = ::GetDlgItem(d, IDC_REFORMCOUNT);
  if (::SendMessageW(rc, CB_GETCOUNT, 0, 0) == 0)
    for (int i = 1; i <= 6; ++i) {
      wchar_t s[4];
      swprintf(s, 4, L"%d", i);
      ::SendMessageW(rc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
  int n = (std::max)(1, (std::min)(6, g_cfg.value("reformCount", 3)));
  ::SendMessageW(rc, CB_SETCURSEL, WPARAM(n - 1), 0);
}

void collectReform(HWND d) {
  std::string url = trim(utf8(getText(d, IDC_BASEURL)));
  std::string model = trim(utf8(getText(d, IDC_MODEL)));
  if (url.empty())
    g_cfg.erase("reformBaseUrl");
  else
    g_cfg["reformBaseUrl"] = url;
  if (model.empty())
    g_cfg.erase("reformModel");
  else
    g_cfg["reformModel"] = model;
  double t = 0;
  if (getNumber(d, IDC_TIMEOUT, t) && t >= 1000)
    g_cfg["reformTimeoutMs"] = int(t);
  else
    g_cfg.erase("reformTimeoutMs");
  int n = int(::SendMessageW(::GetDlgItem(d, IDC_REFORMCOUNT), CB_GETCURSEL, 0, 0)) + 1;
  g_cfg["reformCount"] = (std::max)(1, (std::min)(6, n));
}

void fillAdvanced(HWND d) {
  for (const NumField &f : kAdvancedFields)
    setNumber(d, f.id, f.key);
}

void collectAdvanced(HWND d) {
  for (const NumField &f : kAdvancedFields) {
    double v = 0;
    if (getNumber(d, f.id, v)) {
      if (f.integer)
        g_cfg[f.key] = int(std::llround(v));
      else
        g_cfg[f.key] = v;
    } else {
      g_cfg.erase(f.key);
    }
  }
}

void refreshService(HWND d) {
  bool up = daemonRunning();
  setText(d, IDC_DAEMONSTATUS,
          up ? L"● En marche — répond sur " + wide(core::daemonSocketPath())
             : L"○ Arrêté — aucune suggestion ne sera proposée tant qu'il ne tourne pas.");
  setEnabled(d, IDC_START, !up);
  setEnabled(d, IDC_STOP, up);
  setEnabled(d, IDC_RESTART, up);
  setEnabled(d, IDC_TEST, up);

  std::wstring tsf = registeredTsfPath();
  if (tsf.empty())
    setText(d, IDC_IMESTATUS,
            L"Non enregistrée — « Predict » n'apparaît pas dans Win+Espace. Relancez l'installeur "
            L"ou setup-windows.ps1 en administrateur.");
  else if (!fileExists(tsf))
    setText(d, IDC_IMESTATUS, L"Enregistrée mais la DLL est ABSENTE : " + tsf);
  else
    setText(d, IDC_IMESTATUS, L"Enregistrée : " + tsf + L"\nChoisissez « Predict » avec Win+Espace.");
}

void fillService(HWND d) {
  if (getText(d, IDC_TCONTEXT).empty()) {
    setText(d, IDC_TCONTEXT, L"je");
    setText(d, IDC_TPREFIX, L"v");
  }
  setText(d, IDC_VERSION, L"predictive-ime " + ownVersion() + L" — code MIT, modèle CC BY-SA 4.0");
  refreshService(d);
}

void runQuickTest(HWND d) {
  std::vector<std::string> ctx;
  std::string raw = utf8(getText(d, IDC_TCONTEXT));
  size_t i = 0;
  while (i < raw.size()) {
    size_t sp = raw.find(' ', i);
    std::string w = trim(raw.substr(i, sp == std::string::npos ? std::string::npos : sp - i));
    if (!w.empty())
      ctx.push_back(w);
    if (sp == std::string::npos)
      break;
    i = sp + 1;
  }
  std::string prefix = trim(utf8(getText(d, IDC_TPREFIX)));
  core::DaemonReply r = core::queryDaemon(ctx, prefix);
  if (r.candidates.empty()) {
    setText(d, IDC_TRESULT, daemonRunning() ? L"Aucun candidat pour cette entrée."
                                            : L"Le daemon ne répond pas.");
    return;
  }
  std::string out;
  for (const auto &c : r.candidates)
    out += (out.empty() ? "" : "   ") + c;
  if (!r.autocomplete.empty())
    out += "\nEspace appliquerait : " + r.autocomplete;
  setText(d, IDC_TRESULT, wide(out));
}

void clearLearned(HWND d) {
  int rc = ::MessageBoxW(
      d,
      L"Oublier tous les mots appris ?\n\nLes journaux user.log, user.tri.log et veto.log seront "
      L"supprimés et le daemon redémarré. Votre dict.txt et vos réglages sont conservés.",
      kMainClassTitle, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2);
  if (rc != IDOK)
    return;
  std::wstring why;
  bool wasUp = daemonRunning();
  if (wasUp && !stopDaemon(why)) {
    ::MessageBoxW(d, (L"Impossible d'arrêter le daemon : " + why).c_str(), kMainClassTitle,
                  MB_ICONERROR);
    return;
  }
  for (const char *name : {"user.log", "user.tri.log", "veto.log"})
    oscompat::unlinkFile(dataDir() + "\\" + name);
  if (wasUp)
    startDaemon(why);
  refreshService(d);
  setStatus(L"Mots appris effacés.");
}

// Validation de la clé : le daemon fait l'appel réseau (même code que la
// reformulation réelle) ; on attend sur un thread pour ne pas figer la fenêtre.
void testKeyAsync(HWND d) {
  setEnabled(d, IDC_TESTKEY, false);
  setText(d, IDC_KEYSTATUS, L"Vérification auprès du service…");
  std::thread([d] {
    trace("worker: check start");
    core::ReformResult r = core::reformCheckDaemon();
    trace("worker: check done err=%s src=%s", r.error.c_str(), r.source.c_str());
    std::wstring msg;
    bool ok = false;
    if (r.error.empty() && r.source.empty())
      msg = L"Le daemon ne répond pas : démarrez-le (onglet Service) puis réessayez.";
    else if (r.source == "groq") {
      ok = true;
      msg = L"Clé valide : le service répond.";
    } else if (r.error == "no_key")
      msg = L"Aucune clé trouvée par le daemon — enregistrez-la d'abord.";
    else if (r.error == "auth")
      msg = L"Clé refusée par le service (401/403) : vérifiez-la.";
    else if (r.error == "network")
      msg = L"Réseau injoignable ou délai dépassé.";
    else
      msg = L"Le service a répondu une erreur (" + wide(r.error) + L").";
    ::PostMessageW(d, WM_APP_KEYRESULT, WPARAM(ok), reinterpret_cast<LPARAM>(new std::wstring(msg)));
  }).detach();
}

INT_PTR CALLBACK pageProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_INITDIALOG:
    ::SetWindowLongPtrW(d, GWLP_USERDATA, LONG_PTR(lp));
    return TRUE;

  case WM_TIMER:
    if (wp == kStatusTimer) {
      refreshService(d);
    }
    return TRUE;

  case WM_APP_KEYRESULT: {
    trace("keyresult in");
    auto *s = reinterpret_cast<std::wstring *>(lp);
    setText(d, IDC_KEYSTATUS, *s);
    setStatus(wp ? L"Clé API validée." : L"Échec de la validation de la clé.");
    delete s;
    setEnabled(d, IDC_TESTKEY, true);
    trace("keyresult out");
    return TRUE;
  }

  case WM_COMMAND: {
    int id = LOWORD(wp);
    int code = HIWORD(wp);
    // Toute modification d'un champ de réglage arme « Appliquer ».
    if ((code == EN_CHANGE || code == CBN_SELCHANGE) &&
        id != IDC_APIKEY && id != IDC_TCONTEXT && id != IDC_TPREFIX) {
      markDirty();
      return TRUE;
    }
    if (code != BN_CLICKED)
      return FALSE;
    switch (id) {
    case IDC_NEXTWORDBAR: case IDC_MULTIWORD: case IDC_ESCAPEFORWARD:
    case IDC_AUTOAPPLY: case IDC_ACCENTRESTORE: case IDC_FRENCHSPACING: case IDC_AUTOCAP:
      markDirty();
      return TRUE;

    case IDC_SHOWKEY: {
      HWND e = ::GetDlgItem(d, IDC_APIKEY);
      ::SendMessageW(e, EM_SETPASSWORDCHAR, getCheck(d, IDC_SHOWKEY) ? 0 : WPARAM(L'\x25CF'), 0);
      ::InvalidateRect(e, nullptr, TRUE);
      return TRUE;
    }
    case IDC_SAVEKEY: {
      std::string k = trim(utf8(getText(d, IDC_APIKEY)));
      if (k.empty()) {
        ::MessageBoxW(d, L"Collez d'abord la clé dans le champ.", kMainClassTitle, MB_ICONINFORMATION);
        return TRUE;
      }
      if (writeKeyFile(k))
        setStatus(L"Clé API enregistrée — prise en compte à la prochaine reformulation.");
      else
        ::MessageBoxW(d, (L"Écriture impossible : " + wide(keyPath())).c_str(), kMainClassTitle,
                      MB_ICONERROR);
      refreshKeyStatus(d);
      return TRUE;
    }
    case IDC_DELKEY:
      if (::MessageBoxW(d, L"Supprimer la clé API enregistrée ?", kMainClassTitle,
                        MB_ICONQUESTION | MB_OKCANCEL) == IDOK) {
        oscompat::unlinkFile(keyPath());
        setText(d, IDC_APIKEY, L"");
        refreshKeyStatus(d);
        setStatus(L"Clé API supprimée.");
      }
      return TRUE;
    case IDC_TESTKEY: {
      // La clé du champ est enregistrée avant le test : c'est celle-là que
      // le daemon lit, et c'est ce que l'utilisateur veut vérifier.
      std::string k = trim(utf8(getText(d, IDC_APIKEY)));
      if (!k.empty() && k != readKeyFile())
        writeKeyFile(k);
      refreshKeyStatus(d);
      trace("testkey: status refreshed");
      testKeyAsync(d);
      trace("testkey: thread started");
      return TRUE;
    }
    case IDC_GETKEY:
      openExternal(kGroqKeysUrl);
      return TRUE;

    case IDC_RESETADV:
      for (const NumField &f : kAdvancedFields)
        setText(d, f.id, L"");
      markDirty();
      return TRUE;

    case IDC_START: case IDC_STOP: case IDC_RESTART: {
      std::wstring why;
      bool ok = true;
      ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
      if (id != IDC_START)
        ok = stopDaemon(why);
      if (ok && id != IDC_STOP)
        ok = startDaemon(why);
      refreshService(d);
      if (!ok)
        ::MessageBoxW(d, why.c_str(), kMainClassTitle, MB_ICONERROR);
      else
        setStatus(id == IDC_STOP ? L"Daemon arrêté." : L"Daemon en marche.");
      return TRUE;
    }
    case IDC_OPENLOG:
      openExternal(wide(dataDir() + "\\predictord.log"));
      return TRUE;
    case IDC_TEST:
      runQuickTest(d);
      return TRUE;
    case IDC_OPENCFG:
      oscompat::mkdirTree(cfgDir());
      openExternal(wide(cfgDir()));
      return TRUE;
    case IDC_OPENDATA:
      openExternal(wide(dataDir()));
      return TRUE;
    case IDC_OPENDICT:
      openTextFile(cfgDir() + "\\dict.txt",
                   "# Un mot par ligne, fréquence optionnelle : \"Titosy 500\". Jamais corrigés, toujours complétés.\n");
      return TRUE;
    case IDC_OPENSNIP:
      openTextFile(cfgDir() + "\\snippets.tsv",
                   "# abréviation<TAB>texte développé — une ligne par snippet.\n");
      return TRUE;
    case IDC_CLEARLEARNED:
      clearLearned(d);
      return TRUE;
    case IDC_GITHUB:
      openExternal(kProjectUrl);
      return TRUE;
    }
    return FALSE;
  }
  }
  return FALSE;
}

// ================================================================= principal

void fillPages() {
  g_loading = true;
  fillGeneral(g_pages[kGeneral]);
  fillReform(g_pages[kReform]);
  fillAdvanced(g_pages[kAdvanced]);
  fillService(g_pages[kService]);
  g_loading = false;
  g_dirty = false;
  setEnabled(g_main, IDC_APPLY, false);
}

void collectPages() {
  collectGeneral(g_pages[kGeneral]);
  collectReform(g_pages[kReform]);
  collectAdvanced(g_pages[kAdvanced]);
}

bool applyAll() {
  collectPages();
  if (!saveConfig()) {
    ::MessageBoxW(g_main, (L"Impossible d'écrire " + wide(configPath())).c_str(), kMainClassTitle,
                  MB_ICONERROR);
    return false;
  }
  g_dirty = false;
  setEnabled(g_main, IDC_APPLY, false);
  setStatus(L"Réglages enregistrés — relus immédiatement par le daemon et le service texte.");
  return true;
}

void layoutPages() {
  // Les pages sont enfants du DIALOGUE PRINCIPAL, pas du contrôle d'onglets :
  // un dialogue enfant (DS_CONTROL) posé dans un contrôle qui n'est pas un
  // dialogue égare le gestionnaire de dialogue (bouton par défaut, ordre de
  // tabulation), jusqu'à boucler sans fin au clic sur un bouton. On les place
  // donc au-dessus de la zone d'affichage de l'onglet, en coordonnées du parent.
  RECT rc;
  ::GetWindowRect(g_tab, &rc);
  ::MapWindowPoints(nullptr, g_main, reinterpret_cast<POINT *>(&rc), 2);
  TabCtrl_AdjustRect(g_tab, FALSE, &rc);
  for (HWND p : g_pages)
    if (p)
      ::SetWindowPos(p, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE);
}

void showPage(int idx) {
  for (int i = 0; i < 4; ++i)
    ::ShowWindow(g_pages[i], i == idx ? SW_SHOW : SW_HIDE);
  if (idx == kService)
    refreshService(g_pages[kService]);
  // Le focus ne doit JAMAIS rester sur un contrôle d'une page cachée : à la
  // prochaine activation, le gestionnaire de dialogue cherche un successeur
  // visible à partir de ce contrôle et boucle sans fin (fenêtre figée à 100 %
  // CPU). Le cas se présentait au lancement avec --reform : Windows avait
  // donné le focus à la première page avant qu'on la cache.
  HWND f = ::GetFocus();
  if (!f || !::IsWindowVisible(f))
    ::SetFocus(g_tab);
}

INT_PTR CALLBACK mainProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_INITDIALOG: {
    g_main = d;
    g_tab = ::GetDlgItem(d, IDC_TABS);
    HICON big = ::LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_PREDICT));
    ::SendMessageW(d, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    ::SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(big));

    const wchar_t *titles[] = {L"Général", L"Reformulation (IA)", L"Avancé", L"Service"};
    const int ids[] = {IDD_PAGE_GENERAL, IDD_PAGE_REFORM, IDD_PAGE_ADVANCED, IDD_PAGE_SERVICE};
    for (int i = 0; i < 4; ++i) {
      TCITEMW it{};
      it.mask = TCIF_TEXT;
      it.pszText = const_cast<wchar_t *>(titles[i]);
      TabCtrl_InsertItem(g_tab, i, &it);
      g_pages[i] = ::CreateDialogParamW(g_inst, MAKEINTRESOURCEW(ids[i]), d, pageProc, i);
    }
    layoutPages();
    loadConfig();
    fillPages();
    // L'onglet demandé (--reform…) est sélectionné APRÈS l'affichage, par le
    // même chemin qu'un clic sur son en-tête : montrer une autre page que la
    // première pendant WM_INITDIALOG laissait le gestionnaire de dialogue
    // avec un focus incohérent, et le premier clic dans la page le faisait
    // boucler sans fin (fenêtre figée à 100 % CPU).
    showPage(kGeneral);
    if (int(lp) != kGeneral)
      ::PostMessageW(d, WM_APP_GOTOPAGE, WPARAM(lp), 0);
    ::SetTimer(g_pages[kService], kStatusTimer, 3000, nullptr);
    setStatus(L"");
    return TRUE;
  }

  case WM_NOTIFY: {
    auto *hdr = reinterpret_cast<NMHDR *>(lp);
    if (hdr->idFrom == IDC_TABS && hdr->code == TCN_SELCHANGE)
      showPage(TabCtrl_GetCurSel(g_tab));
    return TRUE;
  }

  case WM_DPICHANGED:
    // Windows remet le dialogue et ses contrôles à l'échelle ; les pages
    // (dialogues enfants) sont repositionnées après coup.
    ::PostMessageW(d, WM_APP_RELAYOUT, 0, 0);
    return FALSE;
  case WM_APP_RELAYOUT:
    layoutPages();
    return TRUE;
  case WM_APP_GOTOPAGE:
    TabCtrl_SetCurSel(g_tab, int(wp));
    showPage(int(wp));
    ::SetFocus(g_tab);
    return TRUE;

  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case IDOK:
      if (!g_dirty || applyAll())
        ::EndDialog(d, IDOK);
      return TRUE;
    case IDCANCEL:
      if (g_dirty &&
          ::MessageBoxW(d, L"Des réglages ne sont pas enregistrés. Quitter sans les appliquer ?",
                        kMainClassTitle, MB_ICONQUESTION | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK)
        return TRUE;
      ::EndDialog(d, IDCANCEL);
      return TRUE;
    case IDC_APPLY:
      applyAll();
      return TRUE;
    }
    return FALSE;
  }
  return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
  g_inst = inst;

  // Une seule fenêtre : un second lancement ramène la première au premier plan.
  HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kMutexName);
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND prev = ::FindWindowW(nullptr, kMainClassTitle)) {
      ::ShowWindow(prev, SW_RESTORE);
      ::SetForegroundWindow(prev);
    }
    return 0;
  }

  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
  ::InitCommonControlsEx(&icc);
  if (!oscompat::netInit()) {
    ::MessageBoxW(nullptr, L"Initialisation réseau (Winsock) impossible.", kMainClassTitle,
                  MB_ICONERROR);
    return 1;
  }

  // predict-admin --reform : ouvre directement sur la clé API (lien depuis le
  // panneau « fournir une clé » de la barre).
  int page = kGeneral;
  std::wstring args = cmdLine ? cmdLine : L"";
  if (args.find(L"--reform") != std::wstring::npos || args.find(L"--key") != std::wstring::npos)
    page = kReform;
  else if (args.find(L"--service") != std::wstring::npos)
    page = kService;

  ::DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, mainProc, LPARAM(page));
  if (mutex)
    ::CloseHandle(mutex);
  return 0;
}
