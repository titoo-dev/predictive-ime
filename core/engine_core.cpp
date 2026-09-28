#include "engine_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace core {

// ============================================================== rendu ========

void EngineCore::clearPanel(PredictState &) { fe_.clearPanel(); }

void EngineCore::setCandidates(PredictState &st) {
  // Le candidat que l'Espace appliquera est marqué (gras → liseré dans l'UI).
  bool willAuto = !st.buffer.empty() && !st.autocomplete.empty() &&
                  !st.vetoAuto && (!st.literalIsWord || st.accentOnly);
  // Picker emoji : une PAGE de la grille seulement ; ailleurs, toute la liste.
  size_t from = st.pageStart < st.cands.size() ? st.pageStart : 0;
  size_t to = isEmojiBuffer(st.buffer)
                  ? std::min(st.cands.size(), from + size_t(kGridPage))
                  : st.cands.size();
  std::vector<Candidate> cands;
  cands.reserve(to - from);
  for (size_t i = from; i < to; i++) {
    const std::string &w = st.cands[i];
    cands.push_back({applyCase(w, st.buffer),
                     willAuto && w == st.autocomplete, std::string{}});
  }
  // cursor -1 : aucun surlignage tant qu'on ne navigue pas.
  fe_.setCandidates(cands, st.navigating ? st.navIndex : -1, std::string{});
}

void EngineCore::setLangMenuCandidates(PredictState &st) {
  int n = 0;
  const LangChoice *choices = langChoices(n);
  std::vector<Candidate> cands;
  for (int i = 0; i < n; i++)
    cands.push_back({choices[i].label, false, std::string{}});
  fe_.setCandidates(cands, st.langIndex, "Langue");
}

void EngineCore::setReformulationCandidates(PredictState &st) {
  std::vector<Candidate> cands;
  std::string header;
  if (st.reformLoading) {
    // Pendant le chargement : un seul candidat « ⟳ … » SANS numéro (l'UI le
    // détecte → spinner).
    for (auto &v : st.cands)
      cands.push_back({v, false, std::string{}});
  } else {
    int i = 1;
    for (auto &v : st.cands)
      cands.push_back({v, false, std::to_string(i++)}); // verbatim (phrases)
    int nm = 0;
    const ReformMode *modes = reformModes(nm);
    std::string badge = st.reformSource == "groq"    ? "  ⚡ Groq"
                        : st.reformSource == "local" ? "  ◆ local"
                                                     : "";
    header = std::string(modes[st.reformMode].label) + badge +
             "   · ←→/rfsct mode";
  }
  fe_.setCandidates(cands, st.navIndex, header);
}

void EngineCore::showReformNotice(PredictState &st, const std::string &kind) {
  st.reformNotice = kind.empty() ? "network" : kind;
  st.reformLoading = false;
  st.navigating = false;
  std::string msg;
  if (st.reformNotice == "no_key")
    msg = "Clé API Groq requise — Entrée : configurer · Échap";
  else if (st.reformNotice == "auth")
    msg = "Clé API refusée — Entrée : reconfigurer · Échap";
  else if (st.reformNotice == "no_text")
    msg = "Rien à reformuler — sélectionnez du texte";
  else if (st.reformNotice == "bad_url")
    msg = "reformBaseUrl refusé — https requis (voir config.json)";
  else
    msg = "⚠ Reformulation indisponible (réseau/API)";
  st.cands = {msg};
  fe_.setCandidates({{msg, false, std::string{}}}, -1, std::string{});
}

// ========================================================== prédiction =======

// La VRAIE phrase avant le curseur en priorité — source de vérité pour
// l'accord grammatical et le mot-suivant —, sinon nos derniers mots committés.
// Fenêtre de 8 mots : le n-gramme n'en lit que 2, mais la couche d'accord du
// daemon a besoin de tout le groupe nominal.
std::vector<std::string> EngineCore::contextFor(PredictState &st) {
  Surrounding s;
  if (fe_.surrounding(s)) {
    auto cps = decodeUtf8(s.text);
    if (s.cursor < cps.size())
      cps.resize(s.cursor);
    auto ws = lastWords(cps, 8);
    if (!ws.empty())
      return ws;
  }
  return st.ctx;
}

// Contexte LARGE pour le prédicteur neuronal : le texte BRUT avant le curseur
// (casse, ponctuation, phrases précédentes). ~240 caractères, coupés à un
// début de mot.
std::string EngineCore::wideTextFor(PredictState &st) {
  Surrounding s;
  if (fe_.surrounding(s)) {
    auto cps = decodeUtf8(s.text);
    if (s.cursor < cps.size())
      cps.resize(s.cursor);
    size_t from = cps.size() > 240 ? cps.size() - 240 : 0;
    if (from > 0) // ne pas démarrer en plein mot
      while (from < cps.size() && isLetterCp(cps[from]))
        ++from;
    std::string out;
    for (size_t i = from; i < cps.size(); i++)
      appendCp(out, cps[i]);
    if (!out.empty())
      return out;
  }
  std::string out;
  for (const auto &w : st.ctx) {
    if (!out.empty())
      out += ' ';
    out += w;
  }
  return out;
}

void EngineCore::pushCtx(PredictState &st, const std::string &word) {
  st.ctx.push_back(word);
  if (st.ctx.size() > 8)
    st.ctx.erase(st.ctx.begin());
}

