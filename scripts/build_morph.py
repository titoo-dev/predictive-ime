#!/usr/bin/env python3
"""Extrait de Lefff une table morphologique compacte, restreinte au vocabulaire
du modèle.

Entrée  : lefff_morpho-3.5.json (array d'objets {form, lemma, category,
          msfeatures, ...}), words.tsv (1re colonne = forme).
Sortie  (stdout) : `forme<TAB>genre(m|f|-)<TAB>nombre(s|p|-)<TAB>lemme`, une
          ligne par forme du vocab connue de Lefff comme nom commun ou adjectif.

msfeatures Lefff pour nc/adj : ms/mp/fs/fp (genre m/f + nombre s/p), parfois
seulement s|p (nombre) ou m|f (genre), ou vide (invariable). Une forme peut
avoir plusieurs analyses (nc + adj, ou épicène ms+fs) : on agrège — genre/nombre
mis à '-' si ambigu entre analyses (ex. « élève » ms+fs → genre indéterminé).
"""
import json
import sys
import collections

lefff_path, words_path = sys.argv[1], sys.argv[2]

vocab = set()
for line in open(words_path, encoding="utf-8"):
    parts = line.split()
    if parts:
        vocab.add(parts[0])

agg = collections.defaultdict(lambda: {"g": set(), "n": set(), "lemma": None})
for e in json.load(open(lefff_path, encoding="utf-8")):
    if e.get("category") not in ("nc", "adj"):  # noms communs + adjectifs
        continue
    form = (e.get("form") or "").lower()
    if form not in vocab:
        continue
    feats = e.get("msfeatures") or ""
    g = "m" if "m" in feats else "f" if "f" in feats else None
    n = "p" if "p" in feats else "s" if "s" in feats else None
    a = agg[form]
    if g:
        a["g"].add(g)
    if n:
        a["n"].add(n)
    if a["lemma"] is None:
        a["lemma"] = (e.get("lemma") or form)

for form in sorted(agg):
    a = agg[form]
    g = next(iter(a["g"])) if len(a["g"]) == 1 else "-"
    n = next(iter(a["n"])) if len(a["n"]) == 1 else "-"
    if g == "-" and n == "-":
        continue
    print(f"{form}\t{g}\t{n}\t{a['lemma']}")
