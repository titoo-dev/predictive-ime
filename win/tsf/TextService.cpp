#include "TextService.h"

#include "Debug.h"
#include "DisplayAttribute.h"
#include "Dpi.h"
#include "KeyboardLayout.h"
#include "LangBarButton.h"

#include "../../core/text.h"

#include <new>
#include <shellapi.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace win {
namespace {

constexpr UINT WM_PREDICT_WAKE = WM_APP + 41;    // travail posté depuis un thread
constexpr UINT WM_PREDICT_SOCKET = WM_APP + 42;  // socket de refresh lisible
constexpr wchar_t kMsgClass[] = L"PredictImeMsgWindow";

// Fenêtre de dialogue de clé API — sous Windows, l'app de préférences n'existe
// pas encore : on ouvre le config.json dans l'éditeur par défaut.
void openConfigInEditor() {
  std::string p = core::configPath();
  auto w = core::toUtf16(p);
  ::ShellExecuteW(nullptr, L"open",
                  reinterpret_cast<const wchar_t *>(w.c_str()), nullptr,
                  nullptr, SW_SHOWNORMAL);
}

// Trace de la disposition obtenue. JAMAIS de correction ici : basculer le HKL
// du thread (ActivateKeyboardLayout) fait resélectionner la disposition simple
// par le sélecteur de Windows — Predict était éjecté à chaque activation.
void logLayout() {
  layout::Status s = layout::current();
  dbg("disposition lang=%04x active=%ls voulue=%ls %s", unsigned(s.lang),
      s.active.c_str(), s.wanted.c_str(), s.matches ? "OK" : "DIFFERENTE");
}

std::wstring wideOf(const std::string &utf8) {
  auto u16 = core::toUtf16(utf8);
  return std::wstring(reinterpret_cast<const wchar_t *>(u16.data()),
                      u16.size());
}

std::string utf8Of(const std::wstring &w) {
  return core::fromUtf16(
      std::u16string(reinterpret_cast<const char16_t *>(w.data()), w.size()));
}

// --------------------------------------------------------- session d'édition
// Toute lecture/écriture du document passe OBLIGATOIREMENT par là : TSF ne
// donne un cookie d'édition que dans ce callback.
class CEditSession : public ITfEditSession {
public:
  explicit CEditSession(std::function<void(TfEditCookie)> fn)
      : fn_(std::move(fn)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
      *ppv = static_cast<ITfEditSession *>(this);
    if (!*ppv)
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return ULONG(InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    LONG c = InterlockedDecrement(&ref_);
    if (c == 0)
      delete this;
    return ULONG(c);
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    fn_(ec);
    return S_OK;
  }

private:
  ~CEditSession() = default;
  LONG ref_ = 1;
  std::function<void(TfEditCookie)> fn_;
};

// Applique un attribut d'affichage à une plage (soulignement du préedit).
void applyDisplayAttribute(ITfContext *ctx, TfEditCookie ec, ITfRange *range,
                           REFGUID guid) {
  ITfProperty *prop = nullptr;
  if (FAILED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop)) || !prop)
    return;
  ITfCategoryMgr *cat = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, (void **)&cat)) &&
      cat) {
    TfGuidAtom atom = TF_INVALID_GUIDATOM;
    if (SUCCEEDED(cat->RegisterGUID(guid, &atom))) {
      VARIANT v;
      ::VariantInit(&v);
      v.vt = VT_I4;
      v.lVal = LONG(atom);
      prop->SetValue(ec, range, &v);
    }
    cat->Release();
  }
  prop->Release();
}

} // namespace

// =========================================================== TsfFrontend =====

// Début de la zone « déjà écrite » : le début de la composition si elle
// existe, sinon le curseur. CRUCIAL : sous TSF le texte de composition est
// DANS le document — lire « avant le curseur » y inclurait le mot en cours de
// frappe, qui polluerait son propre contexte de prédiction.
static bool anchorRange(ITfContext *ctx, TfEditCookie ec,
                        ITfComposition *comp, ITfRange **out) {
  *out = nullptr;
  if (comp) {
    ITfRange *r = nullptr;
    if (SUCCEEDED(comp->GetRange(&r)) && r) {
      ITfRange *clone = nullptr;
      if (SUCCEEDED(r->Clone(&clone))) {
        clone->Collapse(ec, TF_ANCHOR_START);
        *out = clone;
      }
      r->Release();
      return *out != nullptr;
    }
  }
  ITfRange *sel = nullptr;
  TF_SELECTION s{};
  ULONG fetched = 0;
  if (FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &s, &fetched)) ||
      fetched == 0)
    return false;
  sel = s.range;
  ITfRange *clone = nullptr;
  if (SUCCEEDED(sel->Clone(&clone))) {
    clone->Collapse(ec, TF_ANCHOR_START);
    *out = clone;
  }
  sel->Release();
  return *out != nullptr;
}

