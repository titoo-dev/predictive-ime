// Track A — engine fcitx5 (InputMethodEngineV2) : ADAPTATEUR.
//
// Toute la logique de saisie vit désormais dans core/ (machine à états,
// protocole daemon, UTF-8, casse, typographie) et est partagée avec le text
// service TSF de Windows. Ce fichier ne fait plus que traduire :
//   fcitx::KeyEvent      → core::KeyEvent
//   core::Frontend       → appels ic->…
//   boucle d'événements  → surveillance de socket + post sur le thread principal
//
// Rien de ce qui suit ne décide d'un comportement de frappe : si une touche se
// comporte mal, c'est dans core/engine_core.cpp qu'il faut regarder.
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "../core/engine_core.h"

namespace {

// ------------------------------------------------------ liste de candidats --
// Liste MAISON : CommonCandidateList ABORT fcitx (FatalLog « invalid label
// idx ») au-delà de 10 candidats (ses labels « 1. »… « 0. » sont fixes) — la
// grille emoji en montre 24. Pas de pagination, un curseur : exactement ce que
// notre UI consomme.
class PredictCandidate : public fcitx::CandidateWord {
public:
  // `autoApply` : ce candidat sera appliqué par l'Espace — marqué en GRAS dans
  // le Text fcitx, l'UI le rend distinctement.
  explicit PredictCandidate(std::string text, bool autoApply = false)
      : text_(std::move(text)) {
    fcitx::Text t;
    t.append(text_, autoApply
                        ? fcitx::TextFormatFlags{fcitx::TextFormatFlag::Bold}
                        : fcitx::TextFormatFlags{});
    setText(std::move(t));
  }
  void select(fcitx::InputContext *) const override {} // géré à part
  const std::string &word() const { return text_; }

private:
  std::string text_;
};

class PredictCandidateList : public fcitx::CandidateList {
public:
  void append(std::string text, bool autoApply) {
    cands_.push_back(
        std::make_unique<PredictCandidate>(std::move(text), autoApply));
    labels_.emplace_back(); // pas de label (chips de mots/emoji)
  }
  // Candidat AVEC label (numéro) : mode reformulation → l'UI passe en liste
  // verticale numérotée (le label non vide est le signal lu par qmlui).
  void appendLabeled(std::string text, const std::string &label) {
    cands_.push_back(std::make_unique<PredictCandidate>(std::move(text),
                                                       /*autoApply=*/false));
    fcitx::Text t;
    t.append(label);
    labels_.push_back(std::move(t));
  }
  void setCursorIndex(int i) { cursor_ = i; }

