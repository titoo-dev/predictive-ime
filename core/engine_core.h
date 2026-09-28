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
  void navigate(PredictState &st, int dir);
  void commitWord(PredictState &st, const std::string &raw, bool trailingSpace,
                  bool learn = true);
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