void EngineCore::updateCompletion(PredictState &st) {
  st.nextWordGen++; // la frappe invalide tout refresh mot-suivant en vol
  if (st.buffer.empty()) {
    showNextWord(st);
    return;
  }
  auto reply = queryDaemon(contextFor(st), st.buffer, wideTextFor(st));
  st.cands = reply.candidates;
  st.literalIsWord = reply.literalIsWord;
  st.autocomplete = reply.autocomplete;
  st.ghost = reply.ghost;
  st.accentOnly = reply.accentOnly;
  // Repli « le brut » : le mot tapé reste proposable quand le modèle ne rend
  // rien. PAS pour le picker emoji — proposer ':zzz' comme candidat n'a aucun
  // sens ; la grille affiche son état vide.
  if (st.cands.empty() && !isEmojiBuffer(st.buffer))
    st.cands.push_back(st.buffer);

  // PICKER EMOJI : la requête n'est PAS du texte en cours de saisie, c'est une
  // recherche. Rien dans l'application ; le panneau en fait un champ de
  // recherche (cf Frontend::setPanelQuery).
  if (isEmojiBuffer(st.buffer)) {
    st.pageStart = 0; // la frappe change les résultats → retour page 1
    // setPanelQuery vide aussi la préédition de l'application (en préservant
    // le ping de caret fcitx5 en vol).
    fe_.setPanelQuery(st.buffer, pageIndicator(st));
    setCandidates(st);
    return;
  }

  // Pas de filet, pas de remplacement : si l'app n'expose pas le texte
  // environnant, le revert Backspace d'une auto-application est IMPOSSIBLE —
  // l'Espace garde alors le littéral (les candidats restent, Tab choisit).
  Surrounding probe;
  if (engineCfg().autoApplyNeedsRevert && !isTriggerBuffer(st.buffer) &&
      !fe_.surrounding(probe)) {
    st.autocomplete.clear();
    st.accentOnly = false;
    // le fantôme reste : → est un accept EXPLICITE, pas besoin de revert.
  }

  // EFFACEMENT : reculer désarme l'Espace pour ce mot (cf
  // PredictState::erasing). Le fantôme, lui, est déjà éteint par ghostShown().
  // Les candidats RESTENT : Tab / 1-6 permettent toujours de choisir
  // explicitement — c'est l'application AUTOMATIQUE qu'on retire.
  if (st.erasing) {
    st.autocomplete.clear();
    st.accentOnly = false;
  }

  // GHOST TEXT : le reste de la complétion haute-confiance s'affiche dans le
  // préedit (conditions dans ghostShown(), partagées avec la touche →).
  std::string ghost;
  if (ghostShown(st))
    ghost = st.ghost.substr(st.buffer.size());

  fe_.setPreedit(st.buffer, ghost);
  setCandidates(st);
}

// Mode mot-suivant : candidats prédits depuis le contexte (display-only). Un
// contexte VIDE est une requête valide : le daemon répond avec les amorces de
// phrase (<s>) — début de champ, ou après « . ! ? ».
void EngineCore::showNextWord(PredictState &st) {
  st.autocomplete.clear(); // pas de marquage « auto » en mot-suivant
  st.ghost.clear();
  st.accentOnly = false;
  st.literalIsWord = false;
  if (!engineCfg().nextWordBar) { // mode calme : pas de barre spéculative
    st.cands.clear();
    clearPanel(st);
    return;
  }
  // Programme exclu (terminal où la barre ne peut pas s'ancrer au curseur) :
  // match sous-chaîne INSENSIBLE à la casse.
  const std::string prog = lowerAscii(fe_.program());
  for (const auto &pat : engineCfg().nextWordBarExclude)
    if (!pat.empty() && prog.find(lowerAscii(pat)) != std::string::npos) {
      st.cands.clear();
      clearPanel(st);
      return;
    }
  auto ctx = contextFor(st);
  st.nextWordGen++; // nouvelle barre → tout refresh antérieur est périmé
  sock_t pendingFd = kBadSock;
  auto reply = queryDaemon(ctx, "", wideTextFor(st),
                           engineCfg().asyncNextWord ? &pendingFd : nullptr);
  // Deux phases : la 1re réponse (n-gram, instantanée) s'affiche tout de
  // suite ; si le daemon annonce un refresh neural, la connexion reste
  // ouverte et la barre se mettra à jour depuis la boucle d'événements.
  if (oscompat::sockValid(pendingFd)) {
    uint64_t gen = st.nextWordGen;
    PredictState *pst = &st;
    sock_t fd = pendingFd;
    auto carry = std::make_shared<std::string>();
    fe_.watchReadable(fd, [this, pst, fd, gen, carry]() {
      std::vector<std::string> cands;
      bool done = false;
      bool got = readRefresh(fd, *carry, cands, done);
      if (got)
        onRefreshCandidates(*pst, std::move(cands), gen);
      if (got || done)
        fe_.stopWatch(); // single-shot : une ligne = un refresh
    });
  }
  st.cands = reply.candidates;
  if (st.cands.empty()) {
    clearPanel(st);
    return;
  }
  // La barre mot-suivant n'a PAS de préédition : elle est spéculative.
  fe_.setPreedit(std::string{}, std::string{});
  setCandidates(st);
}

void EngineCore::onRefreshCandidates(PredictState &st,
                                     std::vector<std::string> cands,
                                     uint64_t gen) {
  if (cands.empty())
    return;
  // Périmé si quoi que ce soit a bougé depuis l'armement (frappe, commit,
  // navigation, reset) — la barre affichée doit toujours refléter l'état.
  if (st.nextWordGen != gen || !st.buffer.empty() || st.navigating)
    return;
  st.cands = std::move(cands);
  st.navIndex = 0;
  setCandidates(st);
}

// ============================================================ édition =======

// Candidat d'index LOCAL à la page affichée (le picker emoji n'en montre que 24
// à la fois ; partout ailleurs la page vaut toute la liste). TOUT accès par
// index passe par là : sans ça, Entrée committerait l'emoji de la page 1.
const std::string &EngineCore::candOf(PredictState &st, int local) {
  if (st.cands.empty())
    return st.buffer;
  size_t i = st.pageStart + size_t(local < 0 ? 0 : local);
  if (i >= st.cands.size())
    i = st.pageStart < st.cands.size() ? st.pageStart : 0;
  return st.cands[i];
}

const std::string &EngineCore::highlighted(PredictState &st) {
  return candOf(st, st.navIndex);
}

int EngineCore::pageCount(const PredictState &st) const {
  size_t rest =
      st.cands.size() > st.pageStart ? st.cands.size() - st.pageStart : 0;
  return int(isEmojiBuffer(st.buffer) ? std::min<size_t>(rest, kGridPage)
                                      : rest);
}

