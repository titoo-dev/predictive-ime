#!/usr/bin/env python3
# Construit le modèle n-gram KNESER-NEY INTERPOLÉ précalculé depuis des fichiers
# de phrases (format Leipzig "id<TAB>phrase", ou texte brut une phrase/ligne).
#
# Pourquoi KN et pas des comptes bruts + stupid-backoff : le stupid-backoff
# surestime les n-grammes rares (un trigramme vu 2 fois sur un contexte vu 2
# fois donne P=1.0) et son poids de repli est une constante arbitraire. KN
# soustrait une décote D estimée sur le corpus (Good-Turing : D=n1/(n1+2·n2)),
# redistribue exactement cette masse vers l'ordre inférieur via γ(ctx), et
# l'ordre inférieur utilise des COMPTES DE CONTINUATION (dans combien de
# contextes distincts ce mot apparaît) — « york » est fréquent mais presque
# toujours après « new », donc sa probabilité de continuation est faible.
#
# Tout le calcul est OFFLINE : le daemon ne fait que des lookups
#   P(w|u,v) = p3(uvw)            si stocké
#            = γ3(uv)·P(w|v)      sinon (γ3=1 si contexte inconnu)
#   P(w|v)   = p2(vw)             si stocké
#            = γ2(v)·Pcont(w)     sinon (γ2=1 si contexte inconnu)
# Les p stockés sont les probabilités INTERPOLÉES finales (terme γ·inférieur
# inclus) — format ARPA, adapté en TSV.
#
# Sorties (dans <outdir>) :
#   bigrams.tsv   v<TAB>w<TAB>p           trigrams.tsv   u<TAB>v<TAB>w<TAB>p
#   bigrams.bo.tsv  v<TAB>γ               trigrams.bo.tsv  u<TAB>v<TAB>γ
#   pcont.tsv     w<TAB>Pcont(w)
#
# Mots hors vocabulaire (words.tsv) : cassent le n-gramme (pas de comptage à
# travers). Comptage par identifiants entiers compactés (3×21 bits) pour tenir
# en RAM sur de gros corpus.
#
# usage: build_ngrams.py <words.tsv> <outdir> <sentences.txt>...
import sys, re, collections

MIN_BI = 2   # un bigramme stocké doit apparaître >= 2 fois
MIN_TRI = 2  # un trigramme stocké >= 2 fois
TOK = re.compile(r"[^\W\d_]+(?:['-][^\W\d_]+)*", re.UNICODE)

vocabpath, outdir = sys.argv[1], sys.argv[2]
sentence_files = sys.argv[3:]

words = []
wid = {}
with open(vocabpath, encoding="utf-8") as f:
    for line in f:
        p = line.split()
        if p and p[0] not in wid:
            wid[p[0]] = len(words)
            words.append(p[0])
assert len(words) < (1 << 21), "vocab > 2M : élargir le packing"

# ----------------------------------------------------------- comptage brut ---
SH = 21
MASK = (1 << SH) - 1
bi = collections.Counter()   # (v<<21|w) -> count
tri = collections.Counter()  # (u<<42|v<<21|w) -> count
ntok = 0
for path in sentence_files:
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            text = line[tab + 1:] if tab >= 0 else line
            text = text.replace("’", "'").lower()
            ids = [wid.get(t) for t in TOK.findall(text)]
            ntok += len(ids)
            for i, w in enumerate(ids):
                if w is None:
                    continue
                if i >= 1 and ids[i - 1] is not None:
                    v = ids[i - 1]
                    bi[(v << SH) | w] += 1
                    if i >= 2 and ids[i - 2] is not None:
                        tri[(ids[i - 2] << (2 * SH)) | (v << SH) | w] += 1
print(f"{ntok} tokens, {len(bi)} bigrammes bruts, {len(tri)} trigrammes bruts",
      file=sys.stderr)

# ------------------------------------------ comptes de continuation (KN) -----
# c2'(v,w) = N1+(·vw) : nb de u distincts devant (v,w). C'est le compte qu'on
# utilise au niveau bigramme (ordre inférieur du modèle trigramme).
cont2 = collections.Counter()
for key in tri:
    cont2[key & ((1 << (2 * SH)) - 1)] += 1
# Les bigrammes en début de phrase n'ont pas de u : un (v,w) jamais vu en
# 3e position garde son compte brut (sinon il disparaîtrait du modèle).
for key, c in bi.items():
    if key not in cont2:
        cont2[key] = c

