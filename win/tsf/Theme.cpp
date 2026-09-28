#include "Theme.h"

namespace win {
namespace {

D2D1_COLOR_F rgb(BYTE r, BYTE g, BYTE b, float a = 1.f) {
  return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

D2D1_COLOR_F fromColorRef(COLORREF c) {
  return rgb(GetRValue(c), GetGValue(c), GetBValue(c));
}

// IME_PANEL_THEME=light|dark force le thème — pour capturer les deux rendus
// (candidate-preview) sans toucher au réglage de l'utilisateur.
int forcedTheme() {
  wchar_t v[16]{};
  if (!::GetEnvironmentVariableW(L"IME_PANEL_THEME", v, 16))
    return -1;
  if (_wcsicmp(v, L"dark") == 0)
    return 1;
  if (_wcsicmp(v, L"light") == 0)
    return 0;
  return -1;
}

bool appsUseDarkTheme() {
  if (int f = forcedTheme(); f >= 0)
    return f == 1;
  DWORD val = 1, sz = sizeof(val);
  if (::RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                     L"Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &val,
                     &sz) != ERROR_SUCCESS)
    return false;
  return val == 0;
}

// AccentPalette : 8 couleurs RGBA, de la plus claire à la plus sombre —
// Light3, Light2, Light1, Base, Dark1, Dark2, Dark3, (inutilisée). C'est la
// source dont WinUI tire ses SystemAccentColor* ; lire la seule couleur de
// base donnerait un accent illisible en thème sombre.
struct AccentPalette {
  D2D1_COLOR_F shade[7];
};

AccentPalette readAccentPalette() {
  // Défaut : le bleu de Windows 11, si l'utilisateur n'a jamais touché à
  // l'accent (la valeur n'existe alors pas toujours).
  static const BYTE kDefault[7][3] = {
      {0x99, 0xEB, 0xFF}, {0x4C, 0xC2, 0xFF}, {0x00, 0x91, 0xF8},
      {0x00, 0x78, 0xD4}, {0x00, 0x67, 0xC0}, {0x00, 0x3E, 0x92},
      {0x00, 0x1A, 0x68}};
  BYTE raw[32]{};
  DWORD sz = sizeof(raw);
  bool ok = ::RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                L"AccentPalette", RRF_RT_REG_BINARY, nullptr, raw,
                &sz) == ERROR_SUCCESS &&
            sz >= 28;
  AccentPalette p{};
  for (int i = 0; i < 7; i++)
    p.shade[i] = ok ? rgb(raw[4 * i], raw[4 * i + 1], raw[4 * i + 2])
                    : rgb(kDefault[i][0], kDefault[i][1], kDefault[i][2]);
  return p;
}

} // namespace

bool systemAnimationsEnabled() {
  BOOL on = TRUE;
  if (!::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0))
    return true;
  return on != FALSE;
}

Theme loadSystemTheme() {
  Theme t;
  HIGHCONTRASTW hc{sizeof(hc)};
  if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
      (hc.dwFlags & HCF_HIGHCONTRASTON)) {
    // Contraste élevé : AUCUNE couleur maison, aucune transparence — seules
    // les couleurs choisies par l'utilisateur garantissent la lisibilité.
    t.highContrast = true;
    t.surface = fromColorRef(::GetSysColor(COLOR_WINDOW));
    t.text = fromColorRef(::GetSysColor(COLOR_WINDOWTEXT));
    t.textDim = fromColorRef(::GetSysColor(COLOR_GRAYTEXT));
    t.accent = fromColorRef(::GetSysColor(COLOR_HIGHLIGHT));
    t.onAccent = fromColorRef(::GetSysColor(COLOR_HIGHLIGHTTEXT));
    t.accentText = fromColorRef(::GetSysColor(COLOR_HOTLIGHT));
    t.subtle = t.accent;
    t.outline = t.text;
    t.border = ::GetSysColor(COLOR_WINDOWTEXT);
    return t;
  }

  // Jetons Fluent (menus / flyouts de Windows 11). L'accent suit la règle
  // WinUI : Dark1 sur fond clair, Light2 sur fond sombre — la couleur de
  // base seule manque de contraste dans l'un des deux thèmes.
  AccentPalette p = readAccentPalette();
  t.dark = appsUseDarkTheme();
  if (t.dark) {
    t.surface = rgb(0x2C, 0x2C, 0x2C);
    t.text = rgb(0xFF, 0xFF, 0xFF);
    t.textDim = rgb(0xFF, 0xFF, 0xFF, 0.62f);
    t.accent = p.shade[1];
    t.onAccent = rgb(0x00, 0x00, 0x00);
    t.accentText = p.shade[0];
    t.subtle = rgb(0xFF, 0xFF, 0xFF, 0.075f);
    t.outline = rgb(0xFF, 0xFF, 0xFF, 0.10f);
    t.border = RGB(0x45, 0x45, 0x45);
  } else {
    t.surface = rgb(0xF9, 0xF9, 0xF9);
    t.text = rgb(0x1A, 0x1A, 0x1A);
    t.textDim = rgb(0x00, 0x00, 0x00, 0.58f);
    t.accent = p.shade[4];
    t.onAccent = rgb(0xFF, 0xFF, 0xFF);
    t.accentText = p.shade[5];
    t.subtle = rgb(0x00, 0x00, 0x00, 0.055f);
    t.outline = rgb(0x00, 0x00, 0x00, 0.09f);
    t.border = RGB(0xE0, 0xE0, 0xE0);
  }
  return t;
}

} // namespace win
