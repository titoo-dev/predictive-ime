// Tests de non-régression du text service Windows.
//
// Chaque cas rejoue une panne réellement rencontrée :
//   - disposition : Predict tapait en QWERTY sur une machine AZERTY (profil
//     sans disposition déclarée, puis déclaration interdite entre langues) ;
//   - position : la barre partait « n'importe où » sur un caret aberrant
//     (TS_E_NOLAYOUT, rectangle du document, repère client) ;
//   - placement : alignement sur le mot, bascule au-dessus en bas d'écran,
//     recalage au bord droit, pas de saut quand les candidats changent.
//
//   test_win_tsf              → tests autonomes (CI)
//   test_win_tsf --installed  → vérifie l'IME INSTALLÉ sur cette machine :
//                               Predict actif doit taper avec la disposition
//                               de l'utilisateur. Code 77 (sauté) si Predict
//                               n'est ni inscrit ni dans la liste de langues.
#include "../tsf/CandidateWindow.h"
#include "../tsf/Dpi.h"
#include "../tsf/Guids.h"
#include "../tsf/KeyboardLayout.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <msctf.h>
#include <string>
#include <vector>

namespace {

int g_run = 0, g_fail = 0;

void check(bool ok, const char *what, const char *file, int line) {
  g_run++;
  if (!ok) {
    g_fail++;
    std::printf("  ÉCHEC %s:%d  %s\n", file, line, what);
  }
}
#define CHECK(c) check((c), #c, __FILE__, __LINE__)

void checkEq(long got, long want, const char *what, const char *file,
             int line) {
  g_run++;
  if (got != want) {
    g_fail++;
    std::printf("  ÉCHEC %s:%d  %s = %ld, attendu %ld\n", file, line, what, got,
                want);
  }
}
#define CHECK_EQ(a, b) checkEq(long(a), long(b), #a, __FILE__, __LINE__)

HKL klidHkl(unsigned long klid) {
  return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(klid));
}

void pump(int ms) {
  ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG m;
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&m);
      ::DispatchMessageW(&m);
    }
    ::Sleep(5);
  }
}

// ------------------------------------------------------------ disposition --

void testSubstituteLayout() {
  std::printf("disposition déclarée au profil\n");
  using win::layout::substituteFrom;
  const LANGID fr = 0x040C, en = 0x0409;
  // AZERTY (hérité) et AZERTY standard : français → déclarables.
  CHECK(substituteFrom(fr, {L"0000040C"}) == klidHkl(0x0000040C));
  CHECK(substituteFrom(fr, {L"0001040C"}) == klidHkl(0x0001040C));
  // La première disposition DE LA LANGUE, pas la première tout court.
  CHECK(substituteFrom(fr, {L"00000409", L"0000040C"}) == klidHkl(0x0000040C));
  // Anglais en AZERTY : TSF refuse une disposition d'une autre langue → rien
  // à déclarer (et le setup ne propose pas Predict EN).
  CHECK(substituteFrom(en, {L"0000040C"}) == nullptr);
  CHECK(substituteFrom(en, {L"00020409"}) == klidHkl(0x00020409));
  CHECK(substituteFrom(fr, {}) == nullptr);
  CHECK(substituteFrom(fr, {L"n'importe", L"{5F0A1C7E}"}) == nullptr);
}

void testLayoutMatch() {
  std::printf("disposition active vs celles de l'utilisateur\n");
  using win::layout::isUserLayout;
  // LA régression : Predict actif en US alors que l'utilisateur est en AZERTY.
  CHECK(!isUserLayout(L"00000409", {L"0000040C"}));
  CHECK(isUserLayout(L"0000040c", {L"0000040C"})); // casse indifférente
  CHECK(isUserLayout(L"0001040C", {L"0000040C", L"0001040C"}));
  CHECK(!isUserLayout(L"0000040C", {}));
}

// ------------------------------------------------------- caret → physique --