  const fcitx::Text &label(int idx) const override {
    return (idx >= 0 && idx < (int)labels_.size()) ? labels_[idx] : emptyLabel_;
  }
  const fcitx::CandidateWord &candidate(int idx) const override {
    return *cands_[idx];
  }
  int size() const override { return (int)cands_.size(); }
  int cursorIndex() const override { return cursor_; }
  fcitx::CandidateLayoutHint layoutHint() const override {
    return fcitx::CandidateLayoutHint::Horizontal;
  }

private:
  std::vector<std::unique_ptr<PredictCandidate>> cands_;
  std::vector<fcitx::Text> labels_;
  fcitx::Text emptyLabel_;
  int cursor_ = -1;
};

// État par contexte d'entrée : l'état du cœur, plus ce qui ne regarde que
// fcitx5.
struct PredictStateProp : public fcitx::InputContextProperty {
  core::PredictState st;
  // Ce client a-t-il PUBLIÉ un texte environnant depuis qu'il a le focus ?
  // La capacité annoncée ne suffit pas : le cache de fcitx peut dater d'un
  // AUTRE contexte, et un client qui n'implémente pas set_surrounding_text
  // (ghostty et les terminaux GTK4 en général) n'a rien en face — une
  // suppression le fait déréférencer NULL et il MEURT. Cf surrounding().
  bool sawSurrounding = false;
  // Méthode d'entrée d'AVANT l'ouverture du picker par raccourci (vide si on
  // n'a rien basculé). Le picker emprunte l'IME le temps de choisir un emoji,
  // puis on rend la main : sans ça, ouvrir le picker allumait la PRÉDICTION
  // de texte pour la suite de la frappe.
  std::string imBeforePicker;
};

// Chiffre « physique » 0-based d'une touche pour la SÉLECTION dans les panneaux
// MODAUX : '1'-'9' directs, sinon la rangée AZERTY non shiftée (&é"'(-è_ç — sur
// AZERTY les chiffres exigent Shift, et « autre touche » fermait le panneau EN
// SILENCE), sinon le keycode évdev 10-18 (rangée physique, indépendant de la
// disposition). Ne PAS utiliser pendant la composition : é/è/ç/- y sont des
// lettres.
int panelDigit(const fcitx::Key &key, uint32_t cp) {
  if (cp >= '1' && cp <= '9')
    return int(cp - '1');
  static const uint32_t az[] = {'&', 0xE9, '"', '\'', '(', '-', 0xE8, '_', 0xE7};
  for (int i = 0; i < 9; i++)
    if (cp && cp == az[i])
      return i;
  if (key.code() >= 10 && key.code() <= 18)
    return key.code() - 10;
  return -1;
}

core::Key mapKey(fcitx::KeySym sym) {
  switch (sym) {
  case FcitxKey_Escape:
    return core::Key::Escape;
  case FcitxKey_BackSpace:
    return core::Key::Backspace;
  case FcitxKey_Tab:
    return core::Key::Tab;
  case FcitxKey_ISO_Left_Tab:
    return core::Key::ShiftTab;
  case FcitxKey_Return:
  case FcitxKey_KP_Enter:
    return core::Key::Enter;
  case FcitxKey_space:
    return core::Key::Space;
  case FcitxKey_Left:
    return core::Key::Left;
  case FcitxKey_Right:
    return core::Key::Right;
  case FcitxKey_Up:
    return core::Key::Up;
  case FcitxKey_Down:
    return core::Key::Down;
  case FcitxKey_Home:
    return core::Key::Home;
  case FcitxKey_End:
    return core::Key::End;
  case FcitxKey_Page_Up:
    return core::Key::PageUp;
  case FcitxKey_Page_Down:
    return core::Key::PageDown;
  default:
    return core::Key::None;
  }
}

// Dialogue de clé API : la fenêtre doit sortir du process de l'IME (la saisie
// ne peut pas se faire dans le panneau lui-même). Double fork : pas de zombie,
// le dialogue survit à l'engine.
void spawnKeyDialog() {
  pid_t pid = ::fork();
  if (pid == 0) {
    if (::fork() == 0) {
      ::setsid();
      ::execlp("ime-preferences", "ime-preferences", "--groq-key",
               (char *)nullptr);
      ::_exit(127);
    }
    ::_exit(0);
  }
  if (pid > 0)
    ::waitpid(pid, nullptr, 0);
}

} // namespace

class PredictEngine;

// ------------------------------------------------------------- Frontend ----
// Lié à UN contexte d'entrée, recréé à chaque événement : les verbes qui
// touchent au document sont par-contexte, ceux qui touchent à l'asynchrone
// délèguent au moteur (une seule surveillance à la fois).
class FcitxFrontend : public core::Frontend {
public:
  FcitxFrontend(PredictEngine *eng, fcitx::InputContext *ic)
      : eng_(eng), ic_(ic) {}

  void commitText(const std::string &utf8) override { ic_->commitString(utf8); }

