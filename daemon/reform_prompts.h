#pragma once
#include <string>

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
