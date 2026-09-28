#include "KeyboardLayout.h"

#include <algorithm>
#include <cwctype>
#include <shlwapi.h>

namespace win::layout {
namespace {

constexpr wchar_t kLayouts[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";

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
    s.matches = s.active.empty() || isUserLayout(s.active, mine);
  }
  return s;
}

bool isUserLayout(const std::wstring &klid,
                  const std::vector<std::wstring> &mine) {
  return contains(mine, klid);
}

std::wstring displayName(const std::wstring &klid) {
  const std::wstring sub = std::wstring(kLayouts) + L"\\" + klid;
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

HKL substituteFrom(LANGID lang, const std::vector<std::wstring> &mine) {
  for (const std::wstring &k : mine) {
    if (!isKlid(k))
      continue;
    // Mot bas du KLID = langue de la disposition (0000040C, 0001040C « AZERTY
    // standard »… sont françaises). Une autre langue serait ignorée par TSF.
    const unsigned long klid = std::wcstoul(k.c_str(), nullptr, 16);
    if (LOWORD(klid) == lang)
      return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(klid));
  }
  return nullptr;
}

HKL substituteFor(LANGID lang) { return substituteFrom(lang, userLayouts(lang)); }

} // namespace win::layout
