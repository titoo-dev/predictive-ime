// DPI : une barre nette même dans les applications qui ne gèrent pas le DPI.
//
// Le text service vit dans le process hôte et hérite de SA conscience DPI.
// Dans une application « DPI unaware » (encore courante : vieux éditeurs,
// installeurs), une fenêtre créée naïvement est rendue à 96 dpi puis ÉTIRÉE
// par Windows — texte flou à 150 %. On crée donc la barre en Per-Monitor V2
// (mode mixte, Windows 10 1607+) et on convertit nous-mêmes le caret, que
// l'application rapporte dans SES coordonnées logiques.
#pragma once

#include <windows.h>

namespace win::dpi {

// Bascule le contexte DPI du thread le temps d'une portée.
class Scope {
public:
  explicit Scope(DPI_AWARENESS_CONTEXT ctx)
      : prev_(ctx ? ::SetThreadDpiAwarenessContext(ctx) : nullptr) {}
  ~Scope() {
    if (prev_)
      ::SetThreadDpiAwarenessContext(prev_);
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  DPI_AWARENESS_CONTEXT prev_;
};

inline Scope perMonitor() {
  return Scope(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

// Rectangle de caret rapporté par l'application (coordonnées LOGIQUES de
// l'hôte) → pixels physiques. false si le rectangle n'est pas plausible.
//
// Garde-fou contre « la barre apparaît n'importe où » : certaines
// applications renvoient un rectangle vide, relatif au client, ou celui du
// document entier (le piège connu de TS_E_NOLAYOUT, cf. bug Mozilla
// 1609675 : la barre de MS-IME partait en haut à gauche de l'écran). Un caret
// DOIT tomber dans la fenêtre racine qui a le focus — sinon on refuse, et
// l'appelant garde la dernière position valide.
//
// Conversion : identité pour une application Per-Monitor (la majorité) ;
// sinon mise à l'échelle affine via la fenêtre racine, mesurée dans les DEUX
// espaces — indépendante de l'API qui a produit le rectangle.
inline bool toPhysical(HWND host, const RECT &r, RECT &out) {
  if (!host || r.bottom <= r.top || r.right < r.left)
    return false;
  HWND root = ::GetAncestor(host, GA_ROOT);
  if (!root)
    root = host;
  DPI_AWARENESS_CONTEXT hostCtx = ::GetWindowDpiAwarenessContext(root);
  RECT lg{}, ph{};
  {
    Scope s(hostCtx);
    if (!::GetWindowRect(root, &lg))
      return false;
  }
  const LONG lw = lg.right - lg.left, lh = lg.bottom - lg.top;
  if (lw <= 0 || lh <= 0)
    return false;
  // Plus haut que la fenêtre = rectangle du document ou du champ, pas une
  // ligne de texte ; hors de la fenêtre (marge de 48 px pour les bords
  // arrondis et les listes déroulantes) = autre repère de coordonnées.
  const LONG margin = 48;
  const LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
  if (r.bottom - r.top > lh || cx < lg.left - margin ||
      cx > lg.right + margin || cy < lg.top - margin ||
      cy > lg.bottom + margin)
    return false;

  if (hostCtx && ::GetAwarenessFromDpiAwarenessContext(hostCtx) ==
                     DPI_AWARENESS_PER_MONITOR_AWARE) {
    out = r;
    return true;
  }
  {
    Scope s(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!::GetWindowRect(root, &ph))
      return false;
  }
  const double sx = double(ph.right - ph.left) / lw,
               sy = double(ph.bottom - ph.top) / lh;
  auto mx = [&](LONG x) { return LONG(ph.left + (x - lg.left) * sx + 0.5); };
  auto my = [&](LONG y) { return LONG(ph.top + (y - lg.top) * sy + 0.5); };
  out = RECT{mx(r.left), my(r.top), mx(r.right), my(r.bottom)};
  return true;
}

} // namespace win::dpi
