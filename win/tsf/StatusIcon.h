// Glyphe de l'indicateur de la barre des tâches.
//
// Monochrome, blanc ou noir selon la barre des tâches (≠ thème des
// applications), comme les icônes système de Windows 11 — une icône couleur
// y détonnerait. Motif de l'icône de l'application (scripts/make-icon.ps1) :
// texte tapé, curseur, suggestion fantôme ; estompé quand la prédiction est
// coupée.
#pragma once

#include <windows.h>

namespace win {

// Icône de `size` px. L'appelant la détruit (DestroyIcon).
HICON makeStatusIcon(int size, bool enabled);

} // namespace win