void TsfFrontend::commitText(const std::string &utf8) {
  std::wstring w = wideOf(utf8);
  svc_->setTyped(std::string{});
  ITfComposition *comp = svc_->composition();
  if (comp) {
    ITfRange *r = nullptr;
    if (SUCCEEDED(comp->GetRange(&r)) && r) {
      r->SetText(ec_, 0, w.c_str(), LONG(w.size()));
      // Curseur APRÈS le texte inséré, puis on clôt la composition : le texte
      // reste, il cesse simplement d'être « en cours de saisie ».
      ITfRange *end = nullptr;
      if (SUCCEEDED(r->Clone(&end))) {
        end->Collapse(ec_, TF_ANCHOR_END);
        TF_SELECTION sel{};
        sel.range = end;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        ctx_->SetSelection(ec_, 1, &sel);
        end->Release();
      }
      r->Release();
    }
    comp->EndComposition(ec_);
    comp->Release();
    svc_->setComposition(nullptr);
    return;
  }
  // Hors composition : insertion à la sélection courante.
  TF_SELECTION s{};
  ULONG fetched = 0;
  if (FAILED(ctx_->GetSelection(ec_, TF_DEFAULT_SELECTION, 1, &s, &fetched)) ||
      fetched == 0)
    return;
  s.range->SetText(ec_, 0, w.c_str(), LONG(w.size()));
  s.range->Collapse(ec_, TF_ANCHOR_END);
  ctx_->SetSelection(ec_, 1, &s);
  s.range->Release();
}

void TsfFrontend::setPreedit(const std::string &typed,
                             const std::string &ghost) {
  ITfComposition *comp = svc_->composition();
  const std::wstring wtyped = wideOf(typed), wghost = wideOf(ghost);
  svc_->setTyped(typed);

  // Préedit vide = la composition est ABANDONNÉE (sémantique fcitx5 : le
  // préedit n'est pas du texte du document). Sous TSF il l'est : il faut donc
  // l'effacer explicitement, sans quoi le mot resterait inséré.
  if (wtyped.empty() && wghost.empty()) {
    if (comp) {
      ITfRange *r = nullptr;
      if (SUCCEEDED(comp->GetRange(&r)) && r) {
        r->SetText(ec_, 0, L"", 0);
        r->Release();
      }
      comp->EndComposition(ec_);
      comp->Release();
      svc_->setComposition(nullptr);
    }
    return;
  }

  if (!comp) {
    ITfContextComposition *cc = nullptr;
    if (FAILED(ctx_->QueryInterface(IID_ITfContextComposition, (void **)&cc)) ||
        !cc)
      return;
    TF_SELECTION s{};
    ULONG fetched = 0;
    if (SUCCEEDED(ctx_->GetSelection(ec_, TF_DEFAULT_SELECTION, 1, &s,
                                     &fetched)) &&
        fetched) {
      ITfComposition *fresh = nullptr;
      if (SUCCEEDED(cc->StartComposition(ec_, s.range, svc_, &fresh)) && fresh) {
        svc_->setComposition(fresh);
        comp = fresh;
      }
      s.range->Release();
    }
    cc->Release();
    if (!comp)
      return;
  }

  ITfRange *r = nullptr;
  if (FAILED(comp->GetRange(&r)) || !r)
    return;
  std::wstring all = wtyped + wghost;
  r->SetText(ec_, 0, all.c_str(), LONG(all.size()));

  // Soulignement sur ce qui est tapé, pointillé gris sur la proposition.
  ITfRange *typedRange = nullptr;
  if (SUCCEEDED(r->Clone(&typedRange))) {
    typedRange->Collapse(ec_, TF_ANCHOR_START);
    LONG shifted = 0;
    typedRange->ShiftEnd(ec_, LONG(wtyped.size()), &shifted, nullptr);
    applyDisplayAttribute(ctx_, ec_, typedRange,
                          GUID_PredictDisplayAttributeInput);
    typedRange->Release();
  }
  if (!wghost.empty()) {
    ITfRange *ghostRange = nullptr;
    if (SUCCEEDED(r->Clone(&ghostRange))) {
      ghostRange->Collapse(ec_, TF_ANCHOR_START);
      LONG moved = 0;
      ghostRange->ShiftStart(ec_, LONG(wtyped.size()), &moved, nullptr);
      applyDisplayAttribute(ctx_, ec_, ghostRange,
                            GUID_PredictDisplayAttributeGhost);
      ghostRange->Release();
    }
  }

  // Le caret se place ENTRE le tapé et le fantôme : la frappe continue là où
  // l'utilisateur l'attend, la proposition reste devant lui.
  //
  // ShiftEnd puis Collapse(END), surtout PAS ShiftStart : sur une plage vide,
  // pousser le DÉBUT au-delà de la FIN est impossible, la plage est ramenée et
  // le caret ne bouge jamais — il restait collé au début de la composition
  // (l'application affichait « Col 9 » quoi qu'on tape).
  ITfRange *caret = nullptr;
  if (SUCCEEDED(r->Clone(&caret))) {
    caret->Collapse(ec_, TF_ANCHOR_START);
    LONG moved = 0;
    caret->ShiftEnd(ec_, LONG(wtyped.size()), &moved, nullptr);
    caret->Collapse(ec_, TF_ANCHOR_END);
    TF_SELECTION sel{};
    sel.range = caret;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    HRESULT hrSel = ctx_->SetSelection(ec_, 1, &sel);
    dbg("setPreedit typed=%u ghost=%u caretShift=%ld selHR=0x%08lx",
        unsigned(wtyped.size()), unsigned(wghost.size()), moved,
        (unsigned long)hrSel);
    caret->Release();
  }
  r->Release();
}

