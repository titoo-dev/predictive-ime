#pragma once
#include <cctype>
#include <string>

// Langue d'une phrase (FR par défaut = langue primaire de l'utilisateur) :
// accents latins fréquents en FR → FR certain ; sinon vote des mots vides FR
// vs EN. PARTAGÉ entre le backend Groq (reformulate_http.cpp) et le neural
// local (neural.cpp). NB : la langue CHOISIE (cfg.lang "fr"/"en") prime sur
// cette heuristique — elle ne sert qu'en "auto".
inline bool reformIsFrench(const std::string &s) {
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    if ((unsigned char)s[i] == 0xC3) {
      unsigned char b = (unsigned char)s[i + 1];
      // é è ê ë à â ç ô ö ù û ü î ï (formes minuscules courantes)
      if (b == 0xA9 || b == 0xA8 || b == 0xAA || b == 0xAB || b == 0xA0 ||
          b == 0xA2 || b == 0xA7 || b == 0xB4 || b == 0xB6 || b == 0xB9 ||
          b == 0xBB || b == 0xBC || b == 0xAE || b == 0xAF)
        return true; // un accent FR → quasi-certain français
    }
  }
  std::string low;
  low.reserve(s.size() + 2);
  low += ' ';
  for (char c : s)
    low += (std::isalpha((unsigned char)c) ? (char)std::tolower((unsigned char)c)
                                           : ' ');
  low += ' ';
  static const char *fr[] = {" le ", " la ", " les ", " un ", " une ", " des ",
                             " je ", " tu ", " est ", " et ", " pour ", " que ",
                             " pas ", " vous ", " ne ", " dans ", " avec ",
                             " ce ", " sur ", " au ", " du ", " ca ", nullptr};
  static const char *en[] = {" the ", " is ", " are ", " you ", " for ", " and ",
                             " of ", " to ", " that ", " with ", " would ",
                             " like ", " this ", " it ", " on ", " be ",
                             " have ", " do ", " can ", " your ", " my ", nullptr};
  auto count = [&](const char **ws) {
    int n = 0;
    for (int i = 0; ws[i]; ++i)
      for (size_t p = low.find(ws[i]); p != std::string::npos;
           p = low.find(ws[i], p + 1))
        ++n;
    return n;
  };
  return count(fr) >= count(en); // égalité → FR (langue primaire)
}

// Prompt système de reformulation pour (mode, langue source FR?, nombre de
// variantes). PARTAGÉ entre le backend Groq (reformulate_http.cpp) et le neural
// local (neural.cpp) → sémantique identique quelle que soit la source.
//
// Modes : rephrase (défaut) | formal | simple | short | correct | translate.
//   - translate : la SORTIE est dans l'AUTRE langue (FR→EN, EN→FR).
//   - correct   : corrige orthographe/grammaire SANS changer le style.
//   - les autres : même sens, même langue, ponctuation finale conservée.
inline std::string reformSystemPrompt(const std::string &mode, bool fr,
                                      int count) {
  const std::string n = std::to_string(count);

  if (mode == "translate") {
    return fr ? ("Traduis la phrase de l'utilisateur en ANGLAIS. Donne " + n +
                 " traductions naturelles, une par ligne. Garde le sens et la "
                 "ponctuation finale. Pas de numéro, pas de commentaire, pas de "
                 "guillemets.")
              : ("Translate the user's sentence into FRENCH. Give " + n +
                 " natural translations, one per line. Keep the meaning and the "
                 "final punctuation. No numbering, no commentary, no quotes.");
  }

  if (mode == "correct") {
    return fr ? ("Corrige UNIQUEMENT l'orthographe, la grammaire et la "
                 "ponctuation. Ne change PAS le style ni le choix des mots. "
                 "Donne " + n + " versions corrigées, une par ligne, même "
                 "langue (français). Conserve la ponctuation finale. Pas de "
                 "numéro, pas de commentaire, pas de guillemets.")
              : ("Fix ONLY spelling, grammar and punctuation. Do NOT change the "
                 "style or word choice. Give " + n + " corrected versions, one "
                 "per line, same language (English). Keep the final punctuation. "
                 "No numbering, no commentary, no quotes.");
  }

  std::string instrFr, instrEn;
  if (mode == "formal") {
    instrFr = "dans un registre plus FORMEL et professionnel";
    instrEn = "in a more FORMAL, professional register";
  } else if (mode == "simple") {
    instrFr = "de façon plus SIMPLE et claire (mots courants, phrases courtes)";
    instrEn = "in a SIMPLER, clearer way (common words, short sentences)";
  } else if (mode == "short") {
    instrFr = "de façon plus COURTE et concise, sans rien perdre d'essentiel";
    instrEn = "more CONCISE and shorter, without losing anything essential";
  } else { // rephrase (défaut)
    instrFr = "en variant la formulation";
    instrEn = "varying the wording";
  }

  return fr ? ("Réécris la phrase de l'utilisateur " + instrFr + ". Donne " + n +
               " reformulations différentes, une par ligne. Garde EXACTEMENT le "
               "même sens et la même langue (français) — ne traduis pas. "
               "Conserve la ponctuation finale (. ? !). Pas de numéro, pas de "
               "commentaire, pas de guillemets.")
            : ("Rewrite the user's sentence " + instrEn + ". Give " + n +
               " different paraphrases, one per line. Keep EXACTLY the same "
               "meaning and the same language (English) — do not translate. "
               "Keep the final punctuation (. ? !). No numbering, no commentary, "
               "no quotes.");
}
