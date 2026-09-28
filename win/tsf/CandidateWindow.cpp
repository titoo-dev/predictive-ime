#include "CandidateWindow.h"

#include "Debug.h"
#include "Dpi.h"

#include "../../core/text.h"

#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <shellscalingapi.h>

namespace win {
namespace {

constexpr wchar_t kClassName[] = L"PredictImeCandidateBar";
constexpr UINT_PTR kAnimTimer = 1;

// Attributs DWM de Windows 11, en valeurs brutes : un SDK plus ancien ne les
// nomme pas, et Windows 10 les refuse proprement (E_INVALIDARG) — c'est ce
// refus qui nous dit de dessiner la bordure nous-mêmes.
constexpr DWORD kDwmCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBorderColor = 34;      // DWMWA_BORDER_COLOR
constexpr int kDwmCornerRound = 2;         // DWMWCP_ROUND (8 px, menus)

// Géométrie en DIP (1/96 de pouce) — la cible Direct2D applique le DPI.
constexpr float kCardPad = 4;
constexpr float kChipsX0 = 16; // laisse la place à l'indicateur de mode
constexpr float kChipH = 30, kChipPadX = 11, kChipGap = 2, kChipMinW = 34,
                kChipMaxText = 200, kChipRadius = 5;
constexpr int kGridCols = 8;
constexpr float kGridPad = 6, kCellW = 38, kCellH = 36;
constexpr float kListW = 420, kHeaderH = 34, kRowMinH = 38, kRowGap = 2,
                kRowTextX = 48, kRowPadR = 16, kBadgeD = 20;

// Durées calquées sur qmlpanel : la barre suit la frappe, elle ne la
// ralentit jamais.
constexpr int kGlideMs = 110, kAppearMs = 120, kRefreshMs = 80,
              kSpinPeriodMs = 900;

std::wstring wide(const std::string &utf8) {
  auto u16 = core::toUtf16(utf8);
  return std::wstring(reinterpret_cast<const wchar_t *>(u16.data()),
                      u16.size());
}

// Même heuristique que qmlpanel (looksEmoji) : VS16, ZWJ et keycap
// n'apparaissent jamais dans un mot ordinaire.
bool looksEmoji(const std::wstring &s) {
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    if (cp >= 0x1F000 || cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 ||
        (cp >= 0x2190 && cp <= 0x2BFF))
      return true;
  }
  return false;
}

// Placeholder de génération posé par le cœur : un candidat unique « ⟳ … ».
bool isLoadingPlaceholder(const std::vector<core::Candidate> &c) {
  return c.size() == 1 && c[0].text.rfind("\xE2\x9F\xB3", 0) == 0;
}

// Segoe UI Variable est la police de l'interface de Windows 11 ; Windows 10
// ne l'a pas et retombe sur Segoe UI.
const wchar_t *uiFontFamily(IDWriteFactory *f) {
  Microsoft::WRL::ComPtr<IDWriteFontCollection> sys;
  if (SUCCEEDED(f->GetSystemFontCollection(&sys, FALSE)) && sys) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(sys->FindFamilyName(L"Segoe UI Variable Text", &idx,
                                      &exists)) &&
        exists)
      return L"Segoe UI Variable Text";
  }
  return L"Segoe UI";
}

int monitorDpi(HMONITOR mon) {
  UINT dx = 96, dy = 96;
  if (mon && SUCCEEDED(::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)))
    return int(dx);
  return 96;
}

D2D1_COLOR_F fromBorder(COLORREF c) {
  return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f,
                      GetBValue(c) / 255.f);
}

} // namespace

// ------------------------------------------------------------------ Anim ----

float CandidateWindow::Anim::at(ULONGLONG now) const {
  if (ms <= 0 || now >= start + ULONGLONG(ms))
    return to;
  float t = float(now - start) / float(ms);
  float u = 1.f - t;
  return from + (to - from) * (1.f - u * u * u); // ease-out cubique
}

// -------------------------------------------------------------- fenêtre ----