void TsfFrontend::setCandidates(const std::vector<core::Candidate> &cands,
                                int cursor, const std::string &auxTitle) {
  // Beaucoup d'applications — les terminaux en tête — n'implémentent pas
  // GetTextExt. Ne rien afficher du tout y supprimerait la barre ENTIÈREMENT,
  // alors qu'une barre légèrement mal placée reste utilisable (c'est
  // exactement le compromis que `nextWordBarExclude` arbitre côté Linux) :
  // anchorFor retombe donc sur le caret Win32 puis le bas de la fenêtre.
  RECT caret{};
  if (!svc_->anchorFor(ctx_, ec_, caret)) {
    svc_->bar().hide();
    return;
  }
  svc_->bar().show(cands, cursor, auxTitle, caret, svc_->panelHints());
}

void TsfFrontend::clearPanel() {
  // Côté fcitx5, effacer le panneau jette aussi la préédition : même
  // sémantique ici, donc la composition en cours est abandonnée.
  setPreedit(std::string{}, std::string{});
  svc_->bar().hide();
}

bool TsfFrontend::hasCandidates() const { return svc_->bar().visible(); }

bool TsfFrontend::surrounding(core::Surrounding &out) {
  ITfRange *anchor = nullptr;
  if (!anchorRange(ctx_, ec_, svc_->composition(), &anchor))
    return false;

  // Jusqu'à 512 unités UTF-16 avant le point d'ancrage : assez pour le
  // contexte large du neural (240 caractères) avec de la marge.
  ITfRange *back = nullptr;
  if (FAILED(anchor->Clone(&back)) || !back) {
    anchor->Release();
    return false;
  }
  LONG moved = 0;
  back->ShiftStart(ec_, -512, &moved, nullptr);

  std::wstring buf;
  buf.resize(512);
  ULONG got = 0;
  if (FAILED(back->GetText(ec_, 0, &buf[0], ULONG(buf.size()), &got))) {
    back->Release();
    anchor->Release();
    return false;
  }
  buf.resize(got);
  out.text = utf8Of(buf);
  // Le curseur est à la FIN de ce qu'on vient de lire (on a lu jusqu'à
  // l'ancre), exprimé en points de code.
  out.cursor = core::decodeUtf8(out.text).size();
  out.anchor = out.cursor;
  dbg("surrounding shift=%ld got=%u text='%.60s'", moved, unsigned(got),
      out.text.c_str());
  back->Release();
  anchor->Release();

  // Sélection rapportée (pour la reformulation).
  TF_SELECTION s{};
  ULONG fetched = 0;
  if (SUCCEEDED(ctx_->GetSelection(ec_, TF_DEFAULT_SELECTION, 1, &s,
                                   &fetched)) &&
      fetched) {
    BOOL empty = TRUE;
    if (SUCCEEDED(s.range->IsEmpty(ec_, &empty)) && !empty) {
      std::wstring selBuf;
      selBuf.resize(4096);
      ULONG sgot = 0;
      if (SUCCEEDED(s.range->GetText(ec_, 0, &selBuf[0], ULONG(selBuf.size()),
                                     &sgot))) {
        selBuf.resize(sgot);
        out.selected = utf8Of(selBuf);
      }
      // Une sélection non vide déplace l'ancre : le cœur s'en sert pour
      // refuser la recomposition Backspace au milieu d'une sélection.
      out.anchor = out.cursor + core::decodeUtf8(out.selected).size();
    }
    s.range->Release();
  }
  return true;
}

void TsfFrontend::deleteBefore(unsigned cps) {
  if (cps == 0)
    return;
  core::Surrounding s;
  if (!surrounding(s))
    return;
  auto all = core::decodeUtf8(s.text);
  if (cps > all.size())
    cps = unsigned(all.size());
  // Les plages TSF comptent en unités UTF-16, le cœur en points de code : un
  // emoji vaut 2. Confondre les deux décalerait chaque revert.
  std::string tail;
  for (size_t i = all.size() - cps; i < all.size(); i++)
    core::appendCp(tail, all[i]);
  LONG units = LONG(core::utf16Length(tail));

  ITfRange *anchor = nullptr;
  if (!anchorRange(ctx_, ec_, svc_->composition(), &anchor))
    return;
  LONG moved = 0;
  anchor->ShiftStart(ec_, -units, &moved, nullptr);
  anchor->SetText(ec_, 0, L"", 0);
  anchor->Release();
}

void TsfFrontend::deleteRange(unsigned back, unsigned count) {
  (void)count;
  deleteBefore(back);
}