  void setPreedit(const std::string &typed) override {
    fcitx::Text preedit;
    if (!typed.empty())
      preedit.append(typed,
                     fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
    preedit.setCursor(typed.size());
    ic_->inputPanel().setClientPreedit(preedit);
    ic_->updatePreedit();
  }

  void setCandidates(const std::vector<core::Candidate> &cands, int cursor,
                     const std::string &auxTitle) override {
    auto list = std::make_unique<PredictCandidateList>();
    for (const auto &c : cands) {
      if (c.label.empty())
        list->append(c.text, c.autoApply);
      else
        list->appendLabeled(c.text, c.label);
    }
    list->setCursorIndex(cursor);
    // On ne réinitialise PAS le panneau entier : la préédition posée juste
    // avant doit survivre (setClientPreedit vit dans le même InputPanel).
    ic_->inputPanel().setCandidateList(std::move(list));
    if (!auxTitle.empty())
      ic_->inputPanel().setAuxUp(fcitx::Text(auxTitle));
    ic_->updatePreedit();
    ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  void clearPanel() override {
    ic_->inputPanel().reset();
    ic_->updatePreedit();
    ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
  }

  bool hasCandidates() const override {
    auto list = ic_->inputPanel().candidateList();
    return list && list->size() > 0;
  }

  // Le texte environnant de CE client est-il utilisable (lecture du contexte,
  // et surtout SUPPRESSION de texte déjà écrit) ? La capacité annoncée ne
  // suffit PAS : un terminal GTK4 (ghostty) active text-input-v3 sans jamais
  // appeler set_surrounding_text — son tampon GTK reste NULL, mais le cache de
  // fcitx peut contenir le texte d'un AUTRE contexte. La suppression passait
  // alors la validation de fcitx et text_input_delete_surrounding_text
  // déréférençait NULL → SIGSEGV DU CLIENT. On exige la seule preuve fiable :
  // ce client a publié un texte environnant depuis qu'il a le focus.
  bool surrounding(core::Surrounding &out) override {
    if (!sawSurrounding() ||
        !ic_->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) ||
        !ic_->surroundingText().isValid())
      return false;
    const auto &st = ic_->surroundingText();
    out.text = st.text();
    out.cursor = st.cursor();
    out.anchor = st.anchor();
    out.selected = st.selectedText();
    return true;
  }

  // Supprime ET répercute sur la copie LOCALE du texte environnant : l'app ne
  // renvoie son update qu'après un aller-retour, et une complétion relancée
  // juste derrière lirait sinon l'ancien texte (le mot supprimé polluerait son
  // propre contexte).
  //
  // Les bornes sont vérifiées AVANT d'envoyer quoi que ce soit : demander plus
  // que ce que le client détient fait déréférencer hors chaîne côté GTK4
  // (text_input_delete_surrounding_text → g_utf8_pointer_to_offset) et TUE le
  // client. L'ordre était inversé : la vérification ne protégeait que la copie
  // locale pendant que la requête partait quand même.
  void deleteBefore(unsigned n) override {
    auto &st = ic_->surroundingText();
    auto cps = core::decodeUtf8(st.text());
    size_t cur = st.cursor();
    if (n == 0 || n > cur || cur > cps.size())
      return;
    ic_->deleteSurroundingText(-int(n), n);
    std::string out;
    for (size_t i = 0; i < cps.size(); i++)
      if (i < cur - n || i >= cur)
        core::appendCp(out, cps[i]);
    st.setText(out, cur - n, cur - n);
  }

  void deleteRange(unsigned back, unsigned count) override {
    ic_->deleteSurroundingText(-int(back), count);
    ic_->surroundingText().setText("", 0, 0);
  }

  std::string program() override { return ic_->program(); }

  bool isPasswordField() override {
    return ic_->capabilityFlags().test(fcitx::CapabilityFlag::Password) ||
           ic_->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive);
  }

  // Picker emoji : la requête va dans le préedit du PANNEAU — la barre QML en
  // fait un champ de recherche (et y lit le mode grille), l'UI fcitx classique
  // l'affiche au-dessus des candidats. Rien dans l'application, sauf le ping
  // de caret encore en vol : la première lettre tapée l'effacerait avant que
  // le client n'ait publié sa position, et le panneau resterait au coin.
  void setPanelQuery(const std::string &query,
                     const std::string &page) override;
  void pingCaret() override;
  void pickerClosed() override;

  void watchReadable(sock_t fd, std::function<void()> cb) override;
  void stopWatch() override;
  void postToMain(std::function<void()> fn) override;
  void openKeyDialog() override { spawnKeyDialog(); }

private:
  bool sawSurrounding();

  PredictEngine *eng_;
  fcitx::InputContext *ic_;
};