CandidateWindow::~CandidateWindow() {
  if (hwnd_) {
    ::KillTimer(hwnd_, kAnimTimer);
    ::DestroyWindow(hwnd_);
  }
}

LRESULT CALLBACK CandidateWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp) {
  auto *self = reinterpret_cast<CandidateWindow *>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    break;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (self)
      self->paint(dc);
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_TIMER:
    if (wp == kAnimTimer)
      ::InvalidateRect(hwnd, nullptr, FALSE);
    return 0;
  // Thème clair/sombre, accent, contraste élevé, effets d'animation : tout
  // arrive par ces diffusions — la barre suit le système sans redémarrage.
  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
  case WM_SYSCOLORCHANGE:
  case WM_DWMCOLORIZATIONCOLORCHANGED:
    if (self) {
      self->themeDirty_ = true;
      if (self->visible()) {
        self->refreshTheme();
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
    }
    break;
  // On se positionne nous-mêmes à chaque affichage, au DPI de l'écran visé :
  // le rectangle suggéré par Windows ne doit pas redimensionner la barre.
  case WM_DPICHANGED:
    return 0;
  // La barre ne doit JAMAIS prendre le focus : l'application perdrait son
  // curseur et la frappe s'arrêterait net.
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_ERASEBKGND:
    return 1;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool CandidateWindow::ensureCreated() {
  if (hwnd_)
    return true;
  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return false;
    registered = true;
  }
  // Per-Monitor V2 quel que soit l'hôte : c'est ce qui garde le texte net
  // dans les applications qui ne gèrent pas le DPI (cf Dpi.h).
  auto scope = dpi::perMonitor();
  hwnd_ = ::CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClassName, L"",
      WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, this);
  if (!hwnd_)
    return false;
  themeDirty_ = true;
  return true;
}

void CandidateWindow::applyWindowChrome() {
  // Windows 11 : coins arrondis, ombre et bordure fine rendus par DWM — la
  // barre a exactement l'allure d'un menu du système.
  int pref = kDwmCornerRound;
  dwmRounded_ = SUCCEEDED(::DwmSetWindowAttribute(
      hwnd_, kDwmCornerPreference, &pref, sizeof(pref)));
  if (dwmRounded_) {
    COLORREF c = theme_.border;
    ::DwmSetWindowAttribute(hwnd_, kDwmBorderColor, &c, sizeof(c));
  }
}

void CandidateWindow::refreshTheme() {
  if (!themeDirty_ || !hwnd_)
    return;
  theme_ = loadSystemTheme();
  animations_ = systemAnimationsEnabled();
  applyWindowChrome();
  themeDirty_ = false;
}

bool CandidateWindow::ensureGraphics() {
  if (!d2d_ &&
      FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 d2d_.GetAddressOf()))) {
    dbg("bar: D2D1CreateFactory a échoué");
    return false;
  }
  if (!dwrite_) {
    if (FAILED(::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf())))) {
      dbg("bar: DWriteCreateFactory a échoué");
      return false;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"fr-FR";
    ::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    const wchar_t *family = uiFontFamily(dwrite_.Get());
    auto fmt = [&](float size, DWRITE_FONT_WEIGHT w,
                   ComPtr<IDWriteTextFormat> &out) {
      return SUCCEEDED(dwrite_->CreateTextFormat(
          family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, size, locale, out.GetAddressOf()));
    };
    if (!fmt(14.f, DWRITE_FONT_WEIGHT_NORMAL, body_) ||
        !fmt(20.f, DWRITE_FONT_WEIGHT_NORMAL, emoji_) ||
        !fmt(12.f, DWRITE_FONT_WEIGHT_NORMAL, caption_) ||
        !fmt(11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, badge_)) {
      dwrite_.Reset();
      return false;
    }
    emoji_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    badge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dwrite_->CreateEllipsisTrimmingSign(body_.Get(), ellipsis_.GetAddressOf());
  }
  if (!spinnerArc_) {
    // Arc de 270° de rayon 6,5 DIP centré sur l'origine ; on le fait tourner
    // par transformation plutôt que de reconstruire la géométrie à chaque image.
    constexpr float r = 6.5f;
    if (SUCCEEDED(d2d_->CreatePathGeometry(spinnerArc_.GetAddressOf()))) {
      ComPtr<ID2D1GeometrySink> sink;
      if (SUCCEEDED(spinnerArc_->Open(sink.GetAddressOf()))) {
        sink->BeginFigure(D2D1::Point2F(r, 0), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0, -r), D2D1::SizeF(r, r),
                                      0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
      }
    }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
    d2d_->CreateStrokeStyle(sp, nullptr, 0, roundCap_.GetAddressOf());
  }
  if (!rt_) {
    // Rendu LOGICIEL, délibérément : la DLL vit dans chaque application (jeux,
    // bureau sécurisé…) et une barre de quelques centaines de pixels ne
    // justifie pas d'y ouvrir un périphérique Direct3D.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(d2d_->CreateDCRenderTarget(&props, rt_.GetAddressOf()))) {
      dbg("bar: CreateDCRenderTarget a échoué");
      return false;
    }
    rt_->CreateLayer(nullptr, layer_.ReleaseAndGetAddressOf());
  }
  return true;
}