// Le service est chargé DANS le process de l'application : son propre nom de
// module EST le programme hôte (l'équivalent de ic->program()).
std::string TsfFrontend::program() {
  wchar_t path[MAX_PATH]{};
  DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0)
    return {};
  std::wstring full(path, n);
  size_t slash = full.find_last_of(L"\\/");
  return utf8Of(slash == std::wstring::npos ? full : full.substr(slash + 1));
}

// Champs mot de passe : aucune prédiction, aucune préédition. TSF n'expose pas
// directement l'information ; le style ES_PASSWORD du contrôle focalisé couvre
// les contrôles Win32 classiques. Les navigateurs gèrent leurs champs
// eux-mêmes et n'activent en général pas de TIP dessus.
bool TsfFrontend::isPasswordField() {
  GUITHREADINFO gti{sizeof(gti)};
  if (!::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) || !gti.hwndFocus)
    return false;
  LONG style = ::GetWindowLongW(gti.hwndFocus, GWL_STYLE);
  wchar_t cls[64]{};
  ::GetClassNameW(gti.hwndFocus, cls, 64);
  if (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit") == 0 ||
      _wcsicmp(cls, L"RICHEDIT50W") == 0)
    return (style & ES_PASSWORD) != 0;
  return false;
}

void TsfFrontend::watchReadable(sock_t fd, std::function<void()> cb) {
  svc_->watchReadable(fd, std::move(cb));
}
void TsfFrontend::stopWatch() { svc_->stopWatch(); }
void TsfFrontend::postToMain(std::function<void()> fn) {
  svc_->postToMain(std::move(fn));
}
void TsfFrontend::openKeyDialog() { openConfigInEditor(); }

// =========================================================== CTextService ====

CTextService::CTextService() { ::InitializeCriticalSection(&postedLock_); }

CTextService::~CTextService() {
  stopWatch();
  if (msgWnd_)
    ::DestroyWindow(msgWnd_);
  ::DeleteCriticalSection(&postedLock_);
}

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_INVALIDARG;
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppv = static_cast<ITfTextInputProcessor *>(this);
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppv = static_cast<ITfTextInputProcessorEx *>(this);
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppv = static_cast<ITfThreadMgrEventSink *>(this);
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppv = static_cast<ITfKeyEventSink *>(this);
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppv = static_cast<ITfCompositionSink *>(this);
  else if (IsEqualIID(riid, IID_ITfCompartmentEventSink))
    *ppv = static_cast<ITfCompartmentEventSink *>(this);
  else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    *ppv = static_cast<ITfTextLayoutSink *>(this);
  if (!*ppv)
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) CTextService::AddRef() {
  return ULONG(InterlockedIncrement(&refCount_));
}

STDMETHODIMP_(ULONG) CTextService::Release() {
  LONG c = InterlockedDecrement(&refCount_);
  if (c == 0)
    delete this;
  return ULONG(c);
}

STDMETHODIMP CTextService::Activate(ITfThreadMgr *tm, TfClientId cid) {
  return ActivateEx(tm, cid, 0);
}

STDMETHODIMP CTextService::ActivateEx(ITfThreadMgr *tm, TfClientId cid,
                                      DWORD /*flags*/) {
  threadMgr_ = tm;
  threadMgr_->AddRef();
  clientId_ = cid;

  ITfSource *src = nullptr;
  if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfSource, (void **)&src)) &&
      src) {
    src->AdviseSink(IID_ITfThreadMgrEventSink,
                    static_cast<ITfThreadMgrEventSink *>(this),
                    &threadMgrCookie_);
    src->Release();
  }
  ITfKeystrokeMgr *ksm = nullptr;
  if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                           (void **)&ksm)) &&
      ksm) {
    ksm->AdviseKeyEventSink(clientId_, static_cast<ITfKeyEventSink *>(this),
                            TRUE);
    ksm->Release();
  }
  ensureMessageWindow();
  // La socket du daemon exige WSAStartup dans CE process (on est chargé dans
  // l'application hôte, qui n'a aucune raison de l'avoir fait).
  oscompat::netInit();

  // État partagé « prédiction activée » + abonnement à ses changements.
  initEnabledState();
  if (ITfCompartment *comp = enabledCompartment()) {
    ITfSource *csrc = nullptr;
    if (SUCCEEDED(comp->QueryInterface(IID_ITfSource, (void **)&csrc)) &&
        csrc) {
      csrc->AdviseSink(IID_ITfCompartmentEventSink,
                       static_cast<ITfCompartmentEventSink *>(this),
                       &compartmentCookie_);
      csrc->Release();
    }
    comp->Release();
  }
  // Posté : TSF ne bascule le HKL du thread qu'APRÈS le retour d'ActivateEx.
  postToMain([] { logLayout(); });
  // Indicateur dans la barre des tâches.
  ITfLangBarItemMgr *lbm = nullptr;
  if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfLangBarItemMgr,
                                           (void **)&lbm)) &&
      lbm) {
    langBar_ = new (std::nothrow) CLangBarButton(this);
    if (langBar_ && FAILED(lbm->AddItem(langBar_))) {
      langBar_->Release();
      langBar_ = nullptr;
    }
    lbm->Release();
  }
  return S_OK;
}

