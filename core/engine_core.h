// Cœur portable — LA machine à états de saisie.
//
// C'est l'intégralité de l'ancien PredictEngine::keyEvent de l'engine fcitx5,
// traduit terme à terme : `ic->…` → verbes Frontend, `FcitxKey_X` → core::Key,
// `event.filterAndAccept()` → `return true` (touche consommée).
//
// Aucun #include de fcitx5 ni de Win32 ici : c'est la condition pour que les
// deux frontaux ne divergent JAMAIS sur le comportement de frappe.
#pragma once

#include "config.h"
#include "daemon_client.h"
#include "frontend.h"
#include "state.h"
#include "text.h"

#include <string>
#include <vector>

namespace core {

// Préférences de SESSION — ni par champ (elles survivent au changement de
// contexte) ni persistées. L'adaptateur les possède : EngineCore est recréé à
// chaque touche, y garder un état le remettrait à zéro à chaque frappe.
struct SessionPrefs {
  // Dernier mode de reformulation utilisé : le prochain Ctrl+Alt+R repart de là.
  int lastReformMode = 0;
};

class EngineCore {
public:
  EngineCore(Frontend &fe, SessionPrefs &prefs) : fe_(fe), prefs_(prefs) {}

  // Traite une touche. `true` = consommée (ne pas la laisser filer vers
  // l'application), `false` = l'application doit la recevoir normalement.
  bool keyEvent(PredictState &st, const KeyEvent &k);

  // Changement de focus / fin de composition : ne JAMAIS perdre ce qui est
  // tapé — le mot en cours est committé tel quel.
  void reset(PredictState &st);

  // Une ligne de refresh neural est arrivée sur la connexion armée.
  void onRefreshCandidates(PredictState &st, std::vector<std::string> cands,
                           uint64_t gen);

  // Résultat (ou échec) d'une reformulation, reposté sur le thread de saisie.
  void onReformResult(PredictState &st, const ReformResult &r, uint32_t gen);
  void onReformPartial(PredictState &st, std::vector<std::string> vars,
                       uint32_t gen);

  // Lance une reformulation : l'adaptateur fournit le thread (il doit appeler
  // onReformPartial/onReformResult via Frontend::postToMain).
  using ReformRunner = std::function<void(std::string text, std::string mode,
                                          uint32_t nonce, int n, uint32_t gen)>;
  void setReformRunner(ReformRunner r) { runReform_ = std::move(r); }

  // Générations en cours — l'adaptateur en a besoin pour jeter l'obsolète.
  uint64_t nextWordGen(const PredictState &st) const { return st.nextWordGen; }

  // Ouvre le picker emoji (ou le referme s'il est déjà ouvert). Public :
  // l'adaptateur fcitx5 l'appelle aussi quand Super+; tombe alors qu'une AUTRE
  // méthode d'entrée est active (il bascule sur predict puis ouvre).
  void toggleEmojiPicker(PredictState &st);

  // Grille du picker emoji : 8 colonnes × 3 lignes par PAGE. Le daemon rend
  // jusqu'à 4 pages de résultats ; les flèches débordent d'une page à l'autre.
  static constexpr int kGridCols = 8;
  static constexpr int kGridRows = 3;
  static constexpr int kGridPage = kGridCols * kGridRows;

private:
  // --- rendu ---------------------------------------------------------------
  void setCandidates(PredictState &st);
  void setLangMenuCandidates(PredictState &st);
  void setReformulationCandidates(PredictState &st);
  void showReformNotice(PredictState &st, const std::string &kind);
  void clearPanel(PredictState &st);

  // --- prédiction ----------------------------------------------------------
  void updateCompletion(PredictState &st);
  void showNextWord(PredictState &st);
  std::vector<std::string> contextFor(PredictState &st);
  std::string wideTextFor(PredictState &st);
  void pushCtx(PredictState &st, const std::string &word);

  // --- édition -------------------------------------------------------------
  // `clamp` : borne au lieu de boucler — c'est le mode GRILLE (emoji).
  void navigate(PredictState &st, int dir, bool clamp = false);
  // Surlignage sur un index ABSOLU de la page (Début/Fin de la grille).
  void navigateTo(PredictState &st, int index);
  void commitWord(PredictState &st, const std::string &raw, bool trailingSpace,
                  bool learn = true);
  // Ferme le picker sans rien écrire (Échap, re-appui, commit du littéral).
  void closeEmojiPicker(PredictState &st);
  // Candidat d'index LOCAL à la page affichée (le picker n'en montre que 24).
  const std::string &candOf(PredictState &st, int local);
  // Nombre de candidats de la page courante.
  int pageCount(const PredictState &st) const;
  // Change de page en gardant la colonne ; false si la page n'existe pas.
  bool turnPage(PredictState &st, int delta, int column);
  // « 2/4 », vide s'il n'y a qu'une page.
  std::string pageIndicator(const PredictState &st) const;
  // Le texte FANTÔME est-il affiché ? Un seul endroit : ce que
  // updateCompletion peint et ce que la touche → accepte doivent coïncider.
  bool ghostShown(const PredictState &st) const;
  const std::string &highlighted(PredictState &st);
  std::string chooseOnSpace(PredictState &st);
  void frenchThinBefore(PredictState &st);
  void deleteSurroundingBefore(PredictState &st, unsigned n);

  // --- panneaux modaux ------------------------------------------------------
  void enterLangMenu(PredictState &st);
  void exitLangMenu(PredictState &st);
  void applyLangChoice(PredictState &st, int idx);
  void enterReformulation(PredictState &st);
  void exitReformulation(PredictState &st);
  void commitReformulation(PredictState &st, int idx);
  void runReformulation(PredictState &st);

  // Sous-parties de keyEvent : renvoient `true` si la touche est consommée,
  // `false` si le traitement doit CONTINUER dans keyEvent.
  bool handleLangMenu(PredictState &st, const KeyEvent &k, bool &consumed);
  bool handleReformulation(PredictState &st, const KeyEvent &k, bool &consumed);

  Frontend &fe_;
  SessionPrefs &prefs_;
  ReformRunner runReform_;
};

} // namespace core
