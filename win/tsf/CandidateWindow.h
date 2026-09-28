// Barre de candidats — fenêtre Win32 dessinée par nous.
//
// Pourquoi pas l'UI système : TSF n'affiche RIEN de lui-même. Un
// ITfCandidateListUIElement n'est rendu que si l'APPLICATION implémente
// ITfUIElementSink — ce que presque aucune ne fait. Toute IME Windows
// sérieuse (Weasel, l'IME japonais de Microsoft…) dessine donc sa propre
// fenêtre : c'est ce qu'on fait ici, comme qmlpanel côté Wayland.
//
// Contraintes : WS_EX_NOACTIVATE (ne JAMAIS voler le focus — l'application
// perdrait son curseur), topmost, et positionnement sur le caret via
// ITfContextView::GetTextExt.
//
// Rendu : Direct2D + DirectWrite (anticrénelage, emoji en couleur, coins
// arrondis) sur les jetons Fluent de Windows 11 — cf Theme.h. Mêmes trois
// dispositions que qmlpanel : chips de mots, grille emoji, liste numérotée
// de reformulation.
#pragma once

#include "../../core/frontend.h"
#include "Theme.h"

#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace win {

// Ce que la barre ne peut pas déduire des candidats seuls.
struct PanelHints {
  bool composing = false; // un mot est en cours de frappe (préédition)
  bool grid = false;      // picker emoji (':') → grille de 8 colonnes
};

class CandidateWindow {
public:
  ~CandidateWindow();

  bool ensureCreated();
  // Un label non vide (ou le placeholder « ⟳ ») bascule en liste verticale
  // numérotée (reformulation) ; sinon chips horizontales ou grille emoji.
  // `caret` : pixels PHYSIQUES, déjà validés (cf dpi::toPhysical).
  void show(const std::vector<core::Candidate> &cands, int cursor,
            const std::string &auxTitle, const RECT &caret,
            const PanelHints &hints = {});
  // Suit le texte (défilement, fenêtre déplacée) sans toucher au contenu.
  void moveTo(const RECT &caret);
  void hide();
  bool visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }

private:
  template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

  enum class Mode { Chips, Grid, List };

  struct Item {
    std::wstring text, label;
    bool autoApply = false;
    bool emoji = false;
    ComPtr<IDWriteTextLayout> layout, badge;
    D2D1_RECT_F box{}; // en DIP, repère de la carte
  };

  // Interpolation temporelle d'un scalaire (ease-out cubique).
  struct Anim {
    float from = 0, to = 0;
    ULONGLONG start = 0;
    int ms = 0;
    float at(ULONGLONG now) const;
    bool running(ULONGLONG now) const { return ms > 0 && now < start + ms; }
  };

  static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
  bool ensureGraphics();
  void refreshTheme();
  void applyWindowChrome();
  void buildLayout();
  ComPtr<IDWriteTextLayout> makeLayout(const std::wstring &s,
                                       IDWriteTextFormat *fmt, float maxW,
                                       bool wrap);
  void paint(HDC dc);
  void drawItems(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br,
                 const D2D1_RECT_F &hl, bool hlOn);
  void drawSpinner(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br,
                   D2D1_POINT_2F c, ULONGLONG now);
  D2D1_RECT_F highlightRect(ULONGLONG now) const;
  void armTimer();
  void place(const RECT &caret);

  HWND hwnd_ = nullptr;
  float dpi_ = 96.f;
  bool dwmRounded_ = false; // Windows 11 : coins + bordure dessinés par DWM
  bool themeDirty_ = true;
  bool animations_ = true;
  Theme theme_;

  // Direct2D / DirectWrite, créés à la première barre affichée seulement : la
  // DLL est chargée dans CHAQUE application, la plupart n'en auront jamais.
  ComPtr<ID2D1Factory> d2d_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<ID2D1DCRenderTarget> rt_;
  ComPtr<ID2D1Layer> layer_;
  ComPtr<IDWriteTextFormat> body_, emoji_, caption_, badge_;
  ComPtr<IDWriteInlineObject> ellipsis_;
  ComPtr<ID2D1PathGeometry> spinnerArc_;
  ComPtr<ID2D1StrokeStyle> roundCap_;

  Mode mode_ = Mode::Chips;
  std::vector<Item> items_;
  std::wstring aux_;
  ComPtr<IDWriteTextLayout> auxLayout_, placeholder_;
  bool loading_ = false;
  bool composing_ = false;
  int cursor_ = -1;
  D2D1_SIZE_F size_{}; // carte, en DIP
  float auxWidth_ = 0; // largeur de l'étiquette avant les chips (menu Langue)

  // Animations : la pastille GLISSE d'un candidat à l'autre (comme qmlpanel),
  // le contenu apparaît en fondu, le spinner tourne pendant la génération.
  Anim hlX_, hlY_, hlW_, hlH_, fade_;
  bool hlShown_ = false;
  bool timerOn_ = false;
  ULONGLONG spinStart_ = 0;
};

} // namespace win
