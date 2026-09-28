#include "KeyboardLayout.h"

#include <algorithm>
#include <cwctype>
#include <shlwapi.h>

namespace win::layout {
namespace {

// Langues sous lesquelles le profil Predict est inscrit (cf dllmain.cpp).
const LANGID kProfileLangs[] = {MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH),
                                MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)};

constexpr wchar_t kSubstitutes[] = L"Keyboard Layout\\Substitutes";

std::wstring lower(std::wstring s) {
  for (auto &c : s)
    c = wchar_t(std::towlower(c));
  return s;
}

bool isKlid(const std::wstring &s) {
  if (s.size() != 8)
    return false;
  for (wchar_t c : s)
    if (!std::iswxdigit(c))
      return false;
  return true;
}

// « 0000LLLL » : la disposition de base que TSF associe au profil de langue.
std::wstring baseKlid(LANGID lang) {
  wchar_t buf[9];
  swprintf_s(buf, L"0000%04x", unsigned(lang));
  return buf;
}

bool readString(HKEY root, const wchar_t *sub, const wchar_t *name,
                std::wstring &out) {
  wchar_t buf[512];
  DWORD sz = sizeof(buf);
  if (::RegGetValueW(root, sub, name, RRF_RT_REG_SZ, nullptr, buf, &sz) !=
      ERROR_SUCCESS)
    return false;
  out = buf;
  return true;
}

bool contains(const std::vector<std::wstring> &v, const std::wstring &k) {
  const std::wstring lk = lower(k);
  return std::any_of(v.begin(), v.end(),
                     [&](const std::wstring &s) { return lower(s) == lk; });
}

} // namespace

std::vector<std::wstring> userLayouts(LANGID lang) {
  std::vector<std::wstring> out;
  wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
  if (!::LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), tag,
                          LOCALE_NAME_MAX_LENGTH, 0))
    return out;
  // Liste de langues de Windows 8+ : une valeur par méthode de saisie,
  // nommée « LLLL:KKKKKKKK » (disposition) ou « LLLL:{CLSID}{PROFIL} »
  // (text service), dont la donnée est l'ordre d'affichage.
  std::wstring sub = std::wstring(L"Control Panel\\International\\User Profile\\") + tag;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &key) !=
      ERROR_SUCCESS)
    return out;
  std::vector<std::pair<DWORD, std::wstring>> found;
  for (DWORD i = 0;; i++) {
    wchar_t name[128];
    DWORD nameLen = 128, type = 0, data = 0, dataLen = sizeof(data);
    LONG r = ::RegEnumValueW(key, i, name, &nameLen, nullptr, &type,
                             reinterpret_cast<BYTE *>(&data), &dataLen);
    if (r == ERROR_NO_MORE_ITEMS)
      break;
    if (r != ERROR_SUCCESS || type != REG_DWORD)
      continue;
    std::wstring n(name, nameLen);
    size_t colon = n.find(L':');
    if (colon == std::wstring::npos)
      continue;
    std::wstring klid = n.substr(colon + 1);
    if (isKlid(klid))
      found.emplace_back(data, klid);
  }
  ::RegCloseKey(key);
  std::sort(found.begin(), found.end());
  for (auto &f : found)
    out.push_back(f.second);
  return out;
}

Status current() {
  Status s;
  s.lang = LOWORD(reinterpret_cast<ULONG_PTR>(::GetKeyboardLayout(0)));
  wchar_t klid[KL_NAMELENGTH]{};
  if (::GetKeyboardLayoutNameW(klid))
    s.active = klid;
  auto mine = userLayouts(s.lang);
  if (!mine.empty()) {
    s.wanted = mine.front();
    s.matches = s.active.empty() || contains(mine, s.active);
  }
  return s;
}

std::wstring displayName(const std::wstring &klid) {
  const std::wstring sub =
      L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\" + klid;
  std::wstring indirect;
  if (readString(HKEY_LOCAL_MACHINE, sub.c_str(), L"Layout Display Name",
                 indirect)) {
    wchar_t buf[256]{};
    if (SUCCEEDED(::SHLoadIndirectString(indirect.c_str(), buf, 256, nullptr)))
      return buf;
  }
  std::wstring text;
  if (readString(HKEY_LOCAL_MACHINE, sub.c_str(), L"Layout Text", text))
    return text;
  return klid;
}

int repair(bool apply, std::vector<std::wstring> *plan) {
  int changes = 0;
  for (LANGID lang : kProfileLangs) {
    auto mine = userLayouts(lang);
    if (mine.empty())
      continue; // langue absente des Paramètres : rien à aligner
    const std::wstring base = baseKlid(lang);
    std::wstring effective = base;
    readString(HKEY_CURRENT_USER, kSubstitutes, base.c_str(), effective);
    // Déjà une disposition de l'utilisateur : on ne touche à rien. C'est ce
    // qui garantit qu'on ne modifie JAMAIS une disposition qu'il emploie.
    if (contains(mine, effective))
      continue;
    const std::wstring &want = mine.front();
    if (plan)
      plan->push_back(base + L" : " + effective + L" -> " + want);
    if (!apply) {
      changes++;
      continue;
    }
    LONG r;
    if (lower(want) == lower(base)) {
      // La base EST sa disposition : c'est la substitution qui est périmée.
      r = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, kSubstitutes, base.c_str());
    } else {
      r = ::RegSetKeyValueW(HKEY_CURRENT_USER, kSubstitutes, base.c_str(),
                            REG_SZ, want.c_str(),
                            DWORD((want.size() + 1) * sizeof(wchar_t)));
    }
    if (r == ERROR_SUCCESS)
      changes++;
  }
  return changes;
}

} // namespace win::layout

// Point d'entrée rundll32 — l'installeur et setup-windows.ps1 l'appellent
// dans le contexte de l'utilisateur :
//   rundll32.exe predict-tsf.dll,FixKeyboardLayouts
extern "C" void CALLBACK FixKeyboardLayoutsW(HWND, HINSTANCE, LPWSTR, int) {
  win::layout::repair();
}