std::string EngineCore::pageIndicator(const PredictState &st) const {
  int total = int(st.cands.size());
  if (!isEmojiBuffer(st.buffer) || total <= kGridPage)
    return {};
  int pages = (total + kGridPage - 1) / kGridPage;
  int cur = int(st.pageStart) / kGridPage + 1;
  return std::to_string(cur) + "/" + std::to_string(pages);
}

// Change de page (delta en pages) en gardant la COLONNE, et surligne la ligne
// d'arrivée : vers le bas on entre par le haut, vers le haut par le bas.
bool EngineCore::turnPage(PredictState &st, int delta, int column) {
  long start = long(st.pageStart) + long(delta) * kGridPage;
  if (start < 0 || start >= (long)st.cands.size())
    return false;
  st.pageStart = size_t(start);
  int last = pageCount(st) - 1;
  int idx = delta > 0 ? column : (kGridRows - 1) * kGridCols + column;
  st.navigating = true;
  st.navIndex = std::max(0, std::min(idx, last));
  fe_.setPanelQuery(st.buffer, pageIndicator(st));
  setCandidates(st);
  return true;
}

bool EngineCore::ghostShown(const PredictState &st) const {
  // Il ne s'affiche que si la complétion PROLONGE octet-à-octet la frappe
  // (jamais pour une correction floue : la barre + le liseré s'en chargent),
  // et jamais après un refus (vetoAuto) ni un effacement (erasing).
  return engineCfg().ghostText && !st.literalIsWord && !st.vetoAuto &&
         !st.erasing && st.ghost.size() > st.buffer.size() &&
         st.ghost.compare(0, st.buffer.size(), st.buffer) == 0;
}

// Mot retenu quand on appuie sur Espace en cours de composition :
//  - en navigation → le candidat surligné ;
//  - sinon, si le préfixe n'est PAS un mot réel → autocomplétion/autocorrection ;
//  - sinon → le littéral (on ne touche pas à un vrai mot).
std::string EngineCore::chooseOnSpace(PredictState &st) {
  if (st.navigating && !st.cands.empty())
    return highlighted(st);
  // l'utilisateur vient de refuser une auto-application : le littéral reste.
  if (st.vetoAuto)
    return st.buffer;
  // Une RESTAURATION D'ACCENTS (francais→français) s'applique même si le tapé
  // est un vrai mot du corpus — elle ne change jamais le mot.
  if (!st.autocomplete.empty() && (!st.literalIsWord || st.accentOnly))
    return st.autocomplete;
  return st.buffer;
}

// Surligne un candidat. dir : +1 suivant, -1 précédent, 0 (1er appui) → le
// 1er. Premier appui en ARRIÈRE (⇧Tab/↑) → on entre par la droite (dernier).
// `clamp` : borne au lieu de boucler — le mode GRILLE. Dans une grille, boucler
// désoriente (↓ sur la dernière ligne renvoyait en haut) ; borné, une flèche
// qui ne peut plus avancer ne bouge pas, et ↓ depuis une ligne incomplète
// tombe sur la DERNIÈRE case (comportement des grilles emoji système).
void EngineCore::navigate(PredictState &st, int dir, bool clamp) {
  int sz = pageCount(st);
  if (sz == 0)
    return;
  int next = st.navigating ? st.navIndex + dir : (dir < 0 ? sz - 1 : 0);
  next = clamp ? std::max(0, std::min(next, sz - 1)) : ((next % sz) + sz) % sz;
  st.navigating = true;
  st.navIndex = next;
  // Reflète le candidat surligné dans la préédition — SAUF en mode emoji : la
  // préédition reste vide (la requête vit dans le champ de recherche).
  if (!st.buffer.empty() && !isEmojiBuffer(st.buffer))
    fe_.setPreedit(applyCase(candOf(st, next), st.buffer), std::string{});
  setCandidates(st);
}

void EngineCore::navigateTo(PredictState &st, int index) {
  if (pageCount(st) == 0)
    return;
  st.navigating = true; // → navigate(0) repart de `index` exactement
  st.navIndex = index;
  navigate(st, 0, /*clamp=*/true);
}

// ========================================================== picker emoji ====

void EngineCore::closeEmojiPicker(PredictState &st) {
  st.buffer.clear();
  st.cands.clear();
  st.navigating = false;
  st.pageStart = 0;
  clearPanel(st);
  fe_.pickerClosed();
}

// Super+; : ouvre le picker, TOUJOURS disponible — buffer vide, en plein mot,
// barre mot-suivant ouverte. Le mot en cours est committé tel quel, SANS
// apprendre (fragment tapé, pas un mot validé). Re-presser referme.
void EngineCore::toggleEmojiPicker(PredictState &st) {
  if (isEmojiBuffer(st.buffer)) {
    closeEmojiPicker(st);
    return;
  }
  if (!st.buffer.empty())
    commitWord(st, st.buffer, /*trailingSpace=*/false, /*learn=*/false);
  st.buffer = ":";
  st.navigating = false;
  st.pageStart = 0;
  updateCompletion(st);
  fe_.pingCaret(); // panneau sans préédition : le client doit publier son caret
}

void EngineCore::deleteSurroundingBefore(PredictState &, unsigned n) {
  fe_.deleteBefore(n);
}

// Insère une fine insécable (U+202F) avant la ponctuation haute. Absorbe une
// espace ORDINAIRE déjà tapée (sinon « mot  ! ») et ne fait rien si une fine
// est déjà là — best-effort via le texte environnant.
void EngineCore::frenchThinBefore(PredictState &) {
  Surrounding s;
  if (fe_.surrounding(s)) {
    auto cps = decodeUtf8(s.text);
    if (s.cursor > 0 && s.cursor <= cps.size()) {
      uint32_t prev = cps[s.cursor - 1];
      if (prev == 0x202F)
        return; // déjà une fine insécable
      if (prev == ' ')
        fe_.deleteBefore(1); // absorbe l'espace ordinaire
    }
  }
  fe_.commitText(kThinNbsp);
}

