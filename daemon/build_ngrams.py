#!/usr/bin/env python3
# Construit bigrams.tsv (w1<TAB>w2<TAB>count) ET trigrams.tsv
# (w1<TAB>w2<TAB>w3<TAB>count) depuis des fichiers de phrases Leipzig (format
# "id<TAB>phrase"). Ne garde que les n-grammes dont TOUS les mots sont dans le
# vocabulaire (words.tsv) et dont le compte dépasse un seuil, pour rester
# pertinent et compact. Normalise apostrophes courbes → droites et minuscule.
#
# usage: build_ngrams.py <words.tsv> <outdir> <sentences.txt>...
import sys, re, collections

MIN_BI = 3   # un bigramme doit apparaître >= 3 fois
MIN_TRI = 2  # un trigramme (plus rare) >= 2 fois
TOK = re.compile(r"[^\W\d_]+(?:['-][^\W\d_]+)*", re.UNICODE)

vocabpath, outdir = sys.argv[1], sys.argv[2]
sentence_files = sys.argv[3:]

vocab = set()
with open(vocabpath, encoding="utf-8") as f:
    for line in f:
        p = line.split()
        if p:
            vocab.add(p[0])

bi = collections.Counter()
tri = collections.Counter()
for path in sentence_files:
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            text = line[tab + 1:] if tab >= 0 else line
            text = text.replace("’", "'").lower()
            toks = TOK.findall(text)
            for i, w in enumerate(toks):
                if w not in vocab:
                    continue
                if i >= 1 and toks[i - 1] in vocab:
                    bi[(toks[i - 1], w)] += 1
                if i >= 2 and toks[i - 1] in vocab and toks[i - 2] in vocab:
                    tri[(toks[i - 2], toks[i - 1], w)] += 1

with open(outdir + "/bigrams.tsv", "w", encoding="utf-8") as f:
    n = 0
    for (a, b), c in bi.items():
        if c >= MIN_BI:
            f.write(f"{a}\t{b}\t{c}\n")
            n += 1
    print(f"bigrams.tsv: {n} bigrammes (>= {MIN_BI})", file=sys.stderr)

with open(outdir + "/trigrams.tsv", "w", encoding="utf-8") as f:
    n = 0
    for (a, b, d), c in tri.items():
        if c >= MIN_TRI:
            f.write(f"{a}\t{b}\t{d}\t{c}\n")
            n += 1
    print(f"trigrams.tsv: {n} trigrammes (>= {MIN_TRI})", file=sys.stderr)