STDMETHODIMP CTextService::Deactivate() {
  stopWatch();
  bar_.hide();
  unwatchLayout();
  forgetAnchor();
  if (threadMgr_ && langBar_) {
    ITfLangBarItemMgr *lbm = nullptr;
    if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfLangBarItemMgr,
                                             (void **)&lbm)) &&
        lbm) {
      lbm->RemoveItem(langBar_);
      lbm->Release();
    }
  }
  if (langBar_) {
    langBar_->detach();
    langBar_->Release();
    langBar_ = nullptr;
  }
  if (threadMgr_ && compartmentCookie_ != TF_INVALID_COOKIE) {
    if (ITfCompartment *comp = enabledCompartment()) {
      ITfSource *csrc = nullptr;
      if (SUCCEEDED(comp->QueryInterface(IID_ITfSource, (void **)&csrc)) &&
          csrc) {
        csrc->UnadviseSink(compartmentCookie_);
        csrc->Release();
      }
      comp->Release();
    }
    compartmentCookie_ = TF_INVALID_COOKIE;
  }
  if (threadMgr_) {
    ITfKeystrokeMgr *ksm = nullptr;
    if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                             (void **)&ksm)) &&
        ksm) {
      ksm->UnadviseKeyEventSink(clientId_);
      ksm->Release();
    }
    if (threadMgrCookie_ != TF_INVALID_COOKIE) {
      ITfSource *src = nullptr;
      if (SUCCEEDED(threadMgr_->QueryInterface(IID_ITfSource, (void **)&src)) &&
          src) {
        src->UnadviseSink(threadMgrCookie_);
        src->Release();
      }
      threadMgrCookie_ = TF_INVALID_COOKIE;
    }
    threadMgr_->Release();
    threadMgr_ = nullptr;
  }
  states_.clear();
  return S_OK;
}

STDMETHODIMP CTextService::OnInitDocumentMgr(ITfDocumentMgr *) { return S_OK; }
STDMETHODIMP CTextService::OnUninitDocumentMgr(ITfDocumentMgr *) {
  return S_OK;
}

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr *, ITfDocumentMgr *) {
  // Changement de champ : la barre spéculative n'a plus de sens, ni sa
  // position — une ancre d'un autre champ ferait apparaître la barre loin du
  // nouveau caret.
  bar_.hide();
  stopWatch();
  unwatchLayout();
  forgetAnchor();
  return S_OK;
}

STDMETHODIMP CTextService::OnPushContext(ITfContext *) { return S_OK; }

STDMETHODIMP CTextService::OnPopContext(ITfContext *ctx) {
  states_.erase(ctx); // pas de fuite d'état sur les champs fermés
  if (ctx == layoutCtx_)
    unwatchLayout();
  if (ctx == anchorCtx_)
    forgetAnchor();
  return S_OK;
}

STDMETHODIMP CTextService::OnSetFocus(BOOL fForeground) {
  if (!fForeground)
    bar_.hide();
  return S_OK;
}

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie,
                                                   ITfComposition *comp) {
  // L'application a coupé la composition (clic ailleurs, Ctrl+Z…) : on lâche
  // notre référence, sinon toute frappe suivante viserait une plage morte.
  if (composition_ == comp) {
    composition_->Release();
    composition_ = nullptr;
  }
  bar_.hide();
  return S_OK;
}

// --------------------------------------------- prédiction activée / coupée --

namespace {
// Persistance entre sessions : un fichier-drapeau dans ipcDir(), le seul
// dossier où une application en bac à sable peut écrire (cf Debug.h). Le
// compartiment global, lui, ne vit que le temps de la session Windows.
std::wstring disabledFlagPath() {
  return wideOf(oscompat::ipcDir() + "\\prediction-off");
}
} // namespace

ITfCompartment *CTextService::enabledCompartment() {
  if (!threadMgr_)
    return nullptr;
  ITfCompartmentMgr *mgr = nullptr;
  if (FAILED(threadMgr_->GetGlobalCompartment(&mgr)) || !mgr)
    return nullptr;
  ITfCompartment *comp = nullptr;
  mgr->GetCompartment(GUID_PredictCompartmentEnabled, &comp);
  mgr->Release();
  return comp;
}

void CTextService::initEnabledState() {
  ITfCompartment *comp = enabledCompartment();
  VARIANT v;
  ::VariantInit(&v);
  if (comp && SUCCEEDED(comp->GetValue(&v)) && v.vt == VT_I4) {
    enabled_ = v.lVal != 0;
  } else {
    // Première application de la session : l'état vient du disque, et on le
    // publie pour toutes les autres.
    enabled_ = ::GetFileAttributesW(disabledFlagPath().c_str()) ==
               INVALID_FILE_ATTRIBUTES;
    if (comp) {
      ::VariantClear(&v);
      v.vt = VT_I4;
      v.lVal = enabled_ ? 1 : 0;
      comp->SetValue(clientId_, &v);
    }
  }
  ::VariantClear(&v);
  if (comp)
    comp->Release();
}