// Valide un mot : applique la casse du buffer, committe, apprend (sauf
// annulation), met à jour le contexte, puis (si espace) propose le suivant.
void EngineCore::commitWord(PredictState &st, const std::string &raw,
                            bool trailingSpace, bool learn) {
  // Picker emoji : committer le LITTÉRAL (Échap, Entrée nue, ponctuation,
  // Espace sans candidat…) cracherait ':requête' dans le texte alors que ce
  // ':' vient du raccourci Super+;, pas de la frappe. Un picker qu'on annule
  // se ferme, point — c'est un menu, pas une composition.
  if (isEmojiBuffer(st.buffer) && raw == st.buffer) {
    closeEmojiPicker(st);
    return;
  }
  st.nextWordGen++; // le commit invalide tout refresh en vol
  bool trigger = isTriggerBuffer(st.buffer); // emoji ':' / snippet ';'
  bool wasPicker = isEmojiBuffer(st.buffer); // emoji choisi → on referme
  std::string word = applyCase(raw, st.buffer);
  // Auto-majuscule en DÉBUT DE PHRASE : lastWords s'arrête aux frontières
  // « . ! ? », donc un contexte VIDE signifie début de champ OU juste après
  // une fin de phrase — exactement les cas à capitaliser.
  if (engineCfg().autoCapitalize && !trigger && contextFor(st).empty())
    word = capFirst(word);
  fe_.commitText(trailingSpace ? word + " " : word);
  if (trigger) {
    // un emoji/snippet choisi compte comme « favori » ; le contexte de mots
    // reste inchangé.
    if (learn && word != st.buffer)
      learnDaemon(std::string{}, word);
  } else {
    // multi-mots : chaque mot nourrit l'apprentissage et le contexte.
    size_t from = 0;
    while (from <= word.size()) {
      size_t sp = word.find(' ', from);
      std::string w = word.substr(
          from, sp == std::string::npos ? std::string::npos : sp - from);
      if (!w.empty()) {
        if (learn) {
          std::string prev = st.ctx.empty() ? std::string{} : st.ctx.back();
          learnDaemon(prev, w);
        }
        pushCtx(st, w);
      }
      if (sp == std::string::npos)
        break;
      from = sp + 1;
    }
  }
  st.buffer.clear();
  st.cands.clear();
  st.navigating = false;
  st.vetoAuto = false; // le veto ne vaut que pour le mot en cours
  st.erasing = false;  // idem : le mot suivant repart avec le fantôme
  st.pageStart = 0;
  if (trailingSpace)
    showNextWord(st);
  else
    clearPanel(st);
  if (wasPicker)
    fe_.pickerClosed();
}

// ====================================================== panneaux modaux ======

void EngineCore::enterLangMenu(PredictState &st) {
  if (!st.buffer.empty()) // ne jamais perdre la frappe en cours
    commitWord(st, st.buffer, /*trailingSpace=*/false, /*learn=*/false);
  st.langMenu = true;
  st.navigating = false;
  int n = 0;
  const LangChoice *choices = langChoices(n);
  std::string cur = readLang();
  st.langIndex = 0;
  for (int i = 0; i < n; i++)
    if (cur == choices[i].value)
      st.langIndex = i;
  setLangMenuCandidates(st);
  fe_.pingCaret(); // panneau sans préédition : même repli que le picker
}

void EngineCore::exitLangMenu(PredictState &st) {
  st.langMenu = false;
  st.cands.clear();
  clearPanel(st);
}

void EngineCore::applyLangChoice(PredictState &st, int idx) {
  int n = 0;
  const LangChoice *choices = langChoices(n);
  if (idx >= 0 && idx < n)
    writeLang(choices[idx].value); // le daemon recharge à chaud
  exitLangMenu(st);
  // La barre repart dans la nouvelle langue.
  showNextWord(st);
}

void EngineCore::enterReformulation(PredictState &st) {
  Surrounding s;
  bool valid = fe_.surrounding(s);
  // Texte à reformuler :
  //  - sélection rapportée (souris) → la sélection ;
  //  - sinon, texte environnant court (Ctrl+A que l'app n'expose pas comme
  //    sélection) → TOUT le champ.
  std::string sentence;
  if (valid && !s.selected.empty()) {
    sentence = s.selected;
  } else if (valid && !s.text.empty() && decodeUtf8(s.text).size() <= 400) {
    sentence = s.text;
  }
  if (sentence.empty()) {
    // FEEDBACK au lieu d'un no-op silencieux.
    st.reformulating = true;
    showReformNotice(st, "no_text");
    return;
  }
  st.reformulating = true;
  st.reformText = sentence;
  st.reformFromSelection = valid && !s.selected.empty();
  st.reformMode = prefs_.lastReformMode;
  st.reformNonce = 0;
  st.reformSource.clear();
  runReformulation(st);
}

void EngineCore::runReformulation(PredictState &st) {
  st.reformLoading = true;
  st.navigating = false;
  st.navIndex = 0;
  st.cands = {"⟳ Reformulation…"};
  setReformulationCandidates(st);

  uint32_t gen = ++st.reformGen;
  prefs_.lastReformMode = st.reformMode;
  int nm = 0;
  const ReformMode *modes = reformModes(nm);
  if (runReform_)
    runReform_(st.reformText, modes[st.reformMode].key, st.reformNonce,
               engineCfg().reformCount, gen);
}

void EngineCore::onReformPartial(PredictState &st,
                                 std::vector<std::string> vars, uint32_t gen) {
  if (!st.reformulating || st.reformGen != gen || vars.empty())
    return;
  bool first = st.reformLoading;
  st.reformLoading = false;
  st.cands = std::move(vars);
  if (first) {
    st.navIndex = 0;
    st.navigating = true;
  }
  if (st.navIndex >= (int)st.cands.size())
    st.navIndex = 0;
  setReformulationCandidates(st);
}

