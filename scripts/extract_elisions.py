#!/usr/bin/env python3
"""Extrait du corpus les élisions combinées (j'ai, c'est, qu'il, aujourd'hui…)
et les CONTRACTIONS ANGLAISES (don't, i'm, it's, you're…) absentes de words.tsv.

Les listes de fréquence SCINDENT ces formes (fr50k : "j'" + "ai" ; en50k :
"don" + "'t"), alors que le tokeniseur des n-grammes (TOK, ci-dessous,
identique à build_ngrams.py) les garde COMBINÉES. Résultat : « j'ai » et
« don't » sont hors-vocab → build_ngrams les jette → la forme disparaît du
modèle ET casse chaque n-gramme qui la traverse. On les rétablit en les
ajoutant à words.tsv AVANT build_ngrams.

Fréquence : on REDISTRIBUE la masse de l'ANCRE (le fragment que la liste de
fréquence connaît : proclitique "j'" côté FR, suffixe "'t"/"'s"… côté EN) sur
ses formes combinées au prorata de leur fréquence corpus — ça met « j'ai » et
« don't » sur la même échelle que le reste du vocabulaire. Ancres absentes de
words.tsv (jusqu', aujourd'…) : facteur d'échelle médian.

Sortie (stdout) : `forme freq fr|en`, à APPENDRE à words.tsv.
"""
import re
import sys
import collections
import statistics

words_path = sys.argv[1]
sentence_files = sys.argv[2:]

TOK = re.compile(r"[^\W\d_]+(?:['-][^\W\d_]+)*", re.UNICODE)
PROCLITICS = ("j", "m", "t", "s", "l", "d", "c", "n", "qu", "jusqu", "lorsqu",
              "puisqu", "quoiqu", "presqu", "aujourd")
ELISION = re.compile(r"^(?:" + "|".join(PROCLITICS) + r")'.+", re.UNICODE)
# Contractions anglaises : mot + suffixe clitique ('s, n't, 'm, 're, 'll, 've,
# 'd). Les suffixes sont EUX-MÊMES dans words.tsv (tokens scindés de en50k :
# "'s" 14M, "'t" 9,6M…) — ils servent d'ancre de fréquence.
CONTRACTION = re.compile(r".+'(?:s|t|m|re|ll|ve|d)$", re.UNICODE)
MIN_COUNT = 30


def anchor_of(tok, lang):
    # fr : proclitique = préfixe jusqu'à la 1re apostrophe incluse (« j' »)
    # en : suffixe depuis la dernière apostrophe (« 't » de don't)
    if lang == "fr":
        return tok[: tok.find("'") + 1]
    return tok[tok.rfind("'"):]


existing = {}
for line in open(words_path, encoding="utf-8"):
    p = line.split()
    if len(p) >= 2:
        try:
            existing[p[0]] = int(p[1])
        except ValueError:
            pass

count = collections.Counter()
langs = {}  # tok -> "fr" | "en" (le FR a priorité : « n't » matche n' avant 't)
for path in sentence_files:
    for line in open(path, encoding="utf-8", errors="ignore"):
        tab = line.find("\t")
        text = (line[tab + 1:] if tab >= 0 else line).replace("’", "'").lower()
        for t in TOK.findall(text):
            if ELISION.match(t):
                count[t] += 1
                langs[t] = "fr"
            elif CONTRACTION.match(t):
                count[t] += 1
                langs[t] = "en"

tot = collections.Counter()
for tok, c in count.items():
    tot[anchor_of(tok, langs[tok])] += c

scales = {}
anchored = []
for proc, t in tot.items():
    f = existing.get(proc)
    if f and t:
        scales[proc] = f / t
        anchored.append(f / t)
med = statistics.median(anchored) if anchored else 1000.0
for proc in tot:
    scales.setdefault(proc, med)

for tok, c in sorted(count.items(), key=lambda x: -x[1]):
    if c < MIN_COUNT or tok in existing:
        continue
    anchor = anchor_of(tok, langs[tok])
    freq = max(1, round(scales[anchor] * c))
    cap = existing.get(anchor)  # une forme ne dépasse pas son ancre
    if cap:
        freq = min(freq, cap)
    print(f"{tok} {freq} {langs[tok]}")
