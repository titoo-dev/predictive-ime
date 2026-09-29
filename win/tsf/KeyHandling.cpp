// Traduction clavier Windows → core::KeyEvent, et exécution d'une touche dans
// une session d'édition.
//
// Le point délicat de TSF : OnTestKeyDown doit dire si on MANGERAIT la touche,
// sans effet de bord, AVANT de la traiter. On y répond largement (tout ce qui
// pourrait nous concerner) et c'est OnKeyDown qui tranche pour de bon — c'est
// le patron des échantillons Microsoft. Une touche annoncée « mangée » puis
// relâchée par OnKeyDown est bien réinjectée par TSF.
#include "TextService.h"

#include "Debug.h"
#include "Module.h"

#include "../../core/engine_core.h"

#include <memory>
#include <windows.h>

namespace win {
namespace {

// Chiffre PHYSIQUE 0-based pour les panneaux modaux. Sur AZERTY les chiffres
// exigent Shift : sans la rangée physique, « autre touche » fermait le panneau
// EN SILENCE et la bascule de langue était perdue. VK_1..VK_9 correspondent à
// la rangée du haut quelle que soit la disposition.
int panelDigitOf(WPARAM vk, uint32_t cp) {
  if (cp >= '1' && cp <= '9')
    return int(cp - '1');
  // Rangée AZERTY non shiftée : & é " ' ( - è _ ç
  static const uint32_t az[] = {'&', 0xE9, '"', '\'', '(', '-', 0xE8, '_', 0xE7};
  for (int i = 0; i < 9; i++)
    if (cp && cp == az[i])
      return i;
  if (vk >= '1' && vk <= '9')
    return int(vk - '1');
  return -1;
}

core::Key mapVk(WPARAM vk, bool shift) {
  switch (vk) {
  case VK_ESCAPE:
    return core::Key::Escape;
  case VK_BACK:
    return core::Key::Backspace;
  case VK_TAB:
    return shift ? core::Key::ShiftTab : core::Key::Tab;
  case VK_RETURN:
    return core::Key::Enter;
  case VK_SPACE:
    return core::Key::Space;
  case VK_LEFT:
    return core::Key::Left;
  case VK_RIGHT:
    return core::Key::Right;
  case VK_UP:
    return core::Key::Up;
  case VK_DOWN:
    return core::Key::Down;
  case VK_HOME:
    return core::Key::Home;
  case VK_END:
    return core::Key::End;
  case VK_PRIOR:
    return core::Key::PageUp;
  case VK_NEXT:
    return core::Key::PageDown;
  default:
    return core::Key::None;
  }
}

// Caractère produit par la touche, disposition comprise (AZERTY, BÉPO…).
// ToUnicodeEx tient compte de l'état réel du clavier ; sans lui, « é » ou « ç »
// ne seraient jamais reconnus comme des lettres.
uint32_t charOf(WPARAM vk, LPARAM lparam) {
  BYTE ks[256]{};
  if (!::GetKeyboardState(ks))
    return 0;
  // Ctrl/Alt neutralisés pour la TRADUCTION : on veut la lettre « r » de
  // Ctrl+Alt+R, pas le caractère de contrôle 0x12.
  ks[VK_CONTROL] = ks[VK_LCONTROL] = ks[VK_RCONTROL] = 0;
  ks[VK_MENU] = ks[VK_LMENU] = ks[VK_RMENU] = 0;
  wchar_t buf[8]{};
  UINT scan = UINT((lparam >> 16) & 0xFF);
  HKL layout = ::GetKeyboardLayout(0);
  // Le drapeau 4 (bit 2) empêche ToUnicodeEx de PERTURBER l'état des touches
  // mortes — sans lui, un accent circonflexe tapé ensuite serait perdu.
  int n = ::ToUnicodeEx(UINT(vk), scan, ks, buf, 8, 4, layout);
  if (n <= 0)
    return 0;
  // Paire de substitution éventuelle → point de code complet.
  if (n >= 2 && buf[0] >= 0xD800 && buf[0] <= 0xDBFF && buf[1] >= 0xDC00 &&
      buf[1] <= 0xDFFF)
    return 0x10000 + ((uint32_t(buf[0]) - 0xD800) << 10) +
           (uint32_t(buf[1]) - 0xDC00);
  return uint32_t(buf[0]);
}

bool isModifierVk(WPARAM vk) {
  return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
         vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
         vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU ||
         vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL;
}

core::KeyEvent buildKey(WPARAM vk, LPARAM lparam) {
  core::KeyEvent k;
  k.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  k.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
  k.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  k.super = ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000) != 0;
  k.key = mapVk(vk, k.shift);
  k.cp = charOf(vk, lparam);
  // Les touches nommées ne sont pas des caractères de mot : VK_SPACE produit
  // 0x20 et VK_BACK 0x08, qu'on ne veut pas voir passer pour du texte.
  if (k.key != core::Key::None && k.key != core::Key::Space)
    k.cp = (vk >= 'A' && vk <= 'Z') ? k.cp : 0;
  k.panelDigit = panelDigitOf(vk, k.cp);
  return k;
}

} // namespace

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext *ctx, WPARAM vk,
                                         LPARAM lparam, BOOL *eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = FALSE;
  // Prédiction coupée depuis l'indicateur : on s'efface complètement, le
  // clavier se comporte comme la disposition nue.
  if (!ctx || isModifierVk(vk) || !enabled_)
    return S_OK;
  core::KeyEvent k = buildKey(vk, lparam);
  core::PredictState &st = stateFor(ctx);

  // Large volontairement : OnKeyDown tranchera. Ce qui compte ici est de ne
  // JAMAIS rater une touche qu'on veut traiter — une touche non annoncée ne
  // nous serait pas transmise du tout.
  bool composing = !st.buffer.empty();
  bool modal = st.langMenu || st.reformulating;
  if (modal)
    *eaten = TRUE;
  else if (composing)
    *eaten = TRUE;
  else if (k.cp && !k.mod() && core::isWordExtender(k.cp, true))
    *eaten = TRUE;
  else if (k.ctrl && k.alt && (k.cp == 'r' || k.cp == 'R'))
    *eaten = TRUE;
  else if (k.ctrl && k.shift && (k.cp == 'l' || k.cp == 'L'))
    *eaten = TRUE;
  else if (bar_.visible() && (k.key == core::Key::Tab ||
                              k.key == core::Key::ShiftTab ||
                              k.key == core::Key::Escape))
    *eaten = TRUE;
  return S_OK;
}

