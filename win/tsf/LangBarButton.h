// Indicateur de mode dans la barre des tâches (à côté de « FRA »).
//
// C'est l'élément GUID_LBI_INPUTMODE que Windows 8+ place dans la zone de
// notification pour l'IME active — là où l'IME japonaise affiche « あ/A ».
// Clic gauche : activer/couper la prédiction. Clic droit : menu dessiné par
// le système (InitMenu), donc natif et sans vol de focus.
//
// L'icône est un glyphe monochrome rendu à la volée, cf StatusIcon.h.
#pragma once

#include <msctf.h>
#include <windows.h>

namespace win {

class CTextService;

class CLangBarButton : public ITfLangBarItemButton, public ITfSource {
public:
  explicit CLangBarButton(CTextService *svc);

  // Le service est désactivé : plus aucun rappel vers lui.
  void detach() { svc_ = nullptr; }
  // L'état a changé (ici ou dans une autre application) : icône, texte et
  // info-bulle sont redemandés.
  void refresh();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfLangBarItem
  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO *info) override;
  STDMETHODIMP GetStatus(DWORD *status) override;
  STDMETHODIMP Show(BOOL show) override;
  STDMETHODIMP GetTooltipString(BSTR *tip) override;

  // ITfLangBarItemButton
  STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT *area) override;
  STDMETHODIMP InitMenu(ITfMenu *menu) override;
  STDMETHODIMP OnMenuSelect(UINT id) override;
  STDMETHODIMP GetIcon(HICON *icon) override;
  STDMETHODIMP GetText(BSTR *text) override;

  // ITfSource — la barre de langue s'y abonne pour savoir quand redessiner.
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown *sink, DWORD *cookie) override;
  STDMETHODIMP UnadviseSink(DWORD cookie) override;

private:
  ~CLangBarButton();
  bool enabled() const;

  LONG ref_ = 1;
  CTextService *svc_;
  ITfLangBarItemSink *sink_ = nullptr;};

} // namespace win
