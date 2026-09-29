// Cœur portable — la SEULE surface par laquelle la machine à états touche au
// monde. fcitx5 et TSF en sont deux implémentations ; tout le reste de la
// logique de saisie est commun.
//
// Ce qui suit est le contrat exact tiré de l'engine fcitx5 d'origine : chaque
// verbe correspond à un appel `ic->…` qui existait dans predict.cpp.
#pragma once

#include "os_compat.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace core {

// Une chip de la barre. `autoApply` = ce que l'Espace appliquera (rendu en
// gras/liseré) ; `label` non vide fait basculer l'UI en liste verticale
// numérotée (mode reformulation).
struct Candidate {
  std::string text;
  bool autoApply = false;
  std::string label;
};

// Touches « nommées » dont la logique dépend. Tout le reste passe par le point
// de code Unicode — c'est ce qui rend la machine à états indépendante des
// keysyms X11 comme des VK Windows.
enum class Key {
  None,
  Escape,
  Backspace,
  Tab,
  ShiftTab,
  Enter,
  Space,
  Left,
  Right,
  Up,
  Down,
  Home,     // grille emoji : première / dernière case
  End,
  PageUp,   // grille emoji : page précédente / suivante
  PageDown,
};

struct KeyEvent {
  Key key = Key::None;
  uint32_t cp = 0; // caractère produit (0 si la touche n'en produit pas)
  bool ctrl = false, alt = false, shift = false, super = false;
  // Chiffre PHYSIQUE 0-based pour les panneaux modaux (langue, reformulation).
  // Sur AZERTY les chiffres exigent Shift : l'adaptateur le calcule depuis la
  // rangée physique, sinon « autre touche » fermait le panneau EN SILENCE.
  int panelDigit = -1;

  // « Un modificateur qui change le sens de la touche ». Shift en est exclu :
  // il sert à taper des majuscules.
  bool mod() const { return ctrl || alt || super; }
};

// Texte autour du curseur tel que l'application le rapporte.
struct Surrounding {
  std::string text;     // UTF-8
  size_t cursor = 0;    // en POINTS DE CODE, pas en octets
  size_t anchor = 0;    // == cursor s'il n'y a pas de sélection
  std::string selected; // sélection rapportée (souris), sinon vide
};

class Frontend {
public:
  virtual ~Frontend() = default;

  // --- insertion -----------------------------------------------------------
  virtual void commitText(const std::string &utf8) = 0;
  // Préédition : `typed` souligné, curseur à la fin. Vide = pas de préédition.
  virtual void setPreedit(const std::string &typed) = 0;

  // --- barre de candidats ---------------------------------------------------
  // cursor < 0 : aucun surlignage. `auxTitle` = en-tête (mode reformulation).
  virtual void setCandidates(const std::vector<Candidate> &cands, int cursor,
                             const std::string &auxTitle) = 0;
  virtual void clearPanel() = 0; // préédition + barre
  virtual bool hasCandidates() const = 0;

  // --- picker emoji (Super+;) ------------------------------------------------
  // La requête n'est PAS du texte en cours de saisie, c'est une recherche :
  // elle ne va pas dans l'application (préédition vide) mais dans le champ de
  // recherche du PANNEAU, qui marque aussi le mode grille. `page` : « 2/4 »,
  // vide s'il n'y a qu'une page. Appelé AVANT setCandidates. Vide aussi la
  // préédition de l'application.
  virtual void setPanelQuery(const std::string &query,
                             const std::string &page) = 0;
  // Un panneau vient de s'ouvrir SANS préédition dans l'application (picker,
  // menu de langue). Sous Wayland, les clients Chromium ne publient leur
  // rectangle de curseur que sur un changement de préédition : l'adaptateur
  // fcitx5 le provoque (cf pingCaretRect). No-op ailleurs.
  virtual void pingCaret() {}
  // Le picker vient de se FERMER, quel que soit le chemin (emoji choisi,
  // Échap, re-appui, perte de focus). L'adaptateur fcitx5 y rend la méthode
  // d'entrée qu'il avait empruntée pour l'ouvrir. No-op ailleurs.
  virtual void pickerClosed() {}

  // --- texte environnant ----------------------------------------------------
  // false quand l'application ne l'expose pas (terminaux, certains Electron) :
  // accord, récence, auto-majuscule et revert Backspace dégradent alors
  // proprement sur les mots que l'IME a lui-même committés.
  virtual bool surrounding(Surrounding &out) = 0;
  // Supprime `cps` points de code AVANT le curseur. L'implémentation DOIT
  // aussi mettre à jour sa copie locale du texte environnant : l'application
  // ne renvoie son update qu'après un aller-retour, et une complétion relancée
  // juste derrière lirait sinon l'ancien texte.
  virtual void deleteBefore(unsigned cps) = 0;
  // Supprime `count` points de code à partir de `back` avant le curseur —
  // utilisé pour remplacer TOUT le champ en reformulation sans sélection.
  virtual void deleteRange(unsigned back, unsigned count) = 0;

  // --- contexte applicatif --------------------------------------------------
  virtual std::string program() = 0; // pour nextWordBarExclude
  virtual bool isPasswordField() = 0;

  // --- asynchrone -----------------------------------------------------------
  // Surveille UNE socket en lecture (refresh neural en deux phases). Une seule
  // à la fois : réarmer remplace la précédente. La callback tourne sur le
  // thread de saisie.
  virtual void watchReadable(sock_t fd, std::function<void()> onReadable) = 0;
  virtual void stopWatch() = 0;
  // Exécute sur le thread de saisie depuis un thread de travail
  // (reformulation). Ne doit rien faire si le contexte est mort.
  virtual void postToMain(std::function<void()> fn) = 0;

  // Ouvre le dialogue de configuration de la clé API (best-effort, peut être
  // un no-op sur un frontal qui n'en a pas).
  virtual void openKeyDialog() {}
};

} // namespace core
