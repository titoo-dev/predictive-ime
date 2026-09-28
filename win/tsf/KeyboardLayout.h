// Disposition clavier sous « Predict » : doit être CELLE de l'utilisateur.
//
// Un text service n'a pas de disposition à lui. Sans indication, Windows lui
// attribue US QWERTY — constaté pour FR comme pour EN sur une machine
// configurée en AZERTY, et régénéré à chaque ouverture de session
// (HKCU\Keyboard Layout\Preload/Substitutes). Conséquence : touches décalées
// ET raccourcis faux (Ctrl+Z AZERTY reçu comme Ctrl+W).
//
// Ce qui a été essayé et ne marche PAS :
//   - réécrire HKCU\Keyboard Layout\Substitutes : Windows le régénère ;
//   - ActivateProfile avec un HKL : E_INVALIDARG pour un text service ;
//   - ActivateKeyboardLayout depuis le service une fois actif : le
//     sélecteur de Windows resélectionne alors la disposition simple et
//     Predict est éjecté — plus aucune frappe ne lui parvient.
//
// La voie prévue par TSF : déclarer la disposition À L'INSCRIPTION du profil
// (hklSubstitute de RegisterProfile, sous HKLM — regsvr32 en administrateur).
// TSF l'accepte (GetProfile la renvoie) mais la CHARGE à travers
// HKCU\Keyboard Layout\Substitutes : une redirection 0000040c → 00000409
// laissée par Windows quand rien n'était déclaré la ramenait en US — le
// setup la retire (Clear-PredictStaleSubstitute).
// Contraintes (Microsoft, via M. Durdin) : la valeur est un KLID, et la
// disposition doit appartenir à la MÊME langue que le profil. Français +
// AZERTY : possible. Anglais + AZERTY : impossible — ce profil reste en US,
// et l'installation ne le propose pas à qui tape l'anglais en AZERTY
// (Predict FR prédit aussi l'anglais).
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

// Disposition à déclarer pour le profil de `lang` (hklSubstitute) : la
// première disposition de l'utilisateur qui appartient à CETTE langue, sous
// forme de KLID. nullptr si aucune (Windows garde alors US).
HKL substituteFor(LANGID lang);

// Cœurs purs des deux fonctions ci-dessus, testables sans dépendre de la
// configuration de la machine (win/tests).
HKL substituteFrom(LANGID lang, const std::vector<std::wstring> &mine);
bool isUserLayout(const std::wstring &klid,
                  const std::vector<std::wstring> &mine);

} // namespace win::layout
