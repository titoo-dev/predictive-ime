#include "StatusIcon.h"

#include <algorithm>
#include <d2d1.h>
#include <vector>
#include <wrl/client.h>

namespace win {
namespace {

// Couleur de la barre des tâches (≠ thème des applications) : c'est sur elle
// que l'icône est posée.
bool taskbarIsLight() {
  DWORD val = 0, sz = sizeof(val);
  if (::RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                     L"Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &val,
                     &sz) != ERROR_SUCCESS)
    return false;
  return val != 0;
}

} // namespace

HICON makeStatusIcon(int size, bool enabled) {
  if (size <= 0)
    size = 16;
  // Couverture dessinée en blanc sur transparent par Direct2D, puis recolorée :
  // l'alpha porte l'anticrénelage, la couleur est uniforme — pas de frange.
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HDC mem = ::CreateCompatibleDC(nullptr);
  HBITMAP color =
      ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!mem || !color) {
    if (mem)
      ::DeleteDC(mem);
    return nullptr;
  }
  HGDIOBJ old = ::SelectObject(mem, color);

  Microsoft::WRL::ComPtr<ID2D1Factory> f;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  RECT rc{0, 0, size, size};
  if (SUCCEEDED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    f.GetAddressOf())) &&
      SUCCEEDED(f->CreateDCRenderTarget(&props, rt.GetAddressOf())) &&
      SUCCEEDED(rt->BindDC(mem, &rc))) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.GetAddressOf());
    if (br) {
      // Même motif que l'icône de l'application (scripts/make-icon.ps1) :
      // texte tapé, curseur, suggestion fantôme.
      const float s = float(size), cy = s / 2, barH = s * 0.16f;
      const float on = enabled ? 1.f : 0.45f;
      br->SetOpacity(on);
      rt->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(s * 0.06f, cy - barH / 2, s * 0.46f,
                                        cy + barH / 2),
                            barH / 2, barH / 2),
          br.Get());
      const float cw = (std::max)(1.5f, s * 0.09f);
      rt->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(s * 0.58f - cw / 2, s * 0.16f,
                                        s * 0.58f + cw / 2, s * 0.84f),
                            cw / 2, cw / 2),
          br.Get());
      if (enabled) {
        br->SetOpacity(0.5f);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(s * 0.70f, cy - barH / 2, s * 0.94f,
                                          cy + barH / 2),
                              barH / 2, barH / 2),
            br.Get());
      }
    }
    rt->EndDraw();
  }
  ::SelectObject(mem, old);
  ::DeleteDC(mem);

  // Recoloration : alpha « droit », RVB uniforme (blanc ou noir).
  const BYTE ink = taskbarIsLight() ? 0x00 : 0xFF;
  auto *px = static_cast<BYTE *>(bits);
  for (int i = 0; i < size * size; i++) {
    px[4 * i + 0] = px[4 * i + 1] = px[4 * i + 2] = ink;
  }
  // Masque ET à zéro : c'est l'alpha de la couleur qui découpe l'icône.
  std::vector<BYTE> zeros(size_t((size + 15) / 16 * 2) * size, 0);
  HBITMAP mask = ::CreateBitmap(size, size, 1, 1, zeros.data());
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = ::CreateIconIndirect(&ii);
  ::DeleteObject(mask);
  ::DeleteObject(color);
  return icon;
}

} // namespace win
