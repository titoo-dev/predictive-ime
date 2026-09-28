// Attributs d'affichage du préedit.
//
// Sans provider enregistré, une composition TSF est rendue par l'application
// comme du texte ORDINAIRE : le mot en cours et le fantôme seraient
// indiscernables de ce qui est déjà validé. Ces deux attributs donnent le
// soulignement (ce que je tape) et le gris clair (ce qui est proposé) —
// l'équivalent des TextFormatFlag::Underline / Italic de fcitx5.
#pragma once

#include "Guids.h"

#include <msctf.h>
#include <new>
#include <windows.h>

namespace win {

class CDisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
  CDisplayAttributeInfo(REFGUID guid, const wchar_t *desc,
                        const TF_DISPLAYATTRIBUTE &attr)
      : guid_(guid), desc_(desc), attr_(attr), orig_(attr) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfDisplayAttributeInfo))
      *ppv = static_cast<ITfDisplayAttributeInfo *>(this);
    if (!*ppv)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return ULONG(InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    LONG c = InterlockedDecrement(&ref_);
    if (c == 0)
      delete this;
    return ULONG(c);
  }

  STDMETHODIMP GetGUID(GUID *g) override {
    if (!g)
      return E_INVALIDARG;
    *g = guid_;
    return S_OK;
  }
  STDMETHODIMP GetDescription(BSTR *d) override {
    if (!d)
      return E_INVALIDARG;
    *d = SysAllocString(desc_);
    return *d ? S_OK : E_OUTOFMEMORY;
  }
  STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE *a) override {
    if (!a)
      return E_INVALIDARG;
    *a = attr_;
    return S_OK;
  }
  STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE *a) override {
    if (!a)
      return E_INVALIDARG;
    attr_ = *a;
    return S_OK;
  }
  STDMETHODIMP Reset() override { return SetAttributeInfo(&orig_); }

private:
  ~CDisplayAttributeInfo() = default;
  LONG ref_ = 1;
  GUID guid_;
  const wchar_t *desc_;
  TF_DISPLAYATTRIBUTE attr_, orig_;
};

// Le mot RÉELLEMENT tapé : souligné, couleurs du texte normal.
inline TF_DISPLAYATTRIBUTE inputAttr() {
  TF_DISPLAYATTRIBUTE a{};
  a.crText.type = TF_CT_NONE;
  a.crBk.type = TF_CT_NONE;
  a.lsStyle = TF_LS_SOLID;
  a.fBoldLine = FALSE;
  a.crLine.type = TF_CT_NONE;
  a.bAttr = TF_ATTR_INPUT;
  return a;
}

// La complétion PROPOSÉE : grisée, sans soulignement — le rendu des
// suggestions en ligne modernes (éditeurs, messageries). Elle ne se confond
// pas avec la frappe, qui reste soulignée ; le gris moyen reste lisible sur
// fond clair comme sombre.
inline TF_DISPLAYATTRIBUTE ghostAttr() {
  TF_DISPLAYATTRIBUTE a{};
  a.crText.type = TF_CT_COLORREF;
  a.crText.cr = RGB(138, 138, 138);
  a.crBk.type = TF_CT_NONE;
  a.lsStyle = TF_LS_NONE;
  a.fBoldLine = FALSE;
  a.crLine.type = TF_CT_NONE;
  a.bAttr = TF_ATTR_CONVERTED;
  return a;
}

class CDisplayAttributeInfoEnum : public IEnumTfDisplayAttributeInfo {
public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo))
      *ppv = static_cast<IEnumTfDisplayAttributeInfo *>(this);
    if (!*ppv)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return ULONG(InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    LONG c = InterlockedDecrement(&ref_);
    if (c == 0)
      delete this;
    return ULONG(c);
  }
  STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo **out) override {
    if (!out)
      return E_INVALIDARG;
    auto *e = new (std::nothrow) CDisplayAttributeInfoEnum();
    if (!e)
      return E_OUTOFMEMORY;
    e->idx_ = idx_;
    *out = e;
    return S_OK;
  }
  STDMETHODIMP Next(ULONG count, ITfDisplayAttributeInfo **info,
                    ULONG *fetched) override {
    ULONG n = 0;
    while (n < count && idx_ < 2) {
      info[n] = idx_ == 0
                    ? new (std::nothrow) CDisplayAttributeInfo(
                          GUID_PredictDisplayAttributeInput, L"Predict Input",
                          inputAttr())
                    : new (std::nothrow) CDisplayAttributeInfo(
                          GUID_PredictDisplayAttributeGhost, L"Predict Ghost",
                          ghostAttr());
      if (!info[n])
        return E_OUTOFMEMORY;
      n++;
      idx_++;
    }
    if (fetched)
      *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  STDMETHODIMP Reset() override {
    idx_ = 0;
    return S_OK;
  }
  STDMETHODIMP Skip(ULONG count) override {
    idx_ += int(count);
    if (idx_ > 2)
      idx_ = 2;
    return S_OK;
  }

private:
  ~CDisplayAttributeInfoEnum() = default;
  LONG ref_ = 1;
  int idx_ = 0;
};

} // namespace win