void CTextService::setPredictionEnabled(bool on) {
  if (on) {
    ::DeleteFileW(disabledFlagPath().c_str());
  } else {
    HANDLE h = ::CreateFileW(disabledFlagPath().c_str(), GENERIC_WRITE, 0,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE)
      ::CloseHandle(h);
  }
  ITfCompartment *comp = enabledCompartment();
  if (comp) {
    // SetValue notifie TOUS les text services abonnés, nous compris :
    // OnChange fait le reste.
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = on ? 1 : 0;
    HRESULT hr = comp->SetValue(clientId_, &v);
    comp->Release();
    if (SUCCEEDED(hr))
      return;
  }
  enabled_ = on;
  if (!on)
    settleComposition();
  if (langBar_)
    langBar_->refresh();
}

STDMETHODIMP CTextService::OnChange(REFGUID guid) {
  if (!IsEqualGUID(guid, GUID_PredictCompartmentEnabled))
    return S_OK;
  ITfCompartment *comp = enabledCompartment();
  if (!comp)
    return S_OK;
  VARIANT v;
  ::VariantInit(&v);
  if (SUCCEEDED(comp->GetValue(&v)) && v.vt == VT_I4) {
    bool on = v.lVal != 0;
    if (on != enabled_) {
      enabled_ = on;
      if (!on)
        settleComposition();
    }
  }
  ::VariantClear(&v);
  comp->Release();
  if (langBar_)
    langBar_->refresh();
  return S_OK;
}

void CTextService::settleComposition() {
  bar_.hide();
  stopWatch();
  if (!threadMgr_)
    return;
  ITfDocumentMgr *dim = nullptr;
  if (FAILED(threadMgr_->GetFocus(&dim)) || !dim)
    return;
  ITfContext *ctx = nullptr;
  if (SUCCEEDED(dim->GetTop(&ctx)) && ctx) {
    if (composition_) {
      // Le mot tapé est validé tel quel ; seul le fantôme disparaît.
      // Session ASYNCHRONE : elle peut courir après notre retour, elle tient
      // donc sa propre référence sur le contexte.
      Microsoft::WRL::ComPtr<ITfContext> keep(ctx);
      withEditSession(
          ctx,
          [this, keep](TfEditCookie ec) {
            TsfFrontend fe(this, keep.Get(), ec);
            fe.commitText(stateFor(keep.Get()).buffer);
            states_.erase(keep.Get());
          },
          /*async=*/true);
    } else {
      states_.erase(ctx);
    }
    ctx->Release();
  }
  dim->Release();
}

void CTextService::openSettings() { openConfigInEditor(); }

HRESULT CTextService::withEditSession(ITfContext *ctx,
                                      std::function<void(TfEditCookie)> fn,
                                      bool async) {
  auto *session = new (std::nothrow) CEditSession(std::move(fn));
  if (!session)
    return E_OUTOFMEMORY;
  HRESULT hrSession = S_OK;
  HRESULT hr = ctx->RequestEditSession(
      clientId_, session,
      (async ? TF_ES_ASYNCDONTCARE : TF_ES_SYNC) | TF_ES_READWRITE,
      &hrSession);
  session->Release();
  return SUCCEEDED(hr) ? hrSession : hr;
}

bool CTextService::caretRect(ITfContext *ctx, TfEditCookie ec, RECT &out,
                             HWND &host) {
  host = nullptr;
  ITfContextView *view = nullptr;
  if (FAILED(ctx->GetActiveView(&view)) || !view)
    return false;
  // La fenêtre qui affiche CE texte — jamais « la fenêtre au premier plan »,
  // qui peut appartenir à un autre process (et fausser la conversion DPI).
  view->GetWnd(&host);
  if (!host) {
    GUITHREADINFO gti{sizeof(gti)};
    if (::GetGUIThreadInfo(::GetCurrentThreadId(), &gti))
      host = gti.hwndFocus ? gti.hwndFocus : gti.hwndActive;
  }

  // Ancre = PREMIER caractère du mot en cours, pas toute la composition :
  // quand l'application renvoie le fantôme à la ligne, l'union des deux lignes
  // commence en début de ligne et la barre sautait à gauche.
  ITfRange *range = nullptr;
  if (composition_) {
    ITfRange *whole = nullptr;
    if (SUCCEEDED(composition_->GetRange(&whole)) && whole) {
      if (SUCCEEDED(whole->Clone(&range)) && range) {
        range->Collapse(ec, TF_ANCHOR_START);
        LONG moved = 0;
        range->ShiftEnd(ec, 1, &moved, nullptr);
      }
      whole->Release();
    }
  } else {
    TF_SELECTION s{};
    ULONG fetched = 0;
    if (SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &s,
                                    &fetched)) &&
        fetched) {
      // Début de la sélection : une sélection sur plusieurs lignes aurait,
      // elle aussi, un rectangle qui commence en début de ligne.
      s.range->Collapse(ec, TF_ANCHOR_START);
      range = s.range;
    }
  }
  bool result = false;
  if (range) {
    BOOL clipped = FALSE;
    RECT rc{};
    HRESULT hr = view->GetTextExt(ec, range, &rc, &clipped);
    if (SUCCEEDED(hr) && rc.bottom > rc.top) {
      out = rc;
      result = true;
    } else {
      dbg("GetTextExt hr=0x%08lx rc=(%ld,%ld,%ld,%ld)", (unsigned long)hr,
          rc.left, rc.top, rc.right, rc.bottom);
    }
    range->Release();
  }
  view->Release();
  return result;
}