// --------------------------------------------------------------- moteur ----
class PredictEngine : public fcitx::InputMethodEngineV2 {
public:
  explicit PredictEngine(fcitx::Instance *instance)
      : instance_(instance),
        factory_([](fcitx::InputContext &) { return new PredictStateProp; }) {
    instance->inputContextManager().registerProperty("predictState", &factory_);
    // Instance::eventDispatcher() n'existe pas sur le fcitx5 des vieilles
    // distros : on attache le nôtre.
    dispatcher_.attach(&instance->eventLoop());
    // Le SEUL signal fiable qu'un client gère vraiment le texte environnant :
    // il vient d'en publier un (cf FcitxFrontend::surrounding).
    surroundingWatcher_ = instance_->watchEvent(
        fcitx::EventType::InputContextSurroundingTextUpdated,
        fcitx::EventWatcherPhase::Default, [this](fcitx::Event &event) {
          auto &e = static_cast<fcitx::InputContextEvent &>(event);
          e.inputContext()->propertyFor(&factory_)->sawSurrounding = true;
        });
    // Le picker emoji ne doit PAS dépendre de l'activation de l'IME : fcitx
    // n'envoie les touches qu'à la méthode COURANTE, donc Super+; ne faisait
    // rien tant qu'on n'avait pas basculé sur « predict ». On écoute en phase
    // PreInputMethod (avant toute méthode) : si le raccourci tombe alors
    // qu'une autre méthode est active, on EMPRUNTE predict le temps du picker.
    hotkeyWatcher_ = instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent,
        fcitx::EventWatcherPhase::PreInputMethod, [this](fcitx::Event &event) {
          auto &ke = static_cast<fcitx::KeyEvent &>(event);
          if (ke.isRelease() || !isEmojiHotkey(ke.key()))
            return;
          auto *ic = ke.inputContext();
          if (instance_->inputMethod(ic) == "predict")
            return; // notre keyEvent s'en charge (et gère la fermeture)
          auto *prop = ic->propertyFor(&factory_);
          std::string previous = instance_->inputMethod(ic);
          instance_->setCurrentInputMethod(ic, "predict", /*local=*/true);
          // la bascule passe par reset() : on ouvre APRÈS, sinon le picker
          // serait effacé dans la foulée.
          FcitxFrontend fe(this, ic);
          core::EngineCore core(fe, prefs_);
          installReformRunner(core, ic);
          core.toggleEmojiPicker(prop->st);
          prop->imBeforePicker = previous; // à rendre en refermant
          ke.filterAndAccept();
        });
  }

  // Le raccourci du picker. Sur AZERTY, ';' est en Shift+, → le keysym peut
  // remonter en ':' ; on accepte les deux.
  static bool isEmojiHotkey(const fcitx::Key &key) {
    return (key.sym() == FcitxKey_semicolon || key.sym() == FcitxKey_colon) &&
           key.states().test(fcitx::KeyState::Super);
  }

  // Rend la méthode d'entrée empruntée par le picker : une fois refermé, la
  // frappe doit retrouver EXACTEMENT ce qu'elle était (pas de prédiction de
  // texte allumée au passage). Posté : on est au milieu du traitement d'une
  // touche, basculer d'IME ici rappellerait reset() en pleine main.
  void releaseBorrowedIm(fcitx::InputContext *ic) {
    auto *prop = ic->propertyFor(&factory_);
    if (prop->imBeforePicker.empty())
      return;
    std::string previous;
    previous.swap(prop->imBeforePicker);
    postToMain(ic, [this, ic, previous]() {
      instance_->setCurrentInputMethod(ic, previous, /*local=*/true);
    });
  }

  // Espace sans chasse (U+200B) du ping, et durée de pose. 200 ms : très
  // au-delà du délai mesuré entre la préédition et le rectangle publié (~45 ms
  // sur Chrome), assez court pour qu'aucun clic ne s'y glisse.
  static constexpr const char *kCaretPing = "\xE2\x80\x8B";
  static constexpr uint64_t kCaretPingUs = 200 * 1000;

  // « Publie ton caret » — provoque le rectangle de curseur du client.
  //
  // Le compositeur place la popup du panneau AU CARET, mais seulement si le
  // client a publié son rectangle de curseur (text-input-v3,
  // set_cursor_rectangle). Les clients Chromium (Chrome, Discord, VS Code…)
  // ne le publient QUE quand une PRÉÉDITION change. Trace protocole d'un champ
  // Chrome, --enable-wayland-ime :
  //
  //   enter / enable / commit                 ← focus : AUCUN rectangle
  //   preedit_string("b") … 45 ms plus tard → set_cursor_rectangle(406,390,0,22)
  //
  // Les panneaux sans préédition client (picker emoji, menu de langue) ne
  // déclenchaient donc rien, et le compositeur ancrait la popup sur un bloc
  // de REPLI au coin du client (Hyprland : 500×500 en haut à gauche). D'où ce
  // ping : une préédition ZÉRO-LARGEUR posée le temps que le client publie sa
  // position, puis effacée — invisible, et sans saut.
  void pingCaretRect(fcitx::InputContext *ic) {
    // Une préédition REMPLACE la sélection courante : pas de ping quand du
    // texte est sélectionné — on ne détruit pas la sélection pour un détail
    // de placement.
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid() &&
        ic->surroundingText().anchor() != ic->surroundingText().cursor())
      return;
    ic->inputPanel().setClientPreedit(caretPingPreedit());
    ic->updatePreedit();
    // La composition ne doit pas SURVIVRE au ping : un client qui la valide de
    // lui-même (clic ailleurs → Chromium committe la composition en cours)
    // écrirait le caractère invisible dans le texte.
    caretPing_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kCaretPingUs, 0,
        [this, uuid = ic->uuid()](fcitx::EventSourceTime *, uint64_t) {
          caretPing_.reset();
          auto *ic = instance_->inputContextManager().findByUUID(uuid);
          // N'efface QUE notre ping : entre-temps la frappe a pu reprendre —
          // effacer le préedit du mot en cours le ferait disparaître.
          if (!ic || ic->inputPanel().clientPreedit().toString() != kCaretPing)
            return true;
          ic->inputPanel().setClientPreedit(fcitx::Text{});
          ic->updatePreedit();
          return true;
        });
  }

  bool caretPingInFlight() const { return bool(caretPing_); }

  // Le préedit du ping : l'invisible, curseur APRÈS (le client le place où il
  // placerait le caret).
  static fcitx::Text caretPingPreedit() {
    fcitx::Text t(kCaretPing);
    t.setCursor(int(std::strlen(kCaretPing)));
    return t;
  }

  void keyEvent(const fcitx::InputMethodEntry &,
                fcitx::KeyEvent &event) override {
    if (event.isRelease())
      return;
    // Touche modificatrice SEULE (Shift, Ctrl, Alt…) : on ne touche à RIEN.
    // Sans cette garde, l'appui Shift en plein mot committait le buffer et
    // désarmait la fenêtre de revert Backspace.
    if (event.key().isModifier())
      return;
    auto *ic = event.inputContext();
    auto *prop = ic->propertyFor(&factory_);
    const auto &key = event.key();
    auto sym = key.sym();
    auto states = key.states();

    core::KeyEvent k;
    k.key = mapKey(sym);
    k.cp = fcitx::Key::keySymToUnicode(sym);
    k.ctrl = states.test(fcitx::KeyState::Ctrl);
    k.alt = states.test(fcitx::KeyState::Alt);
    k.shift = states.test(fcitx::KeyState::Shift);
    k.super = states.test(fcitx::KeyState::Super);
    k.panelDigit = panelDigit(key, k.cp);

    if (::getenv("IME_DEBUG"))
      fprintf(stderr,
              "[predict] sym=0x%x cp=0x%x C=%d A=%d S=%d Su=%d buf='%s' "
              "nav=%d reform=%d\n",
              sym, k.cp, int(k.ctrl), int(k.alt), int(k.shift), int(k.super),
              prop->st.buffer.c_str(), int(prop->st.navigating),
              int(prop->st.reformulating));

    FcitxFrontend fe(this, ic);
    core::EngineCore core(fe, prefs_);
    installReformRunner(core, ic);
    if (core.keyEvent(prop->st, k))
      event.filterAndAccept();
  }

  void reset(const fcitx::InputMethodEntry &,
             fcitx::InputContextEvent &event) override {
    auto *ic = event.inputContext();
    auto *prop = ic->propertyFor(&factory_);
    FcitxFrontend fe(this, ic);
    core::EngineCore core(fe, prefs_);
    core.reset(prop->st);
    // Changement de focus : ce que le PRÉCÉDENT client avait publié ne dit
    // rien du suivant. On réexige une publication avant de toucher au texte.
    prop->sawSurrounding = false;
  }

  // --- surveillance de socket (refresh neural en deux phases) --------------
  void watchReadable(sock_t fd, std::function<void()> cb) {
    stopWatch();
    watch_.reset(); // l'ancienne source (désactivée) peut mourir ici
    watchFd_ = fd;
    watch_ = instance_->eventLoop().addIOEvent(
        int(fd), fcitx::IOEventFlags{fcitx::IOEventFlag::In},
        [cb](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
          cb();
          return true;
        });
  }

  void stopWatch() {
    if (watch_)
      watch_->setEnabled(false);
    if (oscompat::sockValid(watchFd_)) {
      oscompat::sockClose(watchFd_);
      watchFd_ = kBadSock;
    }
  }

  // Poste un travail depuis un thread de reformulation vers le thread
  // principal, en n'exécutant que si le contexte d'entrée est toujours vivant.
  // C'est ce que fait EventDispatcher::scheduleWithContext, mais celui-ci
  // n'existe que depuis fcitx5 5.1.8 : on le refait à la main pour rester
  // buildable sur le fcitx5 stock des distros plus anciennes (Ubuntu 24.04).
  void postToMain(fcitx::InputContext *ic, std::function<void()> fn) {
    auto ref = ic->watch();
    if (!ref.isValid())
      return;
    dispatcher_.schedule([ref, fn = std::move(fn)]() {
      if (ref.isValid())
        fn();
    });
  }

  fcitx::FactoryFor<PredictStateProp> &factory() { return factory_; }