void EngineCore::onReformResult(PredictState &st, const ReformResult &r,
                                uint32_t gen) {
  if (!st.reformulating || st.reformGen != gen)
    return; // annulé ou résultat obsolète (mode changé entre-temps)
  bool wasLoading = st.reformLoading;
  st.reformLoading = false;
  if (r.variants.empty()) {
    if (r.error == "superseded")
      return; // une demande plus récente est en route
    showReformNotice(st, r.error);
    return;
  }
  st.cands = r.variants;
  st.reformSource = r.source;
  if (wasLoading || st.navIndex >= (int)st.cands.size())
    st.navIndex = 0;
  st.navigating = true;
  setReformulationCandidates(st);
}

void EngineCore::exitReformulation(PredictState &st) {
  st.reformulating = false;
  st.reformLoading = false;
  st.navigating = false;
  st.reformText.clear();
  st.reformSource.clear();
  st.reformNotice.clear();
  // NB : on NE touche PAS à reformRevert* — le revert doit survivre au commit.
  st.cands.clear();
  clearPanel(st);
}

void EngineCore::commitReformulation(PredictState &st, int idx) {
  // Sélection RAPPORTÉE : commitText suffit (commit-over-selection).
  // Repli « champ entier » : commitText seul INSÉRERAIT la variante en plus du
  // texte — on supprime d'abord TOUT le champ.
  if (idx >= 0 && idx < (int)st.cands.size()) {
    const std::string variant = st.cands[idx];
    if (!st.reformFromSelection) {
      Surrounding s;
      if (fe_.surrounding(s)) {
        auto cps = decodeUtf8(s.text);
        size_t cur = (std::min)(s.cursor, cps.size());
        if (!cps.empty())
          fe_.deleteRange((unsigned)cur, (unsigned)cps.size());
      }
    }
    fe_.commitText(variant);
    // arme le REVERT : Backspace juste après restaure le texte original.
    st.reformRevertOrig = st.reformText;
    st.reformRevertCps = (uint32_t)decodeUtf8(variant).size();
  }
  exitReformulation(st);
}

// ============================================================ keyEvent =======

// (L) PANNEAU DE LANGUE. Toute touche gérée est CONSOMMÉE : sinon le frontal
// la réinjecte dans l'application.
bool EngineCore::handleLangMenu(PredictState &st, const KeyEvent &k,
                                bool &consumed) {
  int n = 0;
  langChoices(n);
  if (k.key == Key::Escape) {
    exitLangMenu(st);
    consumed = true;
    return true;
  }
  if (!k.mod() && k.panelDigit >= 0 && k.panelDigit < n) {
    applyLangChoice(st, k.panelDigit);
    consumed = true;
    return true;
  }
  // ←/→/Tab (et Ctrl+Shift+L à nouveau) : déplacer le surlignage.
  bool again = (k.cp == 'l' || k.cp == 'L') && k.ctrl && k.shift;
  if ((!k.mod() && (k.key == Key::Right || k.key == Key::Tab)) || again) {
    st.langIndex = (st.langIndex + 1) % n;
    setLangMenuCandidates(st);
    consumed = true;
    return true;
  }
  if (!k.mod() && (k.key == Key::Left || k.key == Key::ShiftTab)) {
    st.langIndex = (st.langIndex - 1 + n) % n;
    setLangMenuCandidates(st);
    consumed = true;
    return true;
  }
  if (!k.mod() && (k.key == Key::Enter || k.key == Key::Space)) {
    applyLangChoice(st, st.langIndex);
    consumed = true;
    return true;
  }
  exitLangMenu(st); // autre touche → on sort et on continue
  return false;
}

// (R) MODE REFORMULATION.
bool EngineCore::handleReformulation(PredictState &st, const KeyEvent &k,
                                     bool &consumed) {
  if (k.key == Key::Escape) {
    exitReformulation(st);
    consumed = true;
    return true;
  }
  if (st.reformLoading) { // génération en cours → on avale tout (sauf Échap)
    consumed = true;
    return true;
  }
  // RÉGÉNÉRER : re-presser Ctrl+Alt+R → nouvelles variantes (même mode).
  if (k.ctrl && k.alt && (k.cp == 'r' || k.cp == 'R')) {
    st.reformNonce++;
    runReformulation(st);
    consumed = true;
    return true;
  }
  int n = (int)st.cands.size();
  if (!k.mod() && k.panelDigit >= 0 && k.panelDigit < n) {
    commitReformulation(st, k.panelDigit);
    consumed = true;
    return true;
  }
  // Raccourcis DIRECTS de mode : r/f/s/c/t sautent au mode.
  if (!k.mod() && k.cp) {
    uint32_t lc = k.cp | 0x20; // tolère la majuscule
    const char *key = lc == 'r'   ? "rephrase"
                      : lc == 'f' ? "formal"
                      : lc == 's' ? "simple"
                      : lc == 'c' ? "correct"
                      : lc == 't' ? "translate"
                                  : nullptr;
    if (key) {
      int nm = 0;
      const ReformMode *modes = reformModes(nm);
      for (int m = 0; m < nm; m++)
        if (std::string(modes[m].key) == key && m != st.reformMode) {
          st.reformMode = m;
          st.reformNonce = 0; // nouveau mode → repart du 1er tirage
          runReformulation(st);
          break;
        }
      consumed = true;
      return true;
    }
  }
  // ←/→ : changer de MODE (régénère dans le nouveau mode).
  if (!k.mod() && (k.key == Key::Right || k.key == Key::Left)) {
    int nm = 0;
    reformModes(nm);
    int d = (k.key == Key::Right) ? 1 : -1;
    st.reformMode = (st.reformMode + d + nm) % nm;
    st.reformNonce = 0;
    runReformulation(st);
    consumed = true;
    return true;
  }
  // bulle VERTICALE → ↓/Tab = variante suivante, ↑/⇧Tab = précédente.
  if (!k.mod() && (k.key == Key::Tab || k.key == Key::Down) && n) {
    st.navIndex = (st.navIndex + 1) % n;
    setReformulationCandidates(st);
    consumed = true;
    return true;
  }
  if (!k.mod() && (k.key == Key::ShiftTab || k.key == Key::Up) && n) {
    st.navIndex = (st.navIndex - 1 + n) % n;
    setReformulationCandidates(st);
    consumed = true;
    return true;
  }
  if (!k.mod() && k.key == Key::Enter) {
    commitReformulation(st, st.navIndex);
    consumed = true;
    return true;
  }
  exitReformulation(st); // autre touche → on sort et on continue
  return false;
}

