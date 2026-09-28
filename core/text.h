// Cœur portable — UTF-8, casse, classification des caractères de mot.
//
// Extrait TEL QUEL de engine/predict.cpp : aucune dépendance à fcitx5 ni à
// Win32, donc partagé par l'engine fcitx5 (Linux) et le text service TSF
// (Windows). C'est la couche où un écart entre les deux frontaux passerait
// INAPERÇU le plus longtemps — d'où la mise en commun en premier.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

// ----------------------------------------------------------------- UTF-8 ----

inline void appendCp(std::string &s, uint32_t cp) {
  if (cp < 0x80)
    s.push_back(char(cp));
  else if (cp < 0x800) {
    s.push_back(char(0xC0 | (cp >> 6)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    s.push_back(char(0xE0 | (cp >> 12)));
    s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    s.push_back(char(0xF0 | (cp >> 18)));
    s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    s.push_back(char(0x80 | (cp & 0x3F)));
  }
}

inline std::vector<uint32_t> decodeUtf8(const std::string &s) {
  std::vector<uint32_t> cps;
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0xF;
      len = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x7;
      len = 4;
    } else {
      i++;
      continue;
    }
    if (i + len > n)
      break;
    for (int k = 1; k < len; k++)
      cp = (cp << 6) | (s[i + k] & 0x3F);
    cps.push_back(cp);
    i += len;
  }
  return cps;
}

// Retire le dernier point de code UTF-8 (pour Backspace).
inline void popLastCp(std::string &s) {
  if (s.empty())
    return;
  size_t i = s.size();
  do {
    --i;
  } while (i > 0 && (uint8_t(s[i]) & 0xC0) == 0x80);
  s.erase(i);
}

// UTF-8 ↔ UTF-16 : la frontière Windows parle UTF-16 (TSF, Win32), le cœur et
// le protocole du daemon parlent UTF-8. Sans surrogates corrects, les emoji
// (hors BMP) ressortiraient cassés du picker.
inline std::u16string toUtf16(const std::string &s) {
  std::u16string out;
  for (uint32_t cp : decodeUtf8(s)) {
    if (cp < 0x10000) {
      out.push_back(char16_t(cp));
    } else {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    }
  }
  return out;
}

inline std::string fromUtf16(const std::u16string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
        s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    appendCp(out, cp);
  }
  return out;
}

// Nombre d'unités UTF-16 d'une chaîne UTF-8 — c'est CETTE unité que comptent
// les API Windows (ITfRange, SendInput), pas les points de code : un emoji
// vaut 2. Confondre les deux décale toutes les suppressions de revert.
inline size_t utf16Length(const std::string &s) {
  size_t n = 0;
  for (uint32_t cp : decodeUtf8(s))
    n += cp < 0x10000 ? 1 : 2;
  return n;
}

// --------------------------------------------------------------- casse -------

inline bool isUpperCp(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7);
}
inline bool isLowerCp(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7);
}
inline bool isLetterCp(uint32_t cp) {
  return isUpperCp(cp) || isLowerCp(cp) || (cp >= 0x100 && cp <= 0x24F);
}
inline uint32_t toUpperCp(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z')
    return cp - 32;
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
    return cp - 0x20;
  return cp;
}

// Caractère qui prolonge un mot : lettre toujours ; apostrophe / trait d'union /
// chiffre seulement si le buffer est déjà entamé (mot en cours). ':' sur buffer
// VIDE démarre le picker emoji (":coeur" → ❤️), ';' un SNIPPET (";mail" →
// expansion) — jamais en milieu de mot, donc "10:30" ou "voici :" tapent
// normalement.
inline bool isWordExtender(uint32_t cp, bool bufferEmpty) {
  if (isLetterCp(cp))
    return true;
  if (bufferEmpty)
    return cp == ':' || cp == ';';
  return cp == '\'' || cp == 0x2019 || cp == '-' || (cp >= '0' && cp <= '9');
}

// Caractère qui fait partie d'un mot DÉJÀ écrit (recomposition au Backspace).
inline bool isWordCp(uint32_t c) {
  return isLetterCp(c) || c == '\'' || c == 0x2019 || c == '-';
}