private:
  // Le cœur demande une génération ; c'est l'adaptateur qui fournit le thread
  // et qui rapatrie le résultat sur le thread de saisie.
  void installReformRunner(core::EngineCore &core, fcitx::InputContext *ic) {
    auto uuid = ic->uuid();
    core.setReformRunner([this, uuid](std::string text, std::string mode,
                                      uint32_t nonce, int n, uint32_t gen) {
      std::thread([this, uuid, text, mode, nonce, n, gen]() {
        auto deliver = [this, uuid, gen](bool partial,
                                         std::vector<std::string> vars,
                                         core::ReformResult res) {
          auto *ic = instance_->inputContextManager().findByUUID(uuid);
          if (!ic)
            return;
          postToMain(ic, [this, uuid, partial, vars, res, gen]() {
            auto *ic2 = instance_->inputContextManager().findByUUID(uuid);
            if (!ic2)
              return;
            auto *prop = ic2->propertyFor(&factory_);
            FcitxFrontend fe(this, ic2);
                    core::EngineCore c(fe, prefs_);
            installReformRunner(c, ic2);
            if (partial)
              c.onReformPartial(prop->st, vars, gen);
            else
              c.onReformResult(prop->st, res, gen);
          });
        };
        auto onPartial = [&deliver](std::vector<std::string> vars) {
          deliver(true, std::move(vars), {});
        };
        core::ReformResult r =
            core::reformulateDaemon(text, mode, nonce, n, onPartial);
        deliver(false, {}, r);
      }).detach();
    });
  }

  fcitx::Instance *instance_;
  fcitx::EventDispatcher dispatcher_;
  // « Ce client vient de publier son texte environnant » (cf surrounding()).
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>
      surroundingWatcher_;
  // Super+; capté AVANT toute méthode d'entrée (picker sans activer l'IME).
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>
      hotkeyWatcher_;
  // Échéance d'effacement du ping de caret ; non nulle = ping en vol.
  std::unique_ptr<fcitx::EventSourceTime> caretPing_;
  std::unique_ptr<fcitx::EventSourceIO> watch_;
  sock_t watchFd_ = kBadSock;
  core::SessionPrefs prefs_;
  fcitx::FactoryFor<PredictStateProp> factory_;
};

