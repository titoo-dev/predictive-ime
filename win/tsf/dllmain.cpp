// Plomberie COM + enregistrement du text service.
//
// Ce que « installer une IME » veut dire sous Windows :
//   1. une DLL COM in-process inscrite sous HKCR\CLSID (DllRegisterServer) ;
//   2. un PROFIL clavier déclaré à TSF (ITfInputProcessorProfileMgr) ;
//   3. des CATÉGORIES qui disent à Windows de quoi le service est capable.
// Sans (3), le service n'apparaît pas dans les applications modernes (UWP,
// écran de connexion) — c'est le piège classique.
#include "Guids.h"
#include "TextService.h"

#include <msctf.h>
#include <new>
#include <olectl.h>
#include <string>
#include <windows.h>

namespace {

HINSTANCE g_inst = nullptr;
LONG g_dllRefs = 0;

// Langues sous lesquelles le profil est proposé. Le modèle est FR/EN : on
// s'inscrit sous les deux, l'utilisateur retire ce qu'il ne veut pas.
struct ProfileLang {
  LANGID langid;
  const wchar_t *tag;
};
const ProfileLang kLangs[] = {
    {MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH), L"fr-FR"},
    {MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), L"en-US"},
};

// Catégories à déclarer. TIPCAP_IMMERSIVESUPPORT et SECUREMODE sont ce qui
// rend le service utilisable dans les applications du Store et sur les
// bureaux sécurisés ; UIELEMENTENABLED dit qu'on dessine notre propre UI.
const GUID *kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_SECUREMODE,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
};

std::wstring guidToString(REFGUID g) {
  wchar_t buf[64]{};
  ::StringFromGUID2(g, buf, 64);
  return buf;
}

bool setRegString(HKEY root, const std::wstring &sub, const wchar_t *name,
                  const std::wstring &value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
    return false;
  LONG r = ::RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
      DWORD((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return r == ERROR_SUCCESS;
}

// Fabrique COM — une seule classe, le text service.
class CClassFactory : public IClassFactory {
public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
      *ppv = static_cast<IClassFactory *>(this);
    if (!*ppv)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    ::InterlockedIncrement(&g_dllRefs);
    return 2; // singleton statique : le compte n'a pas à être exact
  }
  STDMETHODIMP_(ULONG) Release() override {
    ::InterlockedDecrement(&g_dllRefs);
    return 1;
  }
  STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid,
                              void **ppv) override {
    if (!ppv)
      return E_INVALIDARG;
    *ppv = nullptr;
    if (outer)
      return CLASS_E_NOAGGREGATION;
    auto *svc = new (std::nothrow) win::CTextService();
    if (!svc)
      return E_OUTOFMEMORY;
    HRESULT hr = svc->QueryInterface(riid, ppv);
    svc->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock)
      ::InterlockedIncrement(&g_dllRefs);
    else
      ::InterlockedDecrement(&g_dllRefs);
    return S_OK;
  }
};

CClassFactory g_factory;

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_inst = inst;
    ::DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
  if (!ppv)
    return E_INVALIDARG;
  *ppv = nullptr;
  if (!IsEqualCLSID(rclsid, CLSID_PredictTextService))
    return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_dllRefs > 0 ? S_FALSE : S_OK; }

STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH]{};
  if (::GetModuleFileNameW(g_inst, path, MAX_PATH) == 0)
    return SELFREG_E_CLASS;

  // 1. la classe COM
  const std::wstring clsid = guidToString(CLSID_PredictTextService);
  const std::wstring base = L"CLSID\\" + clsid;
  if (!setRegString(HKEY_CLASSES_ROOT, base, nullptr, PREDICT_SERVICE_DESC) ||
      !setRegString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", nullptr,
                    path) ||
      // Apartment : TSF appelle le service sur le thread d'interface de
      // l'application. Un service marqué Both/Free serait appelé depuis
      // n'importe où et corromprait l'état de composition.
      !setRegString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32",
                    L"ThreadingModel", L"Apartment"))
    return SELFREG_E_CLASS;

  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return SELFREG_E_CLASS;

  HRESULT hr = SELFREG_E_CLASS;
  ITfInputProcessorProfileMgr *mgr = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr,
                                   (void **)&mgr)) &&
      mgr) {
    hr = S_OK;
    for (const auto &lang : kLangs) {
      // 12 arguments exactement : desc+longueur, chemin d'icône+longueur,
      // index d'icône, HKL de substitution, disposition préférée, activé par
      // défaut, drapeaux.
      HRESULT r = mgr->RegisterProfile(
          CLSID_PredictTextService, lang.langid, GUID_PredictProfile,
          PREDICT_SERVICE_NAME, ULONG(wcslen(PREDICT_SERVICE_NAME)), path,
          ULONG(wcslen(path)), 0, nullptr, 0, TRUE, 0);
      if (FAILED(r))
        hr = r;
    }
    mgr->Release();
  }

  // 3. les catégories — sans elles, le service reste invisible des
  //    applications modernes.
  ITfCategoryMgr *cat = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                   (void **)&cat)) &&
      cat) {
    for (const GUID *g : kCategories)
      cat->RegisterCategory(CLSID_PredictTextService, *g,
                            CLSID_PredictTextService);
    cat->Release();
  }

  ::CoUninitialize();
  return hr;
}

STDAPI DllUnregisterServer() {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return SELFREG_E_CLASS;

  ITfInputProcessorProfileMgr *mgr = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr,
                                   (void **)&mgr)) &&
      mgr) {
    for (const auto &lang : kLangs)
      mgr->UnregisterProfile(CLSID_PredictTextService, lang.langid,
                             GUID_PredictProfile, 0);
    mgr->Release();
  }
  ITfCategoryMgr *cat = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                   (void **)&cat)) &&
      cat) {
    for (const GUID *g : kCategories)
      cat->UnregisterCategory(CLSID_PredictTextService, *g,
                              CLSID_PredictTextService);
    cat->Release();
  }
  ::CoUninitialize();

  const std::wstring clsid = guidToString(CLSID_PredictTextService);
  ::RegDeleteTreeW(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str());
  return S_OK;
}