bool CTextService::anchorFor(ITfContext *ctx, TfEditCookie ec, RECT &out) {
  RECT r{};
  HWND host = nullptr;
  if (caretRect(ctx, ec, r, host) && dpi::toPhysical(host, r, out)) {
    anchorCtx_ = ctx;
    anchor_ = out;
    hostRoot_ = ::GetAncestor(host, GA_ROOT);
    reanchorPending_ = false; // une session perdue ne bloque jamais le suivi
    watchLayout(ctx);
    return true;
  }
  // Mise en page pas encore prête (TS_E_NOLAYOUT) ou rectangle aberrant : on
  // reste à la dernière position valide du même champ. C'est la différence
  // entre « la barre suit » et « la barre saute au coin de la fenêtre à
  // chaque premier caractère de mot ».
  if (anchorCtx_ == ctx) {
    out = anchor_;
    watchLayout(ctx);
    return true;
  }
  if (fallbackCaretRect(r, host) && dpi::toPhysical(host, r, out)) {
    hostRoot_ = ::GetAncestor(host, GA_ROOT);
    // Pas mémorisé : dès qu'un vrai caret arrive, il l'emporte.
    watchLayout(ctx);
    return true;
  }
  dbg("aucune ancre crédible : barre cachée");
  return false;
}

// Position de repli quand l'application n'expose pas l'étendue du texte.
// GetGUIThreadInfo rapporte le caret Win32 (les terminaux le posent en
// général) ; à défaut on vise le bas de la fenêtre focalisée, pour que la
// barre reste visible plutôt que de disparaître. Toujours sur CE thread :
// une fenêtre d'un autre process n'a rien à dire de notre caret.
bool CTextService::fallbackCaretRect(RECT &out, HWND &host) {
  GUITHREADINFO gti{sizeof(gti)};
  if (!::GetGUIThreadInfo(::GetCurrentThreadId(), &gti))
    return false;
  if (gti.hwndCaret && gti.rcCaret.bottom > gti.rcCaret.top) {
    RECT r = gti.rcCaret;
    POINT tl{r.left, r.top}, br{r.right, r.bottom};
    ::ClientToScreen(gti.hwndCaret, &tl);
    ::ClientToScreen(gti.hwndCaret, &br);
    out = RECT{tl.x, tl.y, br.x, br.y};
    host = gti.hwndCaret;
    return true;
  }
  HWND hwnd = gti.hwndFocus ? gti.hwndFocus : gti.hwndActive;
  RECT w{};
  if (!hwnd || !::GetWindowRect(hwnd, &w))
    return false;
  // Coin inférieur gauche : la barre se posera juste en dessous.
  out = RECT{w.left + 8, w.bottom - 40, w.left + 8, w.bottom - 20};
  host = hwnd;
  return true;
}

// ------------------------------------------------ suivi de la mise en page --

namespace {
// Le crochet WinEvent n'a pas de paramètre utilisateur : le service du thread
// est retrouvé ici (un text service par thread d'interface).
thread_local CTextService *t_watcher = nullptr;
} // namespace

void CTextService::watchLayout(ITfContext *ctx) {
  if (!moveHook_) {
    moveHook_ = ::SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        &CTextService::winEventProc, ::GetCurrentProcessId(),
        ::GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    if (moveHook_)
      t_watcher = this;
  }
  if (!ctx || ctx == layoutCtx_)
    return;
  if (layoutCtx_) {
    ITfSource *old = nullptr;
    if (SUCCEEDED(layoutCtx_->QueryInterface(IID_ITfSource, (void **)&old)) &&
        old) {
      old->UnadviseSink(layoutCookie_);
      old->Release();
    }
    layoutCtx_->Release();
    layoutCtx_ = nullptr;
    layoutCookie_ = TF_INVALID_COOKIE;
  }
  ITfSource *src = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(IID_ITfSource, (void **)&src)) && src) {
    if (SUCCEEDED(src->AdviseSink(IID_ITfTextLayoutSink,
                                  static_cast<ITfTextLayoutSink *>(this),
                                  &layoutCookie_))) {
      layoutCtx_ = ctx;
      layoutCtx_->AddRef();
    }
    src->Release();
  }
}

