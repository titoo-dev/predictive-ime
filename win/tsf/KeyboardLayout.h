// Disposition clavier sous « Predict » : doit être CELLE de l'utilisateur.
//
// Un text service ne choisit pas sa disposition : quand son profil de langue
// L s'active, TSF prend la disposition « de base » de L — l'entrée 0000LLLL
// de HKCU\Keyboard Layout\Preload, résolue par HKCU\Keyboard Layout\Substitutes.
// Constaté sur une machine réelle (anglais configuré en AZERTY) :
//   Predict EN → 04090409 (US QWERTY)  alors que l'anglais y est en AZERTY ;
//   Predict FR → 0409040C (US QWERTY)  à cause d'une substitution périmée
//                0000040c → 00000409.
// Conséquence : touches physiques décalées ET raccourcis faux (Ctrl+Z AZERTY
// arrive comme Ctrl+W). Les voies « officielles » ne marchent pas ici :
// SubstituteKeyboardLayout écrit dans HKLM, refuse les dispositions d'une
// autre langue (le cas anglais + AZERTY), et ActivateProfile rejette tout HKL
// pour un profil de text service (E_INVALIDARG).
//
// Ce module compare donc la disposition active à la liste de l'utilisateur
// (Paramètres › Langue) et, sur demande, aligne la substitution de la
// disposition de base — par utilisateur, sans droits administrateur. Les
// dispositions déjà chargées sont mises en cache par session : la correction
// prend effet à la prochaine ouverture de session.
#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace win::layout {

struct Status {
  LANGID lang = 0;
  std::wstring active; // KLID actif sur le thread, ex. « 00000409 »
  std::wstring wanted; // première disposition de l'utilisateur pour `lang`
  bool matches = true; // vrai aussi quand la liste est illisible (bac à sable)
};

// Dispositions (KLID) que l'utilisateur a choisies pour une langue, dans
// l'ordre des Paramètres. Vide si la liste n'est pas lisible.
std::vector<std::wstring> userLayouts(LANGID lang);

// État de la disposition ACTIVE du thread appelant.
Status current();

// Nom lisible d'une disposition (« Français (AZERTY) »), localisé.
std::wstring displayName(const std::wstring &klid);

// Aligne la disposition de base de chaque langue de Predict sur celle de
// l'utilisateur. Renvoie le nombre de corrections (0 : déjà bon).
// `apply` false : simulation, rien n'est écrit. `plan` reçoit une ligne
// « base : actuelle -> voulue » par correction.
int repair(bool apply = true, std::vector<std::wstring> *plan = nullptr);

} // namespace win::layout