# N1+(·w) (continuation unigramme) et N1+(v·) / totaux par contexte.
n1w = collections.Counter()       # w -> nb de v distincts
ctx2_tot = collections.Counter()  # v -> Σ_w c2'(vw)
ctx2_nty = collections.Counter()  # v -> nb de types de suite
for key, c in cont2.items():
    v, w = key >> SH, key & MASK
    n1w[w] += 1
    ctx2_tot[v] += c
    ctx2_nty[v] += 1
n_bigram_types = len(cont2)

ctx3_tot = collections.Counter()  # (u,v) -> Σ_w c3
ctx3_nty = collections.Counter()  # (u,v) -> nb de types de suite
for key, c in tri.items():
    uv = key >> SH
    ctx3_tot[uv] += c
    ctx3_nty[uv] += 1

# ----------------------------------------------- décotes (Good-Turing-ish) ---
def discount(counter):
    n1 = sum(1 for c in counter.values() if c == 1)
    n2 = sum(1 for c in counter.values() if c == 2)
    return n1 / (n1 + 2.0 * n2) if (n1 + n2) else 0.5

D2 = discount(cont2)
D3 = discount(tri)
print(f"décotes KN : D2={D2:.3f} D3={D3:.3f}", file=sys.stderr)

# ------------------------------------------------------------ probabilités ---
# Pcont(w) = N1+(·w) / N1+(··)
pcont = {w: n / n_bigram_types for w, n in n1w.items()}

# Bigramme interpolé : P(w|v) = max(c2'-D2,0)/tot(v) + γ2(v)·Pcont(w)
gamma2 = {v: D2 * ctx2_nty[v] / ctx2_tot[v] for v in ctx2_tot}
p2 = {}
for key, c in cont2.items():
    v, w = key >> SH, key & MASK
    p2[key] = max(c - D2, 0.0) / ctx2_tot[v] + gamma2[v] * pcont.get(w, 0.0)

# Trigramme interpolé : P(w|uv) = max(c3-D3,0)/tot(uv) + γ3(uv)·P(w|v)
gamma3 = {uv: D3 * ctx3_nty[uv] / ctx3_tot[uv] for uv in ctx3_tot}

def p_bi(v, w):
    p = p2.get((v << SH) | w)
    if p is not None:
        return p
    g = gamma2.get(v)
    return (g if g is not None else 1.0) * pcont.get(w, 0.0)

# ----------------------------------------------------------------- sorties ---
# On stocke les entrées au-dessus du seuil (comptes BRUTS — la pertinence d'un
# n-gramme se juge sur son occurrence réelle). γ2 : tous les contextes (petit,
# et il pèse dans le scoring trigramme même quand les bigrammes du contexte
# sont élagués). γ3 : SEULEMENT les contextes ayant une entrée stockée — le
# daemon ne lit γ3 que s'il a trouvé des suiveurs trigramme ; le stocker pour
# les autres serait du poids mort (~3× la taille du modèle).
nb = nt = 0
kept3 = set()
with open(outdir + "/bigrams.tsv", "w", encoding="utf-8") as f:
    for key, c in bi.items():
        if c >= MIN_BI:
            v, w = key >> SH, key & MASK
            f.write(f"{words[v]}\t{words[w]}\t{p2[key]:.6g}\n")
            nb += 1
with open(outdir + "/bigrams.bo.tsv", "w", encoding="utf-8") as f:
    for v, g in gamma2.items():
        f.write(f"{words[v]}\t{g:.6g}\n")
with open(outdir + "/trigrams.tsv", "w", encoding="utf-8") as f:
    for key, c in tri.items():
        if c >= MIN_TRI:
            u, v, w = key >> (2 * SH), (key >> SH) & MASK, key & MASK
            p = max(c - D3, 0.0) / ctx3_tot[key >> SH] \
                + gamma3[key >> SH] * p_bi(v, w)
            f.write(f"{words[u]}\t{words[v]}\t{words[w]}\t{p:.6g}\n")
            kept3.add(key >> SH)
            nt += 1
with open(outdir + "/trigrams.bo.tsv", "w", encoding="utf-8") as f:
    for uv in kept3:
        f.write(f"{words[uv >> SH]}\t{words[uv & MASK]}\t{gamma3[uv]:.6g}\n")
with open(outdir + "/pcont.tsv", "w", encoding="utf-8") as f:
    for w, p in pcont.items():
        f.write(f"{words[w]}\t{p:.6g}\n")
print(f"stocké : {nb} bigrammes (>= {MIN_BI}), {nt} trigrammes (>= {MIN_TRI}), "
      f"{len(pcont)} pcont", file=sys.stderr)