// ------------------------------------------------------------ disposition --

CandidateWindow::ComPtr<IDWriteTextLayout>
CandidateWindow::makeLayout(const std::wstring &s, IDWriteTextFormat *fmt,
                            float maxW, bool wrap) {
  ComPtr<IDWriteTextLayout> l;
  if (FAILED(dwrite_->CreateTextLayout(s.c_str(), UINT32(s.size()), fmt, maxW,
                                       10000.f, l.GetAddressOf())))
    return nullptr;
  l->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP
                          : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (!wrap && ellipsis_) {
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    l->SetTrimming(&trim, ellipsis_.Get());
  }
  return l;
}

void CandidateWindow::buildLayout() {
  auto textSize = [](IDWriteTextLayout *l) {
    DWRITE_TEXT_METRICS m{};
    if (l)
      l->GetMetrics(&m);
    return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
  };
  auxLayout_.Reset();
  placeholder_.Reset();
  auxWidth_ = 0;

  if (mode_ == Mode::Chips) {
    float x = kChipsX0;
    if (!aux_.empty()) {
      // Étiquette du menu (« Langue ») posée avant les chips.
      auxLayout_ = makeLayout(aux_, caption_.Get(), kChipMaxText, false);
      auxWidth_ = textSize(auxLayout_.Get()).width;
      x += auxWidth_ + 10;
    }
    for (auto &it : items_) {
      it.layout = makeLayout(it.text, body_.Get(), kChipMaxText, false);
      if (!it.layout)
        continue;
      DWRITE_TEXT_RANGE all{0, UINT32(it.text.size())};
      if (it.emoji)
        it.layout->SetFontSize(17.f, all);
      if (it.autoApply)
        it.layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
      float w = (std::min)(textSize(it.layout.Get()).width, kChipMaxText) +
                2 * kChipPadX;
      w = (std::max)(w, kChipMinW);
      it.box = D2D1::RectF(x, kCardPad, x + w, kCardPad + kChipH);
      x += w + kChipGap;
    }
    size_ = D2D1::SizeF(x - kChipGap + kCardPad, kChipH + 2 * kCardPad);
    return;
  }

  if (mode_ == Mode::Grid) {
    int n = int(items_.size());
    int cols = (std::min)(kGridCols, (std::max)(1, n));
    int rows = (n + kGridCols - 1) / kGridCols;
    for (int i = 0; i < n; i++) {
      auto &it = items_[i];
      float x = kGridPad + (i % kGridCols) * kCellW;
      float y = kGridPad + (i / kGridCols) * kCellH;
      it.box = D2D1::RectF(x, y, x + kCellW, y + kCellH);
      it.layout = makeLayout(it.text, emoji_.Get(), kCellW, false);
    }
    size_ = D2D1::SizeF(2 * kGridPad + cols * kCellW,
                        2 * kGridPad + (std::max)(1, rows) * kCellH);
    return;
  }

  // Liste de reformulation : en-tête (mode + source), séparateur, variantes
  // numérotées sur toute la largeur, texte qui passe à la ligne.
  std::wstring header =
      loading_ ? L"Reformulation…"
               : (!aux_.empty() ? aux_
                                : L"Reformuler · 1–" +
                                      std::to_wstring(items_.size()));
  auxLayout_ = makeLayout(header, caption_.Get(), kListW - 26 - 40, false);
  float y = kHeaderH + 5;
  if (loading_) {
    placeholder_ = makeLayout(L"Génération des variantes…", body_.Get(),
                              kListW - 32, false);
    y += kRowMinH + kRowGap;
  } else {
    for (auto &it : items_) {
      it.layout =
          makeLayout(it.text, body_.Get(), kListW - kRowTextX - kRowPadR, true);
      it.badge = makeLayout(it.label, badge_.Get(), kBadgeD, false);
      float h = (std::max)(kRowMinH, textSize(it.layout.Get()).height + 16);
      it.box = D2D1::RectF(kCardPad, y, kListW - kCardPad, y + h);
      y += h + kRowGap;
    }
  }
  size_ = D2D1::SizeF(kListW, y - kRowGap + 6);
}