// Buffer « déclencheur » (emoji ':' ou snippet ';') : pas d'apprentissage de
// bigramme ni de contexte — ce n'est pas de la prose.
inline bool isTriggerBuffer(const std::string &buffer) {
  return !buffer.empty() && (buffer[0] == ':' || buffer[0] == ';');
}

// Majuscule sur la première lettre (auto-capitalisation en début de phrase).
inline std::string capFirst(const std::string &w) {
  auto cps = decodeUtf8(w);
  std::string out;
  bool done = false;
  for (uint32_t cp : cps) {
    if (!done && isLetterCp(cp)) {
      appendCp(out, toUpperCp(cp));
      done = true;
    } else {
      appendCp(out, cp);
    }
  }
  return out;
}

// Reporte la casse du `buffer` tapé sur un candidat (minuscule du modèle).
// "Bonjou" → "Bonjour" ; "FRAN" → "FRANÇAIS" ; "le" → inchangé.
// Anglais : « i » seul et les contractions « i'… » (i'm, i'll, i've, i'd)
// prennent TOUJOURS la majuscule — aucun mot français n'est « i » ni ne
// commence par « i' », la règle est donc sûre sans condition de langue.
inline std::string applyCase(const std::string &cand_,
                             const std::string &buffer) {
  std::string cand = cand_;
  if (cand == "i" || cand.rfind("i'", 0) == 0)
    cand[0] = 'I';
  auto bcps = decodeUtf8(buffer);
  bool firstUpper = false, allUpper = true;
  int letters = 0;
  for (uint32_t cp : bcps) {
    if (!isLetterCp(cp))
      continue;
    if (letters == 0)
      firstUpper = isUpperCp(cp);
    if (!isUpperCp(cp))
      allUpper = false;
    letters++;
  }
  if (letters == 0)
    return cand;
  auto ccps = decodeUtf8(cand);
  std::string out;
  if (allUpper && letters >= 2) {
    for (uint32_t cp : ccps)
      appendCp(out, toUpperCp(cp));
  } else if (firstUpper) {
    bool done = false;
    for (uint32_t cp : ccps) {
      if (!done && isLetterCp(cp)) {
        appendCp(out, toUpperCp(cp));
        done = true;
      } else
        appendCp(out, cp);
    }
  } else {
    return cand;
  }
  return out;
}

// ---------------------------------------------------------- typographie -----

// Ponctuation « haute » qui prend une fine insécable (U+202F) AVANT, en
// typographie française : point-virgule, deux-points, point d'exclamation,
// point d'interrogation et guillemet fermant « » » (U+00BB).
inline bool needsFrenchThinBefore(uint32_t cp) {
  return cp == ';' || cp == ':' || cp == '!' || cp == '?' || cp == 0x00BB;
}

constexpr const char *kThinNbsp = "\xE2\x80\xAF";          // U+202F
constexpr const char *kOpenGuillemetThin = "\xC2\xAB\xE2\x80\xAF"; // « + U+202F

// Les `maxWords` derniers mots d'un texte (pour amorcer le contexte depuis le
// texte environnant de l'application). S'ARRÊTE aux fins de phrase « . ! ? » :
// le contexte ne traverse jamais une frontière de phrase.
inline std::vector<std::string> lastWords(const std::vector<uint32_t> &cps,
                                          int maxWords) {
  std::vector<std::string> out;
  size_t i = cps.size();
  while (i > 0 && (int)out.size() < maxWords) {
    while (i > 0 && !isLetterCp(cps[i - 1])) {
      uint32_t c = cps[i - 1];
      if (c == '.' || c == '!' || c == '?')
        return out; // frontière de phrase
      --i;          // saute les non-lettres
    }
    size_t end = i;
    while (i > 0 && isWordCp(cps[i - 1]))
      --i;
    if (end > i) {
      std::string w;
      for (size_t j = i; j < end; j++)
        appendCp(w, cps[j]);
      out.push_back(w);
    }
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// Minuscule ASCII — comparaison de nom de programme (nextWordBarExclude).
inline std::string lowerAscii(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}

} // namespace core