void CTextService::unwatchLayout() {
  if (moveHook_) {
    ::UnhookWinEvent(moveHook_);
    moveHook_ = nullptr;
    if (t_watcher == this)
      t_watcher = nullptr;
  }
  hostRoot_ = nullptr;
  reanchorPending_ = false;
  if (!layoutCtx_)
    return;
  ITfSource *src = nullptr;
  if (SUCCEEDED(layoutCtx_->QueryInterface(IID_ITfSource, (void **)&src)) &&
      src) {
    src->UnadviseSink(layoutCookie_);
    src->Release();
  }
  layoutCtx_->Release();
  layoutCtx_ = nullptr;
  layoutCookie_ = TF_INVALID_COOKIE;
}

void CALLBACK CTextService::winEventProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                                         LONG idObject, LONG, DWORD, DWORD) {
  CTextService *self = t_watcher;
  if (!self)
    return;
  // Le caret a bougé (défilement, clic) ou la fenêtre qui porte le texte a
  // été déplacée/redimensionnée. Nos propres fenêtres (la barre) en émettent
  // aussi : elles ne sont ni l'un ni l'autre.
  if (idObject == OBJID_CARET ||
      (idObject == OBJID_WINDOW && hwnd && hwnd == self->hostRoot_))
    self->requestReanchor();
}

void CTextService::requestReanchor() {
  if (reanchorPending_ || !layoutCtx_ || !bar_.visible())
    return;
  reanchorPending_ = true;
  // Session ASYNCHRONE : elle court après le traitement en cours, et tient sa
  // propre référence sur le contexte.
  Microsoft::WRL::ComPtr<ITfContext> keep(layoutCtx_);
  HRESULT hr = withEditSession(
      layoutCtx_,
      [this, keep](TfEditCookie ec) {
        reanchorPending_ = false;
        if (!bar_.visible())
          return;
        RECT r{}, p{};
        HWND host = nullptr;
        // Mise en page pas prête (TS_E_NOLAYOUT) : on ne bouge pas.
        if (caretRect(keep.Get(), ec, r, host) &&
            dpi::toPhysical(host, r, p)) {
          anchorCtx_ = keep.Get();
          anchor_ = p;
          hostRoot_ = ::GetAncestor(host, GA_ROOT);
          bar_.moveTo(p);
        }
      },
      /*async=*/true);
  if (FAILED(hr))
    reanchorPending_ = false;
}

STDMETHODIMP CTextService::OnLayoutChange(ITfContext *ctx, TfLayoutCode code,
                                          ITfContextView *) {
  if (code == TF_LC_DESTROY) {
    if (ctx == layoutCtx_)
      unwatchLayout();
    return S_OK;
  }
  if (ctx && ctx == layoutCtx_)
    requestReanchor();
  return S_OK;
}

// --------------------------------------------------- pont asynchrone --------

LRESULT CALLBACK CTextService::msgProc(HWND hwnd, UINT msg, WPARAM wp,
                                       LPARAM lp) {
  auto *self =
      reinterpret_cast<CTextService *>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self) {
    if (msg == WM_PREDICT_SOCKET) {
      if (self->watchCb_)
        self->watchCb_();
      return 0;
    }
    if (msg == WM_PREDICT_WAKE) {
      std::vector<std::function<void()>> todo;
      ::EnterCriticalSection(&self->postedLock_);
      todo.swap(self->posted_);
      ::LeaveCriticalSection(&self->postedLock_);
      for (auto &fn : todo)
        fn();
      return 0;
    }
  }
  if (msg == WM_NCCREATE) {
    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool CTextService::ensureMessageWindow() {
  if (msgWnd_)
    return true;
  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msgProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMsgClass;
    if (!::RegisterClassExW(&wc) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return false;
    registered = true;
  }
  msgWnd_ = ::CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, inst, this);
  return msgWnd_ != nullptr;
}

// WSAAsyncSelect route la lisibilité de la socket vers la file de messages du
// thread de saisie : aucune attente bloquante, donc la frappe ne gèle jamais
// derrière le daemon (c'est LE risque du chargement in-process).
void CTextService::watchReadable(sock_t fd, std::function<void()> cb) {
  stopWatch();
  if (!ensureMessageWindow())
    return;
  watchFd_ = fd;
  watchCb_ = std::move(cb);
  ::WSAAsyncSelect(fd, msgWnd_, WM_PREDICT_SOCKET, FD_READ | FD_CLOSE);
}

void CTextService::stopWatch() {
  if (oscompat::sockValid(watchFd_)) {
    if (msgWnd_)
      ::WSAAsyncSelect(watchFd_, msgWnd_, 0, 0);
    oscompat::sockClose(watchFd_);
    watchFd_ = kBadSock;
  }
  watchCb_ = nullptr;
}

void CTextService::postToMain(std::function<void()> fn) {
  if (!ensureMessageWindow())
    return;
  ::EnterCriticalSection(&postedLock_);
  posted_.push_back(std::move(fn));
  ::LeaveCriticalSection(&postedLock_);
  ::PostMessageW(msgWnd_, WM_PREDICT_WAKE, 0, 0);
}

} // namespace win