bool EngineCore::keyEvent(PredictState &st, const KeyEvent &k) {
  // Champs mot de passe / sensibles : aucune prédiction, aucune préédition.
  if (fe_.isPasswordField())
    return false;

  if (st.langMenu) {
    bool consumed = false;
    if (handleLangMenu(st, k, consumed))
      return consumed;
  }

  // (R-notice) PANNEAU D'ÉCHEC de reformulation : Entrée ouvre le dialogue de
  // clé quand c'est une affaire de clé ; Échap/Entrée sont avalées, toute
  // autre touche ferme le panneau puis continue sa vie normale.
  if (st.reformulating && !st.reformNotice.empty()) {
    bool keyIssue = st.reformNotice == "no_key" || st.reformNotice == "auth";
    bool enter = !k.mod() && k.key == Key::Enter;
    if (keyIssue && enter)
      fe_.openKeyDialog();
    exitReformulation(st);
    if (enter || k.key == Key::Escape)
      return true;
    // fall-through : la touche est traitée normalement ci-dessous
  }

  if (st.reformulating) {
    bool consumed = false;
    if (handleReformulation(st, k, consumed))
      return consumed;
  }

  // (R-trig) DÉCLENCHEUR : Ctrl+Alt+R sur une SÉLECTION → variantes.
  if (k.ctrl && k.alt && (k.cp == 'r' || k.cp == 'R')) {
    enterReformulation(st);
    return true; // consomme le raccourci dans tous les cas
  }

  // (0-bis) REVERT REFORMULATION : Backspace IMMÉDIATEMENT après avoir choisi
  // une reformulation restaure le texte original. One-shot, façon undo.
  uint32_t reformRevCps = st.reformRevertCps;
  st.reformRevertCps = 0;
  if (k.key == Key::Backspace && !k.mod() && st.buffer.empty() &&
      reformRevCps > 0) {
    Surrounding s;
    if (fe_.surrounding(s)) {
      fe_.deleteBefore(reformRevCps);
      fe_.commitText(st.reformRevertOrig);
      return true;
    }
  }

  // (0-emoji) Super+; : ouvre / referme le PICKER EMOJI (cf toggleEmojiPicker).
  //      Remplace l'ancien déclencheur ':' tapé, qui redevient un caractère
  //      normal (« 10:30 », « voici : »). Sur AZERTY, ';' est en Shift+, → le
  //      caractère peut remonter en ':' ; on accepte les deux.
  if (k.super && (k.cp == ';' || k.cp == ':')) {
    toggleEmojiPicker(st);
    return true;
  }

  // (0-) Ctrl+Shift+L : ouvre le PANNEAU DE LANGUE.
  if ((k.cp == 'l' || k.cp == 'L') && k.ctrl && k.shift) {
    enterLangMenu(st);
    return true;
  }

  // (0) Fenêtre de REVERT : Backspace IMMÉDIATEMENT après une auto-application
  // efface le mot appliqué, restaure le littéral tapé et ré-ouvre la
  // composition — et le prochain Espace gardera le littéral (vetoAuto).
  uint32_t autoCps = st.lastAutoCps;
  st.lastAutoCps = 0; // la fenêtre ne dure qu'une touche
  if (k.key == Key::Backspace && !k.mod() && st.buffer.empty() && autoCps > 0) {
    Surrounding s;
    if (fe_.surrounding(s)) {
      deleteSurroundingBefore(st, autoCps);
      st.buffer = st.lastAutoLit;
      st.vetoAuto = true;
      // veto PERSISTANT : cette paire tapé→appliqué ne sera plus jamais
      // auto-appliquée (le daemon la journalise).
      vetoDaemon(st.lastAutoLit, st.lastAutoWord);
      if (!st.ctx.empty())
        st.ctx.pop_back(); // le mot remplacé n'est plus dans le texte
      st.navigating = false;
      updateCompletion(st);
      return true;
    }
  }

  // (1) Caractère de mot (sans Ctrl/Alt/Super) → prolonge le buffer.
  //     Exception : en NAVIGATION, les chiffres 1-6 sélectionnent directement.
  if (!k.mod() && k.cp && isWordExtender(k.cp, st.buffer.empty())) {
    if (st.navigating && k.cp >= '1' && k.cp <= '6' &&
        int(k.cp - '1') < (int)st.cands.size()) {
      commitWord(st, candOf(st, int(k.cp - '1')), /*trailingSpace=*/true);
      return true;
    }
    appendCp(st.buffer, k.cp);
    st.navigating = false;
    st.erasing = false; // retaper réarme fantôme + auto-application
    updateCompletion(st);
    return true;
  }

  // (1bis) Ctrl+Backspace pendant la composition : ABANDONNE le mot en cours
  //        (rien n'est committé ni appris).
  if (k.key == Key::Backspace && k.ctrl && !st.buffer.empty()) {
    st.buffer.clear();
    st.navigating = false;
    clearPanel(st);
    return true;
  }

  // (2) Backspace pendant la composition → édite le buffer.
  if (k.key == Key::Backspace && !k.mod() && !st.buffer.empty()) {
    popLastCp(st.buffer);
    st.navigating = false;
    st.erasing = true; // cf PredictState::erasing — effacer n'applique pas
    if (st.buffer.empty())
      clearPanel(st);
    else
      updateCompletion(st);
    return true;
  }

  // (2bis) Backspace AVEC BUFFER VIDE : on supprime du texte DÉJÀ committé.
  //        Si la suppression « rentre » dans un mot, on RECOMPOSE : le mot
  //        repasse en préedit et la barre revient, contexte intact.
  if (k.key == Key::Backspace && !k.mod() && st.buffer.empty()) {
    Surrounding s;
    if (fe_.surrounding(s)) {
      auto cps = decodeUtf8(s.text);
      size_t cur = s.cursor;
      // pas de sélection, curseur en FIN de mot (jamais en plein milieu —
      // recomposer la moitié gauche corromprait le texte au commit suivant)
      if (s.anchor == cur && cur > 0 && cur <= cps.size() &&
          (cur == cps.size() || !isWordCp(cps[cur]))) {
        size_t end = cur - 1; // état après le Backspace simulé
        size_t start = end;
        while (start > 0 && isWordCp(cps[start - 1]))
          --start;
        bool hasLetter = false;
        for (size_t i = start; i < end; i++)
          hasLetter = hasLetter || isLetterCp(cps[i]);
        // cap à 32 cp — un token géant (URL…) ne se recompose pas
        if (hasLetter && end - start <= 32) {
          std::string word;
          for (size_t i = start; i < end; i++)
            appendCp(word, cps[i]);
          deleteSurroundingBefore(st, unsigned(cur - start));
          st.buffer = word;
          // le mot recomposé n'est plus committé — mais seulement s'il est
          // bien le dernier du contexte (on peut backspacer un VIEUX mot)
          if (!st.ctx.empty() && st.ctx.back().rfind(word, 0) == 0)
            st.ctx.pop_back();
          st.navigating = false;
          // Reculer sur un mot committé est un geste de CORRECTION : le mot
          // revient tel quel, sans fantôme et sans auto-application (sinon
          // effacer l'espace après « salut » rendait « salutation »).
          st.erasing = true;
          updateCompletion(st);
          return true;
        }
      }
    }
  }
  // Sinon : Backspace simple. La barre mot-suivant est spéculative : on la
  // FERME et on laisse la touche filer à l'application.
  if (k.key == Key::Backspace && st.buffer.empty()) {
    st.navigating = false;
    if (fe_.hasCandidates())
      clearPanel(st);
    return false;
  }

  // (3) Composition active (buffer non vide). Les branches dédiées exigent
  //     « sans modificateur » : Ctrl+Tab, Ctrl+Entrée… committent le littéral
  //     et FILENT à l'application.
  if (!st.buffer.empty()) {
    // Picker emoji : la barre est une GRILLE paginée (8 × 3 par page). HORS
    // grille, ↑/↓/Début/Fin/PgUp/PgDn ne sont pas capturés.
    bool emojiGrid = isEmojiBuffer(st.buffer);
    if (!k.mod() && k.key == Key::Tab) {
      navigate(st, +1);
      return true;
    }
    if (!k.mod() && k.key == Key::ShiftTab) {
      navigate(st, -1); // ⇧Tab : entre par la DROITE de la barre
      return true;
    }
    // ↑/↓ : une LIGNE. Au bord de la grille on ne s'arrête pas, on TOURNE LA
    // PAGE (colonne conservée) — c'est le « scroll » du picker.
    if (!k.mod() && emojiGrid && (k.key == Key::Down || k.key == Key::Up)) {
      bool down = k.key == Key::Down;
      int col = st.navigating ? st.navIndex % kGridCols : 0;
      int next = st.navigating ? st.navIndex + (down ? kGridCols : -kGridCols)
                               : 0;
      if (!st.navigating || (next >= 0 && next < pageCount(st)))
        navigate(st, st.navigating ? (down ? kGridCols : -kGridCols) : 0,
                 /*clamp=*/true);
      else if (!turnPage(st, down ? +1 : -1, col))
        navigate(st, down ? kGridCols : -kGridCols, /*clamp=*/true);
      return true;
    }
    // Page suivante / précédente en un coup.
    if (!k.mod() && emojiGrid &&
        (k.key == Key::PageDown || k.key == Key::PageUp)) {
      turnPage(st, k.key == Key::PageDown ? +1 : -1,
               st.navigating ? st.navIndex % kGridCols : 0);
      return true;
    }
    // Début/Fin : extrémités ABSOLUES de la grille (toutes pages).
    if (!k.mod() && emojiGrid && (k.key == Key::Home || k.key == Key::End)) {
      bool home = k.key == Key::Home;
      st.pageStart =
          home || st.cands.empty()
              ? 0
              : (st.cands.size() - 1) / kGridPage * kGridPage;
      fe_.setPanelQuery(st.buffer, pageIndicator(st));
      navigateTo(st, home ? 0 : pageCount(st) - 1);
      return true;
    }
    // ←/→ naviguent aussi ; en GRILLE emoji ils ENTRENT directement (sans
    // Tab d'abord — sinon ils committaient le littéral) et débordent de page
    // en page.
    if (!k.mod() && (st.navigating || emojiGrid) &&
        (k.key == Key::Left || k.key == Key::Right)) {
      bool right = k.key == Key::Right;
      int next = st.navIndex + (right ? +1 : -1);
      if (emojiGrid && st.navigating && (next < 0 || next >= pageCount(st)) &&
          turnPage(st, right ? +1 : -1, right ? 0 : kGridCols - 1))
        navigateTo(st, right ? 0 : pageCount(st) - 1); // bord opposé
      else
        navigate(st, right ? +1 : -1, /*clamp=*/emojiGrid);
      return true;
    }
    // → ACCEPTE le texte fantôme (accept explicite, façon Copilot/fish) :
    // committe la complétion SANS espace — la frappe continue naturellement.
    if (!k.mod() && k.key == Key::Right && !st.navigating && ghostShown(st)) {
      commitWord(st, st.ghost, /*trailingSpace=*/false);
      return true;
    }
    if (!k.mod() && k.key == Key::Space) {
      std::string lit = st.buffer;
      std::string chosen = chooseOnSpace(st);
      bool autoApplied = !st.navigating && chosen != lit;
      std::string committed = applyCase(chosen, lit);
      commitWord(st, chosen, /*trailingSpace=*/true);
      if (autoApplied) { // arme la fenêtre de revert (cf (0))
        st.lastAutoLit = lit;
        st.lastAutoWord = committed;
        st.lastAutoCps = uint32_t(decodeUtf8(committed).size()) + 1;
      }
      return true;
    }
    if (!k.mod() && k.key == Key::Enter) {
      if (st.navigating) {
        commitWord(st, highlighted(st), /*trailingSpace=*/false);
        return true; // suggestion prise → on avale Entrée
      }
      if (isTriggerBuffer(st.buffer) && !st.cands.empty() &&
          (st.buffer.size() > 1 || isEmojiBuffer(st.buffer))) {
        // Picker emoji (même SANS requête : la grille montre les favoris) et
        // snippet AVEC requête : Entrée prend le 1er candidat de la page. Le
        // snippet nu (';' seul) garde le littéral — Entrée = retour-ligne.
        commitWord(st, candOf(st, 0), /*trailingSpace=*/false);
        return true;
      }
      commitWord(st, st.buffer, /*trailingSpace=*/false);
      // littéral validé → on LAISSE passer Entrée (retour-ligne / envoi).
      return false;
    }
    if (!k.mod() && k.key == Key::Escape) {
      // Échap ANNULE la suggestion : committe le littéral tel quel (on ne perd
      // jamais la frappe) — SANS apprendre le fragment.
      commitWord(st, st.buffer, /*trailingSpace=*/false, /*learn=*/false);
      return !engineCfg().escapeForward;
    }
    // toute autre touche : termine le mot SANS espace puis laisse la touche
    // filer. La PONCTUATION applique la même correction que l'Espace
    // (« teh. » → « the. ») — sauf après un déclencheur.
    bool punctFix = !k.mod() &&
                    (k.cp == '.' || k.cp == ',' || k.cp == ';' || k.cp == ':' ||
                     k.cp == '!' || k.cp == '?') &&
                    !isTriggerBuffer(st.buffer);
    bool fr = engineCfg().frenchSpacing && !k.mod() &&
              !isTriggerBuffer(st.buffer);
    // guillemet OUVRANT « : committer le mot, puis « + fine insécable, et
    // AVALER la touche (sinon « arriverait APRÈS la fine).
    if (fr && k.cp == 0x00AB) {
      commitWord(st, st.buffer, /*trailingSpace=*/false);
      fe_.commitText(kOpenGuillemetThin);
      return true;
    }
    commitWord(st, punctFix ? chooseOnSpace(st) : st.buffer,
               /*trailingSpace=*/false);
    if (fr && needsFrenchThinBefore(k.cp))
      fe_.commitText(kThinNbsp);
    if (k.cp == '.' || k.cp == '!' || k.cp == '?')
      st.ctx.clear(); // fin de phrase → on repart à neuf
    return false;
  }

  // (4) Buffer vide. La barre de mot-suivant est purement informative tant
  //     qu'on n'appuie pas sur Tab : tout le reste passe à l'application.
  if (k.mod())
    return false; // raccourcis (Ctrl+C…) intacts

  bool hasList = fe_.hasCandidates();
  if (hasList && k.key == Key::Escape) {
    st.navigating = false;
    clearPanel(st);
    return !engineCfg().escapeForward;
  }
  if (st.navigating && hasList) {
    // barre horizontale : Tab/→ et ⇧Tab/← naviguent ; ↑/↓ sortent.
    if (k.key == Key::Tab || k.key == Key::Right) {
      navigate(st, +1);
      return true;
    }
    if (k.key == Key::ShiftTab || k.key == Key::Left) {
      navigate(st, -1);
      return true;
    }
    if (k.cp >= '1' && k.cp <= '6' &&
        int(k.cp - '1') < (int)st.cands.size()) {
      commitWord(st, candOf(st, int(k.cp - '1')), /*trailingSpace=*/true);
      return true;
    }
    if (k.key == Key::Space || k.key == Key::Enter) {
      commitWord(st, highlighted(st), /*trailingSpace=*/true);
      return true;
    }
    // toute autre touche annule la navigation et file à l'appli.
    st.navigating = false;
    clearPanel(st);
    return false;
  }
  if (hasList && (k.key == Key::Tab || k.key == Key::ShiftTab)) {
    // Tab entre par la gauche, ⇧Tab par la DROITE de la barre.
    navigate(st, k.key == Key::ShiftTab ? -1 : 0);
    return true;
  }
  // pas de composition, pas de navigation : on efface la barre éphémère et on
  // laisse la touche agir normalement.
  if (hasList)
    clearPanel(st);
  if (k.cp == '.' || k.cp == '!' || k.cp == '?')
    st.ctx.clear();
  // typographie française (opt-in) AUSSI quand le buffer est vide.
  if (engineCfg().frenchSpacing) {
    if (k.cp == 0x00AB) {
      fe_.commitText(kOpenGuillemetThin);
      return true;
    }
    if (needsFrenchThinBefore(k.cp))
      frenchThinBefore(st);
  }
  // Espace sur buffer vide : la touche file à l'appli ET on affiche la barre.
  if (k.key == Key::Space)
    showNextWord(st);
  return false;
}

void EngineCore::reset(PredictState &st) {
  // Ne jamais perdre ce qui est tapé : un mot en cours est committé tel quel.
  // SAUF le picker emoji : sa requête n'est pas du texte (le ':' vient du
  // raccourci) — un picker abandonné se ferme sans rien écrire.
  bool wasPicker = isEmojiBuffer(st.buffer);
  if (!st.buffer.empty() && !wasPicker)
    fe_.commitText(st.buffer);
  st.buffer.clear();
  st.ctx.clear();
  st.cands.clear();
  st.navigating = false;
  st.vetoAuto = false;
  st.erasing = false;
  st.lastAutoCps = 0;
  st.lastAutoLit.clear();
  st.ghost.clear();
  st.accentOnly = false;
  st.pageStart = 0;
  st.nextWordGen++; // un refresh neural en vol devient périmé
  fe_.stopWatch();
  clearPanel(st);
  // Picker abandonné (focus perdu, bascule manuelle…) : l'adaptateur rend la
  // méthode d'entrée qu'il avait empruntée.
  if (wasPicker)
    fe_.pickerClosed();
}

} // namespace core
