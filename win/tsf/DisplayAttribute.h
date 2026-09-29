// Attribut d'affichage du préedit.
//
// Sans provider enregistré, une composition TSF est rendue par l'application
// comme du texte ORDINAIRE : le mot en cours serait indiscernable de ce qui
// est déjà validé. Cet attribut lui donne le soulignement — l'équivalent du
// TextFormatFlag::Underline de fcitx5.
#pragma once

#include "Guids.h"
#include "Module.h"

#include <msctf.h>
#include <new>
#include <windows.h>

namespace win {

class CDisplayAttributeInfo : public ITfDisplayAttributeInfo {
public:
  CDisplayAttributeInfo(REFGUID guid, const wchar_t *desc,
                        const TF_DISPLAYATTRIBUTE &attr)
      : guid_(guid), desc_(desc), attr_(attr), orig_(attr) {
    dllAddRef();
  }

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
  ~CDisplayAttributeInfo() { dllRelease(); }
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

class CDisplayAttributeInfoEnum : public IEnumTfDisplayAttributeInfo {
public:
  CDisplayAttributeInfoEnum() { dllAddRef(); }

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
    while (n < count && idx_ < 1) {
      info[n] = new (std::nothrow) CDisplayAttributeInfo(
          GUID_PredictDisplayAttributeInput, L"Predict Input", inputAttr());
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
    if (idx_ > 1)
      idx_ = 1;
    return S_OK;
  }

private:
  ~CDisplayAttributeInfoEnum() { dllRelease(); }
  LONG ref_ = 1;
  int idx_ = 0;
};

} // namespace win