// ------------------------------------------------------------------ rendu --

D2D1_RECT_F CandidateWindow::highlightRect(ULONGLONG now) const {
  float x = hlX_.at(now), y = hlY_.at(now);
  return D2D1::RectF(x, y, x + hlW_.at(now), y + hlH_.at(now));
}

void CandidateWindow::armTimer() {
  ULONGLONG now = ::GetTickCount64();
  bool busy = loading_ || fade_.running(now) || hlX_.running(now) ||
              hlY_.running(now) || hlW_.running(now) || hlH_.running(now);
  if (busy && !timerOn_) {
    ::SetTimer(hwnd_, kAnimTimer, 15, nullptr); // ~60 i/s
    timerOn_ = true;
  } else if (!busy && timerOn_) {
    ::KillTimer(hwnd_, kAnimTimer);
    timerOn_ = false;
  }
}

void CandidateWindow::drawSpinner(ID2D1RenderTarget *rt,
                                  ID2D1SolidColorBrush *br, D2D1_POINT_2F c,
                                  ULONGLONG now) {
  if (!spinnerArc_)
    return;
  float turns = animations_ ? float((now - spinStart_) % kSpinPeriodMs) /
                                  float(kSpinPeriodMs)
                            : 0.f;
  br->SetColor(theme_.accent);
  rt->SetTransform(D2D1::Matrix3x2F::Rotation(turns * 360.f) *
                   D2D1::Matrix3x2F::Translation(c.x, c.y));
  rt->DrawGeometry(spinnerArc_.Get(), br, 1.8f, roundCap_.Get());
  rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void CandidateWindow::drawItems(ID2D1RenderTarget *rt,
                                ID2D1SolidColorBrush *br,
                                const D2D1_RECT_F &hl, bool hlOn) {
  const auto opts = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
  const float px = 96.f / dpi_; // un pixel physique, en DIP
  auto layoutH = [](IDWriteTextLayout *l) {
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
  };

  if (mode_ != Mode::List) {
    if (mode_ == Mode::Chips) {
      // Indicateur de mode (comme qmlpanel) : accent = mot en cours de
      // frappe, discret = barre passive (mot suivant).
      br->SetColor(composing_ ? theme_.accent
                              : withAlpha(theme_.textDim, 0.45f));
      float cy = size_.height / 2;
      rt->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(7, cy - 7, 10, cy + 7), 1.5f, 1.5f),
          br);
      if (auxLayout_) {
        br->SetColor(theme_.textDim);
        rt->DrawTextLayout(
            D2D1::Point2F(kChipsX0,
                          (size_.height - layoutH(auxLayout_.Get())) / 2),
            auxLayout_.Get(), br, opts);
      }
    }
    // La pastille accent : dessinée AVANT le texte, elle glisse d'un
    // candidat à l'autre pendant la navigation.
    if (hlOn) {
      br->SetColor(theme_.accent);
      float r = mode_ == Mode::Grid ? 6.f : kChipRadius;
      rt->FillRoundedRectangle(D2D1::RoundedRect(hl, r, r), br);
    }
    for (size_t i = 0; i < items_.size(); i++) {
      const auto &it = items_[i];
      if (!it.layout)
        continue;
      bool sel = int(i) == cursor_;
      // Liseré + voile accent : « l'Espace appliquera CE candidat ». Visible
      // tant qu'on ne navigue pas — ensuite la pastille dit tout.
      if (it.autoApply && cursor_ < 0) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(it.box, kChipRadius,
                                                 kChipRadius);
        br->SetColor(withAlpha(theme_.accent, theme_.highContrast ? 0 : 0.12f));
        rt->FillRoundedRectangle(rr, br);
        rr.rect = D2D1::RectF(it.box.left + px / 2, it.box.top + px / 2,
                              it.box.right - px / 2, it.box.bottom - px / 2);
        br->SetColor(withAlpha(theme_.accent, 0.6f));
        rt->DrawRoundedRectangle(rr, br, px);
      }
      br->SetColor(sel ? theme_.onAccent
                       : (it.autoApply ? theme_.accentText : theme_.text));
      float h = layoutH(it.layout.Get());
      float x = mode_ == Mode::Grid ? it.box.left : it.box.left + kChipPadX;
      float y = it.box.top + (it.box.bottom - it.box.top - h) / 2;
      rt->DrawTextLayout(D2D1::Point2F(x, y), it.layout.Get(), br, opts);
    }
    return;
  }

  // ---- liste --------------------------------------------------------------
  const float hy = kHeaderH / 2;
  br->SetColor(theme_.accent);
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(16, hy), 3, 3), br);
  if (auxLayout_) {
    br->SetColor(theme_.textDim);
    rt->DrawTextLayout(D2D1::Point2F(26, hy - layoutH(auxLayout_.Get()) / 2),
                       auxLayout_.Get(), br, opts);
  }
  if (loading_)
    drawSpinner(rt, br, D2D1::Point2F(size_.width - 20, hy),
                ::GetTickCount64());
  br->SetColor(theme_.outline);
  rt->FillRectangle(D2D1::RectF(0, kHeaderH, size_.width, kHeaderH + px), br);

  if (loading_ && placeholder_) {
    br->SetColor(theme_.textDim);
    float top = kHeaderH + 5;
    rt->DrawTextLayout(
        D2D1::Point2F(16, top + (kRowMinH - layoutH(placeholder_.Get())) / 2),
        placeholder_.Get(), br, opts);
    return;
  }

  // Sélection façon liste Fluent : voile discret + barre d'accent à gauche.
  // En contraste élevé, le voile devient la couleur de surlignage système.
  if (hlOn) {
    br->SetColor(theme_.subtle);
    rt->FillRoundedRectangle(D2D1::RoundedRect(hl, 5, 5), br);
    float cy = (hl.top + hl.bottom) / 2;
    br->SetColor(theme_.accent);
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(hl.left + 2, cy - 8, hl.left + 5, cy + 8),
                          1.5f, 1.5f),
        br);
  }
  for (size_t i = 0; i < items_.size(); i++) {
    const auto &it = items_[i];
    if (!it.layout)
      continue;
    bool sel = int(i) == cursor_;
    float cy = (it.box.top + it.box.bottom) / 2;
    const float bx = 26;
    // Pastille du numéro : pleine quand la ligne est sélectionnée.
    if (theme_.highContrast && !sel) {
      br->SetColor(theme_.text);
      rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(bx, cy), kBadgeD / 2 - px,
                                    kBadgeD / 2 - px),
                      br, px);
    } else {
      br->SetColor(sel ? theme_.accent : withAlpha(theme_.accent, 0.16f));
      rt->FillEllipse(
          D2D1::Ellipse(D2D1::Point2F(bx, cy), kBadgeD / 2, kBadgeD / 2), br);
    }
    if (it.badge) {
      br->SetColor(sel ? theme_.onAccent
                       : (theme_.highContrast ? theme_.text
                                              : theme_.accentText));
      rt->DrawTextLayout(
          D2D1::Point2F(bx - kBadgeD / 2, cy - layoutH(it.badge.Get()) / 2),
          it.badge.Get(), br, opts);
    }
    br->SetColor(sel && theme_.highContrast ? theme_.onAccent : theme_.text);
    rt->DrawTextLayout(
        D2D1::Point2F(kRowTextX, cy - layoutH(it.layout.Get()) / 2),
        it.layout.Get(), br, opts);
  }
}

