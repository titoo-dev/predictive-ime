// Text service TSF — l'équivalent Windows de l'addon fcitx5.
//
// Il est chargé DANS le process de chaque application qui reçoit de la frappe.
// Deux conséquences qui commandent toute la conception :
//   1. il doit rester minuscule et ne dépendre de rien (pas de Qt, pas de
//      runtime tiers) ;
//   2. il ne doit JAMAIS bloquer — un appel réseau sur le thread de frappe
//      gèlerait la saisie DANS Word. Les E/S vers le daemon sont donc bornées
//      (150 ms, cf core/daemon_client.h) et la reformulation part sur un
//      thread qui repasse par PostMessage.
#pragma once

#include "CandidateWindow.h"
#include "Guids.h"

#include "../../core/engine_core.h"
#include "../../core/state.h"

#include <ctffunc.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <msctf.h>
#include <windows.h>
#include <wrl/client.h>

namespace win {

class CTextService;

// Frontend lié à UN contexte TSF et à UNE session d'édition. Recréé à chaque
// événement : le cookie d'édition n'est valable que le temps de la session.
class TsfFrontend : public core::Frontend {
public:
  TsfFrontend(CTextService *svc, ITfContext *ctx, TfEditCookie ec)
      : svc_(svc), ctx_(ctx), ec_(ec) {}

  void commitText(const std::string &utf8) override;
  void setPreedit(const std::string &typed) override;
  void setCandidates(const std::vector<core::Candidate> &cands, int cursor,
                     const std::string &auxTitle) override;
  void clearPanel() override;
  bool hasCandidates() const override;
  // Picker emoji : la requête ne va pas dans le document. Sous Windows, Win+;
  // est de toute façon pris par le sélecteur d'emoji du système avant de
  // nous parvenir ; on garde un comportement correct si le picker s'ouvre.
  void setPanelQuery(const std::string &query,
                     const std::string &page) override;
  bool surrounding(core::Surrounding &out) override;
  void deleteBefore(unsigned cps) override;
  void deleteRange(unsigned back, unsigned count) override;
  std::string program() override;
  bool isPasswordField() override;
  void watchReadable(sock_t fd, std::function<void()> onReadable) override;
  void stopWatch() override;
  void postToMain(std::function<void()> fn) override;
  void openKeyDialog() override;

private:
  CTextService *svc_;
  ITfContext *ctx_;
  TfEditCookie ec_;
};

class CLangBarButton;

// Boîte aux lettres entre les threads de reformulation et le thread de
// saisie. Partagée (shared_ptr) : un thread peut survivre au service — il
// trouve alors `hwnd` nul et jette son résultat au lieu de toucher un objet
// détruit.
struct MainBridge {
  MainBridge() { ::InitializeCriticalSection(&lock); }
  ~MainBridge() { ::DeleteCriticalSection(&lock); }
  MainBridge(const MainBridge &) = delete;
  MainBridge &operator=(const MainBridge &) = delete;

  CRITICAL_SECTION lock;
  HWND hwnd = nullptr; // fenêtre de messages du service ; nul après sa mort
  std::vector<std::function<void()>> posted;

  // Depuis n'importe quel thread. false : le service n'existe plus.
  bool post(std::function<void()> fn);
  // Depuis le thread de saisie : coupe le lien et vide la file.
  void close();
};

class CTextService : public ITfTextInputProcessorEx,
                     public ITfThreadMgrEventSink,
                     public ITfKeyEventSink,
                     public ITfCompositionSink,
                     public ITfCompartmentEventSink,
                     public ITfTextLayoutSink {
public:
  CTextService();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfTextInputProcessor / Ex
  STDMETHODIMP Activate(ITfThreadMgr *tm, TfClientId cid) override;
  STDMETHODIMP Deactivate() override;
  STDMETHODIMP ActivateEx(ITfThreadMgr *tm, TfClientId cid, DWORD flags) override;

  // ITfThreadMgrEventSink
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr *) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr *) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr *, ITfDocumentMgr *) override;
  STDMETHODIMP OnPushContext(ITfContext *) override;
  STDMETHODIMP OnPopContext(ITfContext *) override;

  // ITfKeyEventSink
  STDMETHODIMP OnSetFocus(BOOL) override;
  STDMETHODIMP OnTestKeyDown(ITfContext *, WPARAM, LPARAM, BOOL *) override;
  STDMETHODIMP OnKeyDown(ITfContext *, WPARAM, LPARAM, BOOL *) override;
  STDMETHODIMP OnTestKeyUp(ITfContext *, WPARAM, LPARAM, BOOL *) override;
  STDMETHODIMP OnKeyUp(ITfContext *, WPARAM, LPARAM, BOOL *) override;
  STDMETHODIMP OnPreservedKey(ITfContext *, REFGUID, BOOL *) override;

  // ITfCompositionSink — l'application a mis fin à notre composition
  // (clic ailleurs, perte de focus…).
  STDMETHODIMP OnCompositionTerminated(TfEditCookie, ITfComposition *) override;

  // ITfCompartmentEventSink — la prédiction a été activée/coupée, peut-être
  // depuis une AUTRE application (compartiment global).
  STDMETHODIMP OnChange(REFGUID guid) override;

  // ITfTextLayoutSink — la mise en page du texte a changé (défilement,
  // fenêtre déplacée, mise en page enfin calculée après TS_E_NOLAYOUT) : la
  // barre se recale sur le caret.
  STDMETHODIMP OnLayoutChange(ITfContext *ctx, TfLayoutCode code,
                              ITfContextView *view) override;

  // --- indicateur de la barre des tâches -------------------------------------
  bool predictionEnabled() const { return enabled_; }
  void setPredictionEnabled(bool on);
  void openSettings();

  // --- utilisés par TsfFrontend --------------------------------------------
  TfClientId clientId() const { return clientId_; }
  ITfThreadMgr *threadMgr() const { return threadMgr_; }
  ITfComposition *composition() const { return composition_; }
  void setComposition(ITfComposition *c) { composition_ = c; }
  CandidateWindow &bar() { return bar_; }
  // Dernier préedit « tapé » : dit à la barre si un mot est en cours et si
  // c'est le picker emoji (':') — ce que les candidats seuls ne disent pas.
  void setTyped(const std::string &typed) { typed_ = typed; }
  // Requête du picker emoji (vide hors picker) : c'est elle, pas la
  // préédition, qui signale le mode grille.
  void setPickerQuery(const std::string &q) { query_ = q; }
  PanelHints panelHints() const { return {!typed_.empty(), !query_.empty()}; }
  core::PredictState &stateFor(ITfContext *ctx) { return states_[ctx]; }
  // Ancre de la barre, en pixels PHYSIQUES, validée. Ordre de préférence :
  //   1. le caret rapporté par l'application (GetTextExt) ;
  //   2. la dernière ancre valide de CE contexte — pendant TS_E_NOLAYOUT
  //      (Firefox, Chrome : la mise en page arrive d'un autre process), la
  //      barre reste où elle était au lieu de sauter ; OnLayoutChange la
  //      recale dès que la mise en page existe ;
  //   3. le caret Win32 ou le bas de la fenêtre (terminaux sans GetTextExt).
  // false : aucune position crédible, la barre est cachée.
  bool anchorFor(ITfContext *ctx, TfEditCookie ec, RECT &out);
  // Exécute `fn` dans une session d'édition synchrone lecture/écriture.
  // `async` : hors traitement de touche (clic sur l'indicateur…), TSF refuse
  // une session synchrone ; elle est alors simplement mise en file.
  HRESULT withEditSession(ITfContext *ctx,
                          std::function<void(TfEditCookie)> fn,
                          bool async = false);
  // Surveillance de la socket de refresh, via la file de messages du thread.
  void watchReadable(sock_t fd, std::function<void()> cb);
  void stopWatch();
  void postToMain(std::function<void()> fn);