void testCaretValidation() {
  std::printf("validation du caret (dpi::toPhysical)\n");
  HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"hôte", WS_POPUP,
                                200, 200, 600, 400, nullptr, nullptr, nullptr,
                                nullptr);
  CHECK(host != nullptr);
  RECT out{};
  // Caret dans la fenêtre : accepté, inchangé (process Per-Monitor).
  RECT in{300, 300, 302, 320};
  CHECK(win::dpi::toPhysical(host, in, out));
  CHECK_EQ(out.left, 300);
  CHECK_EQ(out.bottom, 320);
  // Les rectangles qui envoyaient la barre « n'importe où » :
  RECT origin{0, 0, 2, 20};         // repère client / TS_E_NOLAYOUT → (0,0)
  RECT distant{5000, 5000, 5002, 5020}; // autre écran, autre repère
  RECT document{210, 150, 790, 700}; // plus haut que la fenêtre = document
  RECT empty{300, 300, 300, 300};
  CHECK(!win::dpi::toPhysical(host, origin, out));
  CHECK(!win::dpi::toPhysical(host, distant, out));
  CHECK(!win::dpi::toPhysical(host, document, out));
  CHECK(!win::dpi::toPhysical(host, empty, out));
  CHECK(!win::dpi::toPhysical(nullptr, in, out));
  // Marge de bord (listes déroulantes, coins arrondis) : juste sous la fenêtre.
  RECT below{300, 600, 302, 620};
  CHECK(win::dpi::toPhysical(host, below, out));
  ::DestroyWindow(host);
}

// ----------------------------------------------------- placement de la barre --

HWND findBar() {
  HWND found = nullptr;
  ::EnumThreadWindows(
      ::GetCurrentThreadId(),
      [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        ::GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"PredictImeCandidateBar")) {
          *reinterpret_cast<HWND *>(lp) = h;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  return found;
}

void testBarPlacement() {
  std::printf("placement de la barre\n");
  win::CandidateWindow bar;
  const std::vector<core::Candidate> words = {
      {"bonjour", false, ""}, {"bonsoir", false, ""}, {"bonheur", false, ""}};
  const win::PanelHints typing{true, false};

  RECT caret{400, 300, 402, 320};
  bar.show(words, -1, "", caret, typing);
  HWND h = findBar();
  CHECK(h != nullptr && ::IsWindowVisible(h));
  if (!h)
    return;
  const double k = ::GetDpiForWindow(h) / 96.0;
  const long lead = std::lround(27 * k), gap = std::lround(6 * k);
  RECT r{};
  ::GetWindowRect(h, &r);
  // Texte du 1er candidat aligné sur le mot, 6 DIP sous la ligne.
  CHECK_EQ(r.left, caret.left - lead);
  CHECK_EQ(r.top, caret.bottom + gap);

  // Les candidats changent à chaque touche : la barre ne doit PAS bouger.
  bar.show({{"bonjour", true, ""}, {"bon", false, ""}}, -1, "", caret, typing);
  RECT r2{};
  ::GetWindowRect(h, &r2);
  CHECK_EQ(r2.left, r.left);
  CHECK_EQ(r2.top, r.top);

  // Navigation (pastille) : même position.
  bar.show(words, 1, "", caret, typing);
  ::GetWindowRect(h, &r2);
  CHECK_EQ(r2.left, r.left);

  // Suivi du texte (fenêtre déplacée / défilement).
  RECT moved{520, 380, 522, 400};
  bar.moveTo(moved);
  ::GetWindowRect(h, &r2);
  CHECK_EQ(r2.left, moved.left - lead);
  CHECK_EQ(r2.top, moved.bottom + gap);

  HMONITOR mon = ::MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  ::GetMonitorInfoW(mon, &mi);
  const RECT wa = mi.rcWork;

  // Bas d'écran : la barre passe AU-DESSUS du texte, sans le recouvrir.
  RECT low{400, wa.bottom - 20, 402, wa.bottom};
  bar.show(words, -1, "", low, typing);
  ::GetWindowRect(h, &r2);
  CHECK(r2.bottom <= low.top);
  CHECK(r2.top >= wa.top);

  // Bord droit : recalée dans la zone de travail.
  RECT right{wa.right - 5, 300, wa.right - 3, 320};
  bar.show(words, -1, "", right, typing);
  ::GetWindowRect(h, &r2);
  CHECK(r2.right <= wa.right);
  CHECK(r2.left >= wa.left);

  // Liste de reformulation : même ancrage vertical, largeur fixe.
  bar.show({{"Première variante.", false, "1"}, {"Seconde.", false, "2"}}, 0,
           "Formel", caret, {false, false});
  ::GetWindowRect(h, &r2);
  CHECK_EQ(r2.top, caret.bottom + gap);
  CHECK(r2.right - r2.left >= long(400 * k));

  bar.hide();
  CHECK(!::IsWindowVisible(h));
}

// --------------------------------------------- IME installé (machine locale) --

bool predictInLanguageList(LANGID lang) {
  wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
  if (!::LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), tag,
                          LOCALE_NAME_MAX_LENGTH, 0))
    return false;
  wchar_t clsid[64], prof[64], name[160];
  ::StringFromGUID2(CLSID_PredictTextService, clsid, 64);
  ::StringFromGUID2(GUID_PredictProfile, prof, 64);
  swprintf_s(name, L"%04X:%s%s", unsigned(lang), clsid, prof);
  std::wstring sub =
      std::wstring(L"Control Panel\\International\\User Profile\\") + tag;
  DWORD v = 0, sz = sizeof(v);
  return ::RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_DWORD,
                        nullptr, &v, &sz) == ERROR_SUCCESS;
}