STDMETHODIMP CTextService::OnKeyDown(ITfContext *ctx, WPARAM vk, LPARAM lparam,
                                     BOOL *eaten) {
  if (!eaten)
    return E_INVALIDARG;
  *eaten = FALSE;
  if (!ctx || isModifierVk(vk) || !enabled_)
    return S_OK;

  core::KeyEvent k = buildKey(vk, lparam);
  core::PredictState &st = stateFor(ctx);
  bool consumed = false;

  // TOUT le traitement se fait dans UNE session d'édition : c'est la seule
  // façon d'avoir un cookie valide pour lire et écrire le document.
  HRESULT hrEs = withEditSession(ctx, [&](TfEditCookie ec) {
    TsfFrontend fe(this, ctx, ec);
    core::EngineCore core(fe, prefs_);
    installReformRunner(core, ctx);
    consumed = core.keyEvent(st, k);
  });
  if (FAILED(hrEs))
    dbg("EDIT SESSION REFUSEE hr=0x%08lx vk=0x%02x", (unsigned long)hrEs,
        unsigned(vk));

  // sockErr != 0 = le daemon n'a pas été joint : « aucune suggestion » vient
  // alors du transport, pas du modèle. C'est la distinction qu'on ne peut pas
  // faire depuis l'extérieur du process hôte.
  dbg("key vk=0x%02x cp=U+%04X buf='%s' cands=%d auto='%s' eaten=%d sockErr=%d "
      "sock='%s'",
      unsigned(vk), k.cp, st.buffer.c_str(), int(st.cands.size()),
      st.autocomplete.c_str(), int(consumed), core::lastConnectError(),
      core::daemonSocketPath().c_str());

  *eaten = consumed ? TRUE : FALSE;
  return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext *, WPARAM, LPARAM,
                                       BOOL *eaten) {
  if (eaten)
    *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext *, WPARAM, LPARAM, BOOL *eaten) {
  if (eaten)
    *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP CTextService::OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) {
  if (eaten)
    *eaten = FALSE;
  return S_OK;
}

// La reformulation part sur un thread (réseau : des secondes) et revient par
// la file de messages. Sans ça, Word gèlerait pendant l'appel à l'API.
//
// Le thread ne touche NI au service NI au contexte : il n'emporte que des
// chaînes, la clé opaque du contexte et le MainBridge partagé. Le service
// peut être désactivé, détruit, et la DLL déchargée pendant l'appel réseau —
// runDetached retient le module, et un bridge fermé jette le résultat.
void CTextService::installReformRunner(core::EngineCore &core,
                                       ITfContext *ctx) {
  core.setReformRunner([this, ctx](std::string text, std::string mode,
                                   uint32_t nonce, int n, uint32_t gen) {
    // Référence COM prise ICI, sur le thread de saisie, et rendue par
    // OnPopContext/Deactivate — jamais depuis le thread réseau.
    if (!reformCtx_.count(ctx))
      reformCtx_[ctx] = ctx;
    std::shared_ptr<MainBridge> bridge = bridge_;
    CTextService *self = this;
    runDetached([self, bridge, ctx, text, mode, nonce, n, gen]() {
      auto deliver = [self, bridge, ctx, gen](bool partial,
                                              std::vector<std::string> vars,
                                              core::ReformResult res) {
        bridge->post([self, ctx, partial, vars, res, gen]() {
          // Exécuté sur le thread de saisie, donc `self` est vivant (le bridge
          // est fermé avant sa destruction). Le contexte, lui, a pu fermer.
          auto it = self->reformCtx_.find(ctx);
          if (it == self->reformCtx_.end())
            return;
          Microsoft::WRL::ComPtr<ITfContext> keep = it->second;
          self->withEditSession(keep.Get(), [&](TfEditCookie ec) {
            TsfFrontend fe(self, keep.Get(), ec);
            core::EngineCore c(fe, self->prefs_);
            self->installReformRunner(c, keep.Get());
            if (partial)
              c.onReformPartial(self->stateFor(keep.Get()), vars, gen);
            else
              c.onReformResult(self->stateFor(keep.Get()), res, gen);
          });
        });
      };
      auto onPartial = [&deliver](std::vector<std::string> vars) {
        deliver(true, std::move(vars), {});
      };
      core::ReformResult r =
          core::reformulateDaemon(text, mode, nonce, n, onPartial);
      deliver(false, {}, r);
    });
  });
}

} // namespace win
