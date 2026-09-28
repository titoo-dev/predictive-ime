// Palette de la barre, dérivée du thème Windows COURANT.
//
// Une IME qui dessine sa propre fenêtre doit ressembler au système qui
// l'entoure : mêmes surfaces que les menus de Windows 11, couleur d'accent
// choisie par l'utilisateur, et — non négociable — le contraste élevé, où
// seules les couleurs système sont lisibles pour qui l'a activé.
#pragma once

#include <d2d1.h>
#include <windows.h>

namespace win {

struct Theme {
  bool dark = false;
  bool highContrast = false;
  D2D1_COLOR_F surface{};    // fond de la carte
  D2D1_COLOR_F text{};       // texte courant
  D2D1_COLOR_F textDim{};    // en-têtes, placeholders
  D2D1_COLOR_F accent{};     // remplissage accent (candidat surligné)
  D2D1_COLOR_F onAccent{};   // texte posé sur l'accent
  D2D1_COLOR_F accentText{}; // accent lisible sur la surface (auto-appliqué)
  D2D1_COLOR_F subtle{};     // voile de sélection des lignes (liste)
  D2D1_COLOR_F outline{};    // séparateurs, bordure dessinée à la main
  COLORREF border = 0;       // bordure DWM (Windows 11)
};

// Lit le registre et SystemParametersInfo : à appeler sur changement de
// réglages (WM_SETTINGCHANGE…), pas à chaque touche.
Theme loadSystemTheme();

// Les animations suivent le réglage « Effets d'animation » de Windows.
bool systemAnimationsEnabled();

inline D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a) {
  c.a *= a;
  return c;
}

} // namespace win