private:
  ~CTextService();
  bool ensureMessageWindow();
  static LRESULT CALLBACK msgProc(HWND, UINT, WPARAM, LPARAM);
  // Le cœur demande une génération ; c'est le service qui fournit le thread et
  // qui rapatrie le résultat sur le thread de saisie.
  void installReformRunner(core::EngineCore &core, ITfContext *ctx);
  // Compartiment global « prédiction activée ».
  ITfCompartment *enabledCompartment();
  void initEnabledState();
  // Prédiction coupée en pleine frappe : le mot tapé reste, la suggestion et
  // la barre disparaissent.
  void settleComposition();

  // Rectangle LOGIQUE (coordonnées de l'hôte) du début du mot en cours, ou du
  // caret, et la fenêtre qui l'affiche.
  bool caretRect(ITfContext *ctx, TfEditCookie ec, RECT &out, HWND &host);
  // Repli quand l'application n'expose pas GetTextExt (terminaux).
  bool fallbackCaretRect(RECT &out, HWND &host);
  // Abonnement aux changements de mise en page du contexte qui a la barre.
  void watchLayout(ITfContext *ctx);
  void unwatchLayout();
  void forgetAnchor() { anchorCtx_ = nullptr; }
  // Relit le caret (session asynchrone) et recale la barre. Coalescé : un
  // glisser de fenêtre produit un événement par pixel.
  void requestReanchor();
  // Beaucoup d'applications (RichEdit compris) n'émettent PAS OnLayoutChange
  // quand leur fenêtre bouge : on écoute aussi les déplacements de la fenêtre
  // racine et du caret Win32, limités à CE thread.
  static void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild, DWORD thread,
                                    DWORD time);

  ITfContext *layoutCtx_ = nullptr; // référence tenue
  DWORD layoutCookie_ = TF_INVALID_COOKIE;
  HWINEVENTHOOK moveHook_ = nullptr;
  HWND hostRoot_ = nullptr;
  bool reanchorPending_ = false;
  ITfContext *anchorCtx_ = nullptr; // simple clé, jamais déréférencée
  RECT anchor_{};

  bool enabled_ = true;
  CLangBarButton *langBar_ = nullptr;
  DWORD compartmentCookie_ = TF_INVALID_COOKIE;

  // Préférences de session (dernier mode de reformulation). EngineCore étant
  // recréé à chaque touche, elles ne peuvent pas y vivre.
  core::SessionPrefs prefs_;

  LONG refCount_ = 1;
  ITfThreadMgr *threadMgr_ = nullptr;
  TfClientId clientId_ = TF_CLIENTID_NULL;
  DWORD threadMgrCookie_ = TF_INVALID_COOKIE;
  ITfComposition *composition_ = nullptr;
  ITfContext *activeCtx_ = nullptr;
  CandidateWindow bar_;
  std::string typed_;
  std::string query_;
  std::map<ITfContext *, core::PredictState> states_;

  // Pont asynchrone : la socket de refresh et les résultats de reformulation
  // réveillent CE thread par un message, jamais par un appel direct.
  HWND msgWnd_ = nullptr;
  sock_t watchFd_ = kBadSock;
  std::function<void()> watchCb_;
  std::shared_ptr<MainBridge> bridge_;
  // Contextes visés par une reformulation en cours. Le thread réseau ne
  // porte qu'une CLÉ (pointeur opaque) ; la référence COM, elle, ne vit et ne
  // meurt que sur ce thread. Une clé absente = champ fermé, résultat jeté.
  std::map<ITfContext *, Microsoft::WRL::ComPtr<ITfContext>> reformCtx_;
};

} // namespace win