void CandidateWindow::paint(HDC dc) {
  refreshTheme();
  if (!ensureGraphics())
    return;
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  if (FAILED(rt_->BindDC(dc, &rc)))
    return;
  rt_->SetDpi(dpi_, dpi_);

  const ULONGLONG now = ::GetTickCount64();
  rt_->BeginDraw();
  rt_->SetTransform(D2D1::Matrix3x2F::Identity());
  rt_->Clear(theme_.surface);

  ComPtr<ID2D1SolidColorBrush> br;
  rt_->CreateSolidColorBrush(theme_.text, br.GetAddressOf());
  if (br) {
    const float px = 96.f / dpi_;
    // Windows 10 : pas de bordure DWM, on la trace (coins droits, comme les
    // menus de Windows 10).
    if (!dwmRounded_) {
      br->SetColor(fromBorder(theme_.border));
      rt_->DrawRectangle(D2D1::RectF(px / 2, px / 2, size_.width - px / 2,
                                     size_.height - px / 2),
                         br.Get(), px);
    }
    float fade = fade_.at(now);
    bool layered = fade < 0.995f && layer_;
    if (layered)
      rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                           D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), fade),
                     layer_.Get());
    drawItems(rt_.Get(), br.Get(), highlightRect(now),
              hlShown_ && cursor_ >= 0);
    if (layered)
      rt_->PopLayer();
  }
  HRESULT hr = rt_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    layer_.Reset();
    rt_.Reset();
  }
  armTimer();
}

