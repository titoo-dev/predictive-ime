#include "Module.h"

#include <new>
#include <process.h>
#include <string>
#include <vector>

namespace win {
namespace {

LONG g_moduleRefs = 0;

// Classes de fenêtre inscrites par ce module, à désinscrire au déchargement.
struct ClassRegistry {
  ClassRegistry() { ::InitializeCriticalSection(&lock); }
  CRITICAL_SECTION lock;
  std::vector<std::wstring> names;
};
ClassRegistry &classes() {
  static ClassRegistry r; // jamais détruit : DllMain(DETACH) peut le lire
  return r;
}

struct DetachedThread {
  std::function<void()> fn;
  HMODULE self;
};

unsigned __stdcall detachedThreadMain(void *arg) {
  auto *ctx = static_cast<DetachedThread *>(arg);
  HMODULE self = ctx->self;
  ctx->fn();
  delete ctx; // avant de lâcher le module : le destructeur est DANS ce module
  ::FreeLibraryAndExitThread(self, 0);
}

} // namespace

HINSTANCE dllInstance() {
  // Le module qui contient CE code — la DLL du text service, ou l'exécutable
  // quand CandidateWindow est liée dans un outil ou un test.
  static HMODULE self = [] {
    HMODULE h = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&dllInstance), &h);
    return h;
  }();
  return self;
}

void dllAddRef() { ::InterlockedIncrement(&g_moduleRefs); }
void dllRelease() { ::InterlockedDecrement(&g_moduleRefs); }
bool dllHasRefs() { return g_moduleRefs > 0; }

bool registerWindowClass(WNDCLASSEXW &wc) {
  wc.cbSize = sizeof(wc);
  wc.hInstance = dllInstance();
  if (!::RegisterClassExW(&wc)) {
    // Déjà inscrite par CE module (même HINSTANCE, donc même WndProc) :
    // c'est le cas normal d'un second service sur un autre thread.
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  }
  ClassRegistry &r = classes();
  ::EnterCriticalSection(&r.lock);
  r.names.emplace_back(wc.lpszClassName);
  ::LeaveCriticalSection(&r.lock);
  return true;
}

void unregisterWindowClasses() {
  ClassRegistry &r = classes();
  ::EnterCriticalSection(&r.lock);
  for (const auto &name : r.names)
    ::UnregisterClassW(name.c_str(), dllInstance());
  r.names.clear();
  ::LeaveCriticalSection(&r.lock);
}

bool runDetached(std::function<void()> fn) {
  HMODULE self = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&detachedThreadMain),
                            &self))
    return false;
  auto *ctx = new (std::nothrow) DetachedThread{std::move(fn), self};
  if (!ctx) {
    ::FreeLibrary(self);
    return false;
  }
  uintptr_t h =
      ::_beginthreadex(nullptr, 0, detachedThreadMain, ctx, 0, nullptr);
  if (h == 0) {
    delete ctx;
    ::FreeLibrary(self);
    return false;
  }
  ::CloseHandle(reinterpret_cast<HANDLE>(h));
  return true;
}

} // namespace win