int testInstalled() {
  std::printf("IME installé : disposition de Predict actif\n");
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ITfThreadMgr *tm = nullptr;
  TfClientId cid = 0;
  ITfInputProcessorProfileMgr *pm = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                (void **)&tm)) ||
      FAILED(tm->Activate(&cid)) ||
      FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfileMgr, (void **)&pm))) {
    std::printf("  TSF indisponible — sauté\n");
    return 77;
  }
  int tested = 0;
  for (LANGID lang : {LANGID(0x040C), LANGID(0x0409)}) {
    TF_INPUTPROCESSORPROFILE p{};
    if (!predictInLanguageList(lang) ||
        FAILED(pm->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, lang,
                              CLSID_PredictTextService, GUID_PredictProfile,
                              nullptr, &p)))
      continue;
    tested++;
    HRESULT hr = pm->ActivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR, lang, CLSID_PredictTextService,
        GUID_PredictProfile, nullptr, TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
    pump(300);
    TF_INPUTPROCESSORPROFILE act{};
    pm->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &act);
    win::layout::Status s = win::layout::current();
    auto mine = win::layout::userLayouts(lang);
    std::printf("  %04x : actif=%ls voulu=%ls déclaré=%p\n", unsigned(lang),
                s.active.c_str(), mine.empty() ? L"?" : mine[0].c_str(),
                static_cast<void *>(p.hklSubstitute));
    CHECK(SUCCEEDED(hr));
    // Predict reste le profil actif (une correction de disposition depuis le
    // service l'éjectait).
    CHECK(act.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
          IsEqualCLSID(act.clsid, CLSID_PredictTextService));
    // Et il tape avec la disposition de l'utilisateur, pas en US.
    CHECK(mine.empty() || win::layout::isUserLayout(s.active, mine));
  }
  pm->Release();
  tm->Deactivate();
  tm->Release();
  if (!tested) {
    std::printf("  Predict ni inscrit ni dans la liste de langues — sauté\n");
    return 77;
  }
  return -1; // continuer : bilan commun
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (argc > 1 && !wcscmp(argv[1], L"--installed")) {
    int r = testInstalled();
    if (r == 77)
      return 77;
  } else {
    testSubstituteLayout();
    testLayoutMatch();
    testCaretValidation();
    testBarPlacement();
  }
  std::printf("%d vérifications, %d échec(s)\n", g_run, g_fail);
  return g_fail ? 1 : 0;
}