// ---------------------------------------------------------------- affichage --

void CandidateWindow::show(const std::vector<core::Candidate> &cands,
                           int cursor, const std::string &auxTitle,
                           const RECT &caret, const PanelHints &hints) {
  if (cands.empty()) {
    hide();
    return;
  }
  if (!ensureCreated())
    return;
  refreshTheme();
  if (!ensureGraphics())
    return;

  const bool wasVisible = visible();
  const Mode prevMode = mode_;
  const ULONGLONG now = ::GetTickCount64();
  const D2D1_RECT_F prevHl = highlightRect(now);
  const bool prevHlShown = hlShown_ && cursor_ >= 0;
  std::vector<std::wstring> prevTexts;
  for (const auto &it : items_)
    prevTexts.push_back(it.text);

  // --- modèle -------------------------------------------------------------
  const bool wasLoading = loading_;
  loading_ = isLoadingPlaceholder(cands);
  bool list = loading_;
  for (const auto &c : cands)
    list = list || !c.label.empty();
  mode_ = list ? Mode::List : (hints.grid ? Mode::Grid : Mode::Chips);
  composing_ = hints.composing;
  aux_ = wide(auxTitle);
  items_.clear();
  if (!loading_)
    for (const auto &c : cands) {
      Item it;
      it.text = wide(c.text);
      it.label = wide(c.label);
      it.autoApply = c.autoApply;
      it.emoji = looksEmoji(it.text);
      items_.push_back(std::move(it));
    }
  cursor_ = (cursor >= 0 && cursor < int(items_.size())) ? cursor : -1;
  if (loading_ && !wasLoading)
    spinStart_ = now;
  buildLayout();

  // --- animations -----------------------------------------------------------
  const int glide = animations_ ? kGlideMs : 0;
  if (cursor_ >= 0) {
    D2D1_RECT_F to = items_[cursor_].box;
    bool slide = wasVisible && prevHlShown && prevMode == mode_ && glide;
    D2D1_RECT_F from = slide ? prevHl : to;
    hlX_ = {from.left, to.left, now, slide ? glide : 0};
    hlY_ = {from.top, to.top, now, slide ? glide : 0};
    hlW_ = {from.right - from.left, to.right - to.left, now, slide ? glide : 0};
    hlH_ = {from.bottom - from.top, to.bottom - to.top, now, slide ? glide : 0};
    hlShown_ = true;
  } else {
    hlShown_ = false;
  }
  std::vector<std::wstring> texts;
  for (const auto &it : items_)
    texts.push_back(it.text);
  if (!animations_) {
    fade_ = {1, 1, now, 0};
  } else if (!wasVisible || prevMode != mode_) {
    fade_ = {0, 1, now, kAppearMs}; // apparition
  } else if (!composing_ && mode_ == Mode::Chips && texts != prevTexts) {
    // Barre PASSIVE rafraîchie en arrière-plan (refresh neural) : fondu
    // doux. Pendant la frappe, le contenu change instantanément.
    fade_ = {0.35f, 1, now, kRefreshMs};
  }

  place(caret);
  ::InvalidateRect(hwnd_, nullptr, FALSE);
  ::UpdateWindow(hwnd_);
}

