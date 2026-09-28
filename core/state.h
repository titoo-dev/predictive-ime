// Cœur portable — état de saisie d'un champ (un « input context » fcitx5, un
// ITfContext TSF). Aucune base de classe côté cœur : l'adaptateur fcitx5 le
// possède via une InputContextProperty, l'adaptateur TSF via son contexte.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct PredictState {
  std::string buffer;             // mot en cours (préfixe, UTF-8)
  std::vector<std::string> ctx;   // jusqu'à 8 derniers mots committés
  std::vector<std::string> cands; // candidats courants (preedit/commit)
  int navIndex = 0;               // candidat surligné quand on navigue
  bool navigating = false;        // l'utilisateur a commencé à choisir (Tab)
  bool literalIsWord = false;     // le préfixe tapé est-il déjà un vrai mot ?
  std::string autocomplete;       // mot appliqué sur Espace (haute confiance)
  std::string ghost;              // complétion fantôme (→ l'accepte)
  bool accentOnly = false;        // autocomplete = restauration d'accents pure

  // Fenêtre de REVERT d'une auto-application (Backspace juste après) :
  std::string lastAutoLit;  // le littéral qui a été remplacé
  std::string lastAutoWord; // le mot qui avait été appliqué
  uint32_t lastAutoCps = 0; // points de code committés à effacer
  bool vetoAuto = false;    // l'utilisateur a refusé : Espace garde le littéral

  bool langMenu = false; // panneau de langue ouvert (Ctrl+Shift+L)
  int langIndex = 0;     // choix surligné

  bool reformulating = false;   // cands = variantes de la sélection
  bool reformLoading = false;   // génération en cours (placeholder « ⟳ »)
  std::string reformNotice;     // panneau d'échec (no_key/auth/network/…)
  std::string reformText;       // texte source — regen + revert
  bool reformFromSelection = false; // false = repli « champ entier » : le
                                    // commit doit SUPPRIMER le champ lui-même
  int reformMode = 0;           // index dans reformModes()
  uint32_t reformNonce = 0;     // varie le seed → « régénérer »
  uint32_t reformGen = 0;       // ignore les résultats obsolètes
  std::string reformSource;     // "groq"/"local"/"none" → badge du header

  // Revert de reformulation (Backspace juste après → restaure). One-shot.
  std::string reformRevertOrig;
  uint32_t reformRevertCps = 0;

  // Génération de la barre mot-suivant : un refresh neural arrivé APRÈS que
  // l'état a changé (frappe, commit, reset) est jeté (gen différente).
  uint64_t nextWordGen = 0;
};

} // namespace core
