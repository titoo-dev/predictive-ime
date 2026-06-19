#!/usr/bin/env python3
"""Extrait du corpus les élisions combinées (j'ai, c'est, qu'il, aujourd'hui…)
absentes de words.tsv.

Les listes de fréquence (fr50k) SCINDENT les élisions ("j'" + "ai"), alors que
le tokeniseur des n-grammes (TOK, ci-dessous, identique à build_ngrams.py) les
garde COMBINÉES. Résultat : « j'ai » est hors-vocab → build_ngrams le jette →
toute élision disparaît du modèle, et la complétion de « j' » ne propose rien
d'utile. On rétablit ces formes en les ajoutant à words.tsv AVANT build_ngrams.

Fréquence : on REDISTRIBUE la masse du proclitique (sa fréquence dans words.tsv,
ex. "j'" = 2982375) sur ses élisions au prorata de leur fréquence corpus — ça
met « j'ai » sur la même échelle que le reste du vocabulaire. Proclitiques
absents de words.tsv (jusqu', aujourd'…) : facteur d'échelle médian.

Sortie (stdout) : `élision freq fr`, à APPENDRE à words.tsv.
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
MIN_COUNT = 30


def proc_of(tok):  # proclitique = préfixe jusqu'à la 1re apostrophe incluse
    return tok[: tok.find("'") + 1]


existing = {}
for line in open(words_path, encoding="utf-8"):
    p = line.split()
    if len(p) >= 2:
        try:
            existing[p[0]] = int(p[1])
        except ValueError:
            pass

count = collections.Counter()
for path in sentence_files:
    for line in open(path, encoding="utf-8", errors="ignore"):
        tab = line.find("\t")
        text = (line[tab + 1:] if tab >= 0 else line).replace("’", "'").lower()
        for t in TOK.findall(text):
            if ELISION.match(t):
                count[t] += 1

tot = collections.Counter()
for tok, c in count.items():
    tot[proc_of(tok)] += c

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
    proc = proc_of(tok)
    freq = max(1, round(scales[proc] * c))
    cap = existing.get(proc)  # une élision ne dépasse pas son proclitique
    if cap:
        freq = min(freq, cap)
    print(f"{tok} {freq} fr")
