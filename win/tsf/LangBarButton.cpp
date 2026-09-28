#include "LangBarButton.h"

#include "KeyboardLayout.h"
#include "StatusIcon.h"

#include "Guids.h"
#include "TextService.h"

#include <ctffunc.h>
#include <olectl.h>
#include <shellapi.h>
#include <string>

namespace win {
namespace {

constexpr DWORD kSinkCookie = 0x50524544; // un seul abonné : cookie fixe
enum MenuId : UINT { kMenuToggle = 1, kMenuSettings, kMenuHelp, kMenuFixLayout };
constexpr wchar_t kHelpUrl[] = L"https://github.com/titoo-dev/predictive-ime";

void addItem(ITfMenu *menu, UINT id, DWORD flags, const wchar_t *text) {
  menu->AddMenuItem(id, flags, nullptr, nullptr, text,
                    text ? ULONG(wcslen(text)) : 0, nullptr);
}

} // namespace

// ======================================================= CLangBarButton ====

CLangBarButton::CLangBarButton(CTextService *svc) : svc_(svc) {}

CLangBarButton::~CLangBarButton() {
  if (sink_)
    sink_->Release();
}

bool CLangBarButton::enabled() const {
  return svc_ ? svc_->predictionEnabled() : true;
}

void CLangBarButton::refresh() {
  if (sink_)
    sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

STDMETHODIMP CLangBarButton::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_INVALIDARG;
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
      IsEqualIID(riid, IID_ITfLangBarItemButton))
    *ppv = static_cast<ITfLangBarItemButton *>(this);
  else if (IsEqualIID(riid, IID_ITfSource))
    *ppv = static_cast<ITfSource *>(this);
  if (!*ppv)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) CLangBarButton::AddRef() {
  return ULONG(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) CLangBarButton::Release() {
  LONG c = InterlockedDecrement(&ref_);
  if (c == 0)
    delete this;
  return ULONG(c);
}

STDMETHODIMP CLangBarButton::GetInfo(TF_LANGBARITEMINFO *info) {
  if (!info)
    return E_INVALIDARG;
  info->clsidService = CLSID_PredictTextService;
  // GUID_LBI_INPUTMODE : c'est CE GUID qui vaut une place dans la barre des
  // tâches de Windows 8+ ; tout autre élément n'irait que dans l'ancienne
  // barre de langue flottante, que plus personne n'affiche.
  info->guidItem = GUID_LBI_INPUTMODE;
  info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
  info->ulSort = 0;
  wcsncpy_s(info->szDescription, PREDICT_SERVICE_NAME, _TRUNCATE);
  return S_OK;
}

STDMETHODIMP CLangBarButton::GetStatus(DWORD *status) {
  if (!status)
    return E_INVALIDARG;
  *status = 0;
  return S_OK;
}

STDMETHODIMP CLangBarButton::Show(BOOL) { return E_NOTIMPL; }

STDMETHODIMP CLangBarButton::GetTooltipString(BSTR *tip) {
  if (!tip)
    return E_INVALIDARG;
  std::wstring t = enabled() ? L"Predict — prédiction activée (clic : couper)"
                             : L"Predict — prédiction coupée (clic : activer)";
  // La disposition réellement active : si elle n'est pas celle de
  // l'utilisateur, les touches ET les raccourcis sont décalés — il faut que
  // ça se voie.
  layout::Status s = layout::current();
  if (!s.active.empty())
    t += (s.matches ? L"\nDisposition : " : L"\n⚠ Disposition : ") +
         layout::displayName(s.active);
  if (!s.matches && !s.wanted.empty())
    t += L" (la vôtre : " + layout::displayName(s.wanted) + L")";
  *tip = ::SysAllocString(t.c_str());
  return *tip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CLangBarButton::OnClick(TfLBIClick click, POINT, const RECT *) {
  // Le clic droit passe par InitMenu : le système dessine alors le menu.
  if (click == TF_LBI_CLK_LEFT && svc_)
    svc_->setPredictionEnabled(!svc_->predictionEnabled());
  return S_OK;
}

STDMETHODIMP CLangBarButton::InitMenu(ITfMenu *menu) {
  if (!menu)
    return E_INVALIDARG;
  addItem(menu, kMenuToggle, enabled() ? TF_LBMENUF_CHECKED : 0,
          L"Prédiction activée");
  addItem(menu, 0, TF_LBMENUF_SEPARATOR, nullptr);
  // Disposition clavier : affichée, et réparable quand elle ne correspond pas
  // à celle des Paramètres (cf KeyboardLayout.h).
  layout::Status s = layout::current();
  if (!s.active.empty()) {
    std::wstring line = (s.matches ? L"Disposition : " : L"⚠ Disposition : ") +
                        layout::displayName(s.active);
    addItem(menu, 0, TF_LBMENUF_GRAYED, line.c_str());
  }
  if (layoutFixed_) {
    addItem(menu, 0, TF_LBMENUF_GRAYED,
            L"Disposition corrigée — effet à la prochaine ouverture de session");
  } else if (!s.matches && !s.wanted.empty()) {
    std::wstring fix =
        L"Utiliser ma disposition (" + layout::displayName(s.wanted) + L")";
    addItem(menu, kMenuFixLayout, 0, fix.c_str());
  }
  addItem(menu, 0, TF_LBMENUF_SEPARATOR, nullptr);
  addItem(menu, kMenuSettings, 0, L"Réglages…");
  addItem(menu, kMenuHelp, 0, L"Aide en ligne");
  return S_OK;
}

STDMETHODIMP CLangBarButton::OnMenuSelect(UINT id) {
  if (!svc_)
    return S_OK;
  switch (id) {
  case kMenuToggle:
    svc_->setPredictionEnabled(!svc_->predictionEnabled());
    break;
  case kMenuSettings:
    svc_->openSettings();
    break;
  case kMenuFixLayout:
    layoutFixed_ = layout::repair() > 0;
    break;
  case kMenuHelp:
    ::ShellExecuteW(nullptr, L"open", kHelpUrl, nullptr, nullptr,
                    SW_SHOWNORMAL);
    break;
  }
  return S_OK;
}

STDMETHODIMP CLangBarButton::GetIcon(HICON *icon) {
  if (!icon)
    return E_INVALIDARG;
  // Taille d'une petite icône au DPI du SYSTÈME (celui de la barre des
  // tâches), pas à celui — peut-être virtualisé — de l'application hôte.
  int size = ::GetSystemMetricsForDpi(SM_CXSMICON, ::GetDpiForSystem());
  *icon = makeStatusIcon(size, enabled());
  return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP CLangBarButton::GetText(BSTR *text) {
  if (!text)
    return E_INVALIDARG;
  *text = ::SysAllocString(PREDICT_SERVICE_NAME);
  return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CLangBarButton::AdviseSink(REFIID riid, IUnknown *sink,
                                        DWORD *cookie) {
  if (!sink || !cookie)
    return E_INVALIDARG;
  if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
    return CONNECT_E_CANNOTCONNECT;
  if (sink_)
    return CONNECT_E_ADVISELIMIT;
  if (FAILED(sink->QueryInterface(IID_ITfLangBarItemSink,
                                  reinterpret_cast<void **>(&sink_))))
    return E_NOINTERFACE;
  *cookie = kSinkCookie;
  return S_OK;
}

STDMETHODIMP CLangBarButton::UnadviseSink(DWORD cookie) {
  if (cookie != kSinkCookie || !sink_)
    return CONNECT_E_NOCONNECTION;
  sink_->Release();
  sink_ = nullptr;
  return S_OK;
}

} // namespace win