void CandidateWindow::moveTo(const RECT &caret) {
  if (visible())
    place(caret);
}

// Position en pixels physiques (Per-Monitor V2). `c` est déjà validé et
// converti par l'appelant (dpi::toPhysical).
void CandidateWindow::place(const RECT &c) {
  auto scope = dpi::perMonitor();
  HMONITOR mon =
      ::MonitorFromPoint({c.left, c.bottom}, MONITOR_DEFAULTTONEAREST);
  const float newDpi = float(monitorDpi(mon));
  if (newDpi != dpi_) {
    dpi_ = newDpi; // autre écran : redessiner à SON échelle
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
  const float k = dpi_ / 96.f;
  const int w = int(std::ceil(size_.width * k)),
            h = int(std::ceil(size_.height * k));

  // Le premier mot proposé s'aligne sur le mot en cours de frappe : l'œil
  // n'a pas à sauter pour lire la suggestion.
  float lead = 12;
  if (mode_ == Mode::Chips && !items_.empty())
    lead = items_[0].box.left + kChipPadX;
  else if (mode_ == Mode::Grid)
    lead = kGridPad;
  int x = c.left - int(lead * k + 0.5f);
  int y = c.bottom + int(6 * k + 0.5f);

  // Recalée dans l'écran : une barre qui déborde à droite ou sous la barre
  // des tâches serait inutilisable.
  MONITORINFO mi{sizeof(mi)};
  ::GetMonitorInfoW(mon, &mi);
  if (x + w > mi.rcWork.right)
    x = mi.rcWork.right - w;
  if (x < mi.rcWork.left)
    x = mi.rcWork.left;
  if (y + h > mi.rcWork.bottom)
    y = c.top - h - int(6 * k + 0.5f); // au-dessus du caret

  // Rien ne bouge si rien n'a changé : les notifications de mise en page
  // arrivent en rafale pendant la frappe.
  RECT cur{};
  ::GetWindowRect(hwnd_, &cur);
  if (visible() && cur.left == x && cur.top == y && cur.right - cur.left == w &&
      cur.bottom - cur.top == h)
    return;
  ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidateWindow::hide() {
  if (!hwnd_)
    return;
  ::ShowWindow(hwnd_, SW_HIDE);
  if (timerOn_) {
    ::KillTimer(hwnd_, kAnimTimer);
    timerOn_ = false;
  }
  hlShown_ = false;
}

} // namespace win
