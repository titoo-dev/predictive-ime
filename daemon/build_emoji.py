#!/usr/bin/env python3
# Construit emoji.tsv (clé<TAB>emoji<TAB>poids) depuis les annotations CLDR
# (cldr-json, annotations-full) — les mots-clés officiels Unicode, FR et EN
# fusionnés. La clé est REPLIÉE (minuscule sans accent, comme foldStr du
# daemon) : ":coeur" matche "cœur", ":etoile" matche "étoile".
#
# Poids : tts (nom canonique) > mot-clé entier > mot extrait d'une phrase.
# Le daemon préfère ensuite les clés courtes et les emojis déjà utilisés.
#
# Le 1er argument est emoji-test.txt (Unicode officiel) : il sert de LISTE
# AUTORITAIRE — les annotations CLDR couvrent aussi ponctuation/math/devises
# (✗ picker) et leurs clés sont NON QUALIFIÉES (sans U+FE0F → rendu texte
# monochrome possible). On ne garde que les vrais emoji et on émet leur forme
# fully-qualified.
#
# usage: build_emoji.py <emoji-test.txt> <out.tsv> <annotations.json>...
import json, sys, unicodedata

STOP = {"de", "du", "des", "la", "le", "les", "un", "une", "et", "avec",
        "sur", "en", "d", "l", "a", "of", "the", "and", "with", "on", "in",
        "or", "ou", "au", "aux", "pour", "for", "to"}

# Prior de POPULARITÉ (étude de fréquence Unicode, ordre décroissant) : à
# mots-clés équivalents, l'emoji que les gens utilisent vraiment sort en tête
# (":coeur" → ❤️ avant 🫀 dont le nom tts est exactement « cœur »).
POPULAR = [
    "😂", "❤️", "🤣", "👍", "😭", "🙏", "😘", "🥰", "😍", "😊", "🎉", "😁",
    "💕", "🥺", "😅", "🔥", "☺️", "🤦", "♥️", "🤷", "🙄", "😆", "🤗", "😉",
    "🎂", "🤔", "👏", "🙂", "😳", "🥳", "😎", "👌", "💜", "😔", "💪", "✨",
    "💖", "👀", "😋", "😏", "😢", "👉", "💗", "😩", "💯", "🌹", "💞", "🎈",
    "💙", "😃", "😡", "💐", "😜", "🙈", "🤞", "😄", "🤤", "🙌", "🤪", "❣️",
    "😀", "💋", "💀", "👇", "💔", "😌", "💓", "🤩", "🙃", "😬", "😱", "😴",
    "🤭", "😐", "🌞", "😒", "😇", "🌸", "😈", "🎶", "✌️", "🎊", "🥵", "😞",
    "💚", "☀️", "🖤", "💰", "😚", "👑", "🎁", "💥", "🙋", "☹️", "😑", "🥴",
    "👈", "💩", "✅", "👋", "🤮", "😤", "🤢", "🌟", "❗", "😥", "🌈", "💛",
    "😝", "😫", "😲", "🖕", "‼️", "🔴", "🌻", "🤯", "💃", "👊", "🤬", "🏃",
    "😕", "👁", "⚡", "☕", "🍀", "💦", "⭐", "🦋", "🤨", "🌺", "😹", "🤘",
    "🌷", "💝", "💤", "🤝", "🐰", "😓", "💘", "🍻", "😟", "😣", "🧐", "😠",
    "🤠", "😻", "🌙", "😛", "🤙", "🙊", "🧡", "🤡", "🤫", "🌼", "🥂", "😷",
    "🤓", "🥶", "😶", "😖", "🎵", "🚶", "😙", "🍆", "🤑", "💅", "😗", "🐶",
    "🍓", "👅", "👄", "🌿", "🚨", "📣", "🤟", "🍑", "🍃", "😮", "💎", "📢",
    "🌱", "🙁", "🍷", "😪", "🌚", "🏆", "🍒", "💉", "💢", "🛒", "😸", "🐾",
]
# (clés indexées par squelette : les formes de POPULAR matchent qu'elles
# portent ou non le FE0F)
POP_BONUS = {}


def fold(s):
    s = s.lower().replace("’", "'").replace("œ", "oe").replace("æ", "ae")
    nfd = unicodedata.normalize("NFD", s)
    return "".join(c for c in nfd if unicodedata.category(c) != "Mn")


emoji_test, out, files = sys.argv[1], sys.argv[2], sys.argv[3:]

# squelette (sans FE0F) -> forme fully-qualified. Toute clé CLDR absente de
# cette table n'est PAS un emoji → ignorée.
def skel(s):
    return tuple(cp for cp in map(ord, s) if cp != 0xFE0F)

fq = {}
with open(emoji_test, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        codes, _, status_part = line.partition(";")
        if not status_part.strip().startswith("fully-qualified"):
            continue
        seq = "".join(chr(int(h, 16)) for h in codes.split())
        fq.setdefault(skel(seq), seq)
print(f"{len(fq)} emojis fully-qualified (emoji-test.txt)", file=sys.stderr)

best = {}  # (clé, emoji FQ) -> poids


def offer(key, emoji, w):
    key = key.strip()
    if len(key) < 2 or key in STOP:
        return
    k = (key, emoji)
    if best.get(k, 0.0) < w:
        best[k] = w


# Poids : le NOM canonique (tts) domine les mots-clés — ":soleil" doit donner
# ☀️ (tts « soleil ») avant 😎 (mot-clé « soleil »).
for path in files:
    with open(path, encoding="utf-8") as f:
        ann = json.load(f)["annotations"]["annotations"]
    for key, entry in ann.items():
        emoji = fq.get(skel(key))
        if emoji is None:
            continue  # pas un emoji (ponctuation, math, devise…)
        for name in entry.get("tts", []):
            fn = fold(name)
            offer(fn, emoji, 5.0)
            for wd in fn.replace("-", " ").split():
                offer(wd, emoji, 2.5)
        for kw in entry.get("default", []):
            fk = fold(kw)
            offer(fk, emoji, 2.0)
            for wd in fk.replace("-", " ").split():
                offer(wd, emoji, 1.0)

for i, e in enumerate(POPULAR):
    POP_BONUS[skel(e)] = 4.0 * (1.0 - i / len(POPULAR))

with open(out, "w", encoding="utf-8") as f:
    for (key, emoji), w in sorted(best.items()):
        f.write(f"{key}\t{emoji}\t{w + POP_BONUS.get(skel(emoji), 0.0):g}\n")
print(f"emoji.tsv: {len(best)} (clé, emoji), "
      f"{len({e for _, e in best})} emojis", file=sys.stderr)