bool FcitxFrontend::sawSurrounding() {
  return ic_->propertyFor(&eng_->factory())->sawSurrounding;
}

void FcitxFrontend::setPanelQuery(const std::string &query,
                                  const std::string &page) {
  fcitx::Text q(query);
  q.setCursor(int(query.size()));
  ic_->inputPanel().setPreedit(q);
  ic_->inputPanel().setClientPreedit(eng_->caretPingInFlight()
                                         ? PredictEngine::caretPingPreedit()
                                         : fcitx::Text{});
  // « 2/4 » : la barre QML le rend à droite du champ de recherche.
  ic_->inputPanel().setAuxUp(page.empty() ? fcitx::Text{} : fcitx::Text(page));
  ic_->updatePreedit();
}

void FcitxFrontend::pingCaret() { eng_->pingCaretRect(ic_); }
void FcitxFrontend::pickerClosed() { eng_->releaseBorrowedIm(ic_); }

void FcitxFrontend::watchReadable(sock_t fd, std::function<void()> cb) {
  eng_->watchReadable(fd, std::move(cb));
}
void FcitxFrontend::stopWatch() { eng_->stopWatch(); }
void FcitxFrontend::postToMain(std::function<void()> fn) {
  eng_->postToMain(ic_, std::move(fn));
}

class PredictEngineFactory : public fcitx::AddonFactory {
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
    return new PredictEngine(manager->instance());
  }
};

FCITX_ADDON_FACTORY(PredictEngineFactory)
