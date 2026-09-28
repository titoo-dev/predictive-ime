// Aperçu de la barre de candidats HORS de toute application hôte.
//
// La barre ne s'observe sinon qu'en installant le text service et en tapant
// dans une vraie application — impossible à automatiser, pénible à comparer.
// Cet outil affiche chaque disposition (chips, auto-appliqué, navigation,
// grille emoji, reformulation, chargement) et en capture l'écran, ombre et
// coins DWM compris.
//
//   candidate-preview [dossier-de-sortie]      (défaut : répertoire courant)
//   set IME_PANEL_THEME=dark  → rendu sombre sans changer le thème Windows
#include "../tsf/CandidateWindow.h"
#include "../tsf/StatusIcon.h"

#include <cstdio>
#include <string>
#include <vector>
#include <wincodec.h>
#include <windows.h>

namespace {

void pump(int ms) {
  ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG m;
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&m);
      ::DispatchMessageW(&m);
    }
    ::Sleep(5);
  }
}

// Capture d'une zone de l'écran composé (DWM) en PNG via WIC.
bool capture(const RECT &r, const std::wstring &path) {
  const int w = r.right - r.left, h = r.bottom - r.top;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = ::SelectObject(mem, bmp);
  ::BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
  ::SelectObject(mem, old);

  bool ok = false;
  IWICImagingFactory *wic = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wic)))) {
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr))) {
      // BitBlt laisse l'alpha à 0 : on l'opacifie, et on exige un BGRA que
      // l'encodeur PNG accepte tel quel (en BGR il passe en 24 bits et nos
      // lignes de 32 bits seraient lues de travers).
      auto *px = static_cast<BYTE *>(bits);
      for (int i = 0; i < w * h; i++)
        px[4 * i + 3] = 0xFF;
      WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
      frame->SetSize(UINT(w), UINT(h));
      frame->SetPixelFormat(&fmt);
      ok = IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(UINT(h), UINT(w * 4), UINT(w * h * 4),
                                        static_cast<BYTE *>(bits))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (frame)
      frame->Release();
    if (enc)
      enc->Release();
    if (stream)
      stream->Release();
    wic->Release();
  }
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return ok;
}

struct Scenario {
  const wchar_t *name;
  std::vector<core::Candidate> cands;
  int cursor;
  std::string aux;
  win::PanelHints hints;
};

} // namespace

int wmain(int argc, wchar_t **argv) {
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::wstring out = argc > 1 ? argv[1] : L".";

  const std::vector<Scenario> scenarios = {
      {L"chips",
       {{"bonjour", false, ""}, {"bonsoir", false, ""}, {"bonheur", false, ""},
        {"bon", false, ""}},
       -1, "", {true, false}},
      {L"chips-auto",
       {{"aujourd'hui", true, ""}, {"auparavant", false, ""},
        {"aussi", false, ""}, {"autre", false, ""}},
       -1, "", {true, false}},
      {L"chips-nav",
       {{"merci", false, ""}, {"beaucoup", false, ""}, {"pour", false, ""},
        {"votre", false, ""}, {"aide", false, ""}},
       2, "", {true, false}},
      {L"next-word",
       {{"de", false, ""}, {"que", false, ""}, {"le", false, ""},
        {"😊", false, ""}},
       -1, "", {false, false}},
      {L"emoji-grid",
       {{"😀", false, ""}, {"😃", false, ""}, {"😄", false, ""},
        {"😁", false, ""}, {"😆", false, ""}, {"😅", false, ""},
        {"🤣", false, ""}, {"😂", false, ""}, {"🙂", false, ""},
        {"🙃", false, ""}, {"😉", false, ""}, {"😊", false, ""}},
       3, "", {true, true}},
      {L"lang-menu",
       {{"Auto", false, ""}, {"Français", false, ""}, {"English", false, ""}},
       1, "Langue", {false, false}},
      {L"reform",
       {{"Pourriez-vous m'envoyer le rapport avant vendredi ?", false, "1"},
        {"Merci de me transmettre le rapport d'ici vendredi, c'est assez "
         "urgent pour la réunion de lundi.",
         false, "2"},
        {"Tu peux m'envoyer le rapport avant vendredi ?", false, "3"}},
       1, "Formel  ⚡ Groq   · ←→/rfsct mode", {false, false}},
      {L"reform-loading",
       {{"\xE2\x9F\xB3 Reformulation…", false, ""}},
       -1, "", {false, false}},
  };

  win::CandidateWindow bar;
  const RECT caret{400, 300, 402, 320};
  int failures = 0;
  for (const auto &s : scenarios) {
    bar.show(s.cands, s.cursor, s.aux, caret, s.hints);
    pump(350); // animations d'apparition terminées
    // Fenêtre de la barre = la plus haute fenêtre de notre classe ; on capture
    // avec une marge pour l'ombre.
    HWND h = ::FindWindowW(L"PredictImeCandidateBar", nullptr);
    RECT r{};
    ::GetWindowRect(h, &r);
    const int m = 18;
    RECT cap{r.left - m, r.top - m, r.right + m, r.bottom + m};
    std::wstring path = out + L"\\" + s.name + L".png";
    bool ok = capture(cap, path);
    std::wprintf(L"%-16s %ldx%ld  %s\n", s.name, r.right - r.left,
                 r.bottom - r.top, ok ? path.c_str() : L"ÉCHEC capture");
    failures += ok ? 0 : 1;
    bar.hide();
    pump(60);
  }
  // Glyphe de l'indicateur (activé / coupé), agrandi ×4 sur un fond couleur de
  // barre des tâches, dessiné par DrawIconEx comme l'Explorateur le ferait.
  {
    const int ico = 16, zoom = 4, pad = 8;
    HWND strip = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP | WS_VISIBLE, 200, 200, 2 * (ico * zoom + pad) + pad,
        ico * zoom + 2 * pad, nullptr, nullptr, nullptr, nullptr);
    pump(100);
    HDC dc = ::GetDC(strip);
    DWORD light = 0, sz = sizeof(light);
    ::RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                   L"Personalize",
                   L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light,
                   &sz);
    HBRUSH bg = ::CreateSolidBrush(light ? RGB(238, 238, 238) : RGB(32, 32, 32));
    RECT all{0, 0, 2 * (ico * zoom + pad) + pad, ico * zoom + 2 * pad};
    ::FillRect(dc, &all, bg);
    ::DeleteObject(bg);
    for (int i = 0; i < 2; i++) {
      HICON h = win::makeStatusIcon(ico * zoom, i == 0);
      ::DrawIconEx(dc, pad + i * (ico * zoom + pad), pad, h, ico * zoom,
                   ico * zoom, 0, nullptr, DI_NORMAL);
      ::DestroyIcon(h);
    }
    ::ReleaseDC(strip, dc);
    RECT r{};
    ::GetWindowRect(strip, &r);
    std::wstring path = out + L"\\tray-icon.png";
    bool ok = capture(r, path);
    std::wprintf(L"%-16s %s\n", L"tray-icon", ok ? path.c_str() : L"ÉCHEC");
    failures += ok ? 0 : 1;
    ::DestroyWindow(strip);
  }

  ::CoUninitialize();
  return failures ? 1 : 0;
}
