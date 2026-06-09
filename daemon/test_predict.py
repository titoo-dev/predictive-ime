#!/usr/bin/env python3
# Test comportemental du daemon de prédiction (cerveau v2), sans session
# graphique : monte un petit modèle synthétique, lance predictord sur un socket
# temporaire, envoie des requêtes JSON et vérifie repli accent, autocorrection,
# ré-ordonnancement par contexte, mot-suivant trigramme et apprentissage.
#
# usage: test_predict.py <chemin/predictord>
import json, os, socket, subprocess, sys, tempfile, time

BIN = sys.argv[1]
tmp = tempfile.mkdtemp(prefix="ime-test-")
sock = os.path.join(tmp, "sock")

WORDS = {
    "être": 9000, "français": 8000, "france": 7000, "bonjour": 6000,
    "comment": 12000, "comme": 20000, "content": 5000, "code": 4000,
    "veux": 3000, "vais": 3500, "vous": 50000, "va": 9000,
    "le": 99000, "les": 80000, "the": 100000, "quand": 15000,
    "interesting": 2000, "internet": 2500, "développement": 1500,
    "travail": 6000, "travailler": 4000, "aujourd'hui": 7000,
    "pc": 3000, "soleil": 3000, "solar": 3500,
}
# 3e colonne de words.tsv : langue (boost selon la langue du contexte)
LANGS = {"bonjour": "fr", "soleil": "fr", "solar": "en", "the": "en"}
# Format modèle KN précalculé (cf build_ngrams.py) : probabilités interpolées
# + poids de backoff γ par contexte + continuation unigramme.
BIGRAMS = [("je", "veux", 0.30), ("je", "vais", 0.35), ("je", "vous", 0.12),
           ("je", "va", 0.02), ("ne", "pas", 0.60), ("il", "faut", 0.40),
           ("<s>", "the", 0.50), ("<s>", "bonjour", 0.20)]
BIGRAMS_BO = [("je", 0.2), ("ne", 0.2), ("il", 0.3), ("<s>", 0.2)]
TRIGRAMS = [("je", "ne", "sais", 0.50), ("je", "ne", "veux", 0.12),
            ("ne", "sais", "pas", 0.70)]
TRIGRAMS_BO = [("je", "ne", 0.3), ("ne", "sais", 0.2)]

with open(f"{tmp}/words.tsv", "w", encoding="utf-8") as f:
    for w, c in WORDS.items():
        lang = f" {LANGS[w]}" if w in LANGS else ""
        f.write(f"{w} {c}{lang}\n")
with open(f"{tmp}/bigrams.tsv", "w", encoding="utf-8") as f:
    for a, b, p in BIGRAMS:
        f.write(f"{a}\t{b}\t{p}\n")
with open(f"{tmp}/bigrams.bo.tsv", "w", encoding="utf-8") as f:
    for a, g in BIGRAMS_BO:
        f.write(f"{a}\t{g}\n")
with open(f"{tmp}/trigrams.tsv", "w", encoding="utf-8") as f:
    for a, b, d, p in TRIGRAMS:
        f.write(f"{a}\t{b}\t{d}\t{p}\n")
with open(f"{tmp}/trigrams.bo.tsv", "w", encoding="utf-8") as f:
    for a, b, g in TRIGRAMS_BO:
        f.write(f"{a}\t{b}\t{g}\n")
with open(f"{tmp}/pcont.tsv", "w", encoding="utf-8") as f:
    tot = sum(WORDS.values())
    for w, c in WORDS.items():
        f.write(f"{w}\t{c / tot:.6g}\n")

# Index emoji (clé repliée -> emoji, poids), cf build_emoji.py.
EMOJI = [("coeur", "❤️", 3), ("heart", "❤️", 3), ("etoile", "⭐", 3),
         ("star", "⭐", 3), ("sourire", "😊", 3), ("souris", "🐭", 3),
         ("feu", "🔥", 3), ("fire", "🔥", 3)]
with open(f"{tmp}/emoji.tsv", "w", encoding="utf-8") as f:
    for k, e, w in EMOJI:
        f.write(f"{k}\t{e}\t{w}\n")

# config/dict/snippets perso ($XDG_CONFIG_HOME/ime-predictord, rechargé à chaud)
cfgdir = f"{tmp}/cfg/ime-predictord"
os.makedirs(cfgdir)
with open(f"{cfgdir}/snippets.tsv", "w", encoding="utf-8") as f:
    f.write("# commentaire\n;mail\tdev@example.test\n;shrug\t¯\\_(ツ)_/¯\n")
with open(f"{cfgdir}/dict.txt", "w", encoding="utf-8") as f:
    f.write("# dico perso\nzzcustom 7000\n")

env = dict(os.environ, XDG_DATA_HOME=f"{tmp}/xdg",
           XDG_CONFIG_HOME=f"{tmp}/cfg")
proc = subprocess.Popen([BIN, f"{tmp}/words.tsv", sock], env=env)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.05)
time.sleep(0.2)


def req(obj):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    s.sendall((json.dumps(obj) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf)


def cands(prefix="", context=None):
    return req({"prefix": prefix, "context": context or []})["candidates"]


def auto(prefix, context=None):
    return req({"prefix": prefix, "context": context or []}).get("autocomplete", "")


fails = []


def check(name, cond, detail=""):
    print(("  ok  " if cond else " FAIL ") + name + (f"  [{detail}]" if detail else ""))
    if not cond:
        fails.append(name)


try:
    # 1) repli accent-insensible
    c = cands("etre")
    check("fold: etre → être", "être" in c, str(c))
    c = cands("francais")
    check("fold: francais → français", "français" in c, str(c))
    c = cands("developpement")
    check("fold: developpement → développement", "développement" in c, str(c))

    # 2) autocorrection floue
    c = cands("bonjuor")
    check("typo transposition: bonjuor → bonjour", "bonjour" in c, str(c))
    c = cands("qaund")
    check("typo transposition: qaund → quand", "quand" in c, str(c))
    c = cands("bonjpur")
    check("typo adjacence AZERTY: bonjpur → bonjour", "bonjour" in c, str(c))

    # 3) complétion re-classée par le contexte
    base = cands("v")
    check("sans contexte: 'va' avant 'vais'",
          base.index("va") < base.index("vais"), str(base))
    ctx = cands("v", ["je"])
    check("contexte 'je': 'vais' avant 'va'",
          ctx.index("vais") < ctx.index("va"), str(ctx))

    # 4) literalIsWord (n'écrase jamais un vrai mot)
    r = req({"prefix": "le", "context": []})
    check("literalIsWord: 'le' est un mot", r["literalIsWord"] is True)
    r = req({"prefix": "bonjou", "context": []})
    check("literalIsWord: 'bonjou' n'est pas un mot", r["literalIsWord"] is False)

    # 5) mot-suivant : trigramme (proba) > bigramme fréquent
    c = cands("", ["je", "ne"])
    check("trigramme: 'je ne' → 'sais' en tête", c and c[0] == "sais", str(c))
    c = cands("", ["ne"])
    check("bigramme: 'ne' → 'pas' en tête", c and c[0] == "pas", str(c))

    # 6) apprentissage utilisateur (priorité)
    req({"learn": {"prev": "je", "word": "code"}})
    req({"learn": {"prev": "je", "word": "code"}})
    c = cands("", ["je"])
    check("appris: 'je' → 'code' en tête", c and c[0] == "code", str(c))
    c = cands("co", ["je"])
    check("appris: 'co' → 'code' en tête", c and c[0] == "code", str(c))

    # 7) AUTO-COMPLÉTION SUR ESPACE (haute confiance) : ne mutile pas les
    #    contractions françaises (j'ai ne doit jamais devenir jail).
    check("autocomplete: 'bonjou' → 'bonjour' (préfixe)", auto("bonjou") == "bonjour",
          auto("bonjou"))
    check("autocomplete: 'teh' → 'the' (faute simple, pas d'apostrophe)",
          auto("teh") == "the", auto("teh"))
    check("autocomplete: \"aujourd'\" → \"aujourd'hui\" (préfixe avec apostrophe)",
          auto("aujourd'") == "aujourd'hui", auto("aujourd'"))
    check("autocomplete: apostrophe SANS complétion → '' (garde le littéral)",
          auto("z'x") == "", repr(auto("z'x")))

    # 7bis) GARDE-FOUS d'auto-application (les candidats restent affichés,
    #       seul le remplacement automatique est bridé).
    check("garde-fou: préfixe < 3 lettres → pas d'auto ('v')", auto("v") == "",
          repr(auto("v")))
    check("garde-fou: top non dominant → pas d'auto ('comm': comme vs comment)",
          auto("comm") == "", repr(auto("comm")))
    c = cands("comm")
    check("garde-fou: les candidats restent listés ('comm')",
          "comme" in c and "comment" in c, str(c))
    check("garde-fou: dominant → auto OK ('conten' → content)",
          auto("conten") == "content", repr(auto("conten")))
    c = cands("pcq")
    check("garde-fou: une faute floue ne RACCOURCIT pas ('pcq' ↛ 'pc')",
          auto("pcq") == "" and "pc" in c, f"auto={auto('pcq')!r} cands={c}")
    c = cands("aujourd’")
    check("apostrophe typographique: \"aujourd’\" → aujourd'hui",
          "aujourd'hui" in c, str(c))

    # 7ter) SEUIL D'APPRENTISSAGE : un fragment committé UNE fois ne passe pas
    #       devant le modèle ; deux fois → confiance.
    req({"learn": {"prev": "", "word": "bonjo"}})
    c = cands("bonj")
    check("seuil: 1 commit de 'bonjo' → 'bonjour' reste premier",
          c and c[0] == "bonjour", str(c))
    req({"learn": {"prev": "", "word": "bonjo"}})
    c = cands("bonj")
    check("seuil: 2 commits de 'bonjo' → il passe devant",
          c and c[0] == "bonjo", str(c))

    # 7quater) FORGET : oublier un mot appris (et réécrire le journal).
    r = req({"forget": {"word": "bonjo"}})
    check("forget: 'bonjo' retiré", r.get("ok") and r.get("removed", 0) >= 1,
          str(r))
    c = cands("bonj")
    check("forget: 'bonjour' redevient premier", c and c[0] == "bonjour", str(c))

    # 7quinquies) AMORCE DE PHRASE : contexte vide → bigrammes <s>.
    c = cands("", [])
    check("amorce <s>: contexte vide → 'the' en tête", c and c[0] == "the",
          str(c))

    # 7sexies) SNIPPETS (préfixe ';') — déclencheur exact auto-appliqué.
    c = cands(";mail")
    check("snippet: ';mail' → expansion en tête", c and c[0] == "dev@example.test",
          str(c))
    check("snippet: autocomplete ';mail' (Espace applique)",
          auto(";mail") == "dev@example.test", repr(auto(";mail")))
    c = cands(";ma")
    check("snippet: préfixe ';ma' → expansion AFFICHÉE, pas auto",
          "dev@example.test" in c and auto(";ma") == "", str(c))

    # 7septies) DICTIONNAIRE PERSO : vocabulaire déclaratif, jamais corrigé.
    r = req({"prefix": "zzcustom", "context": []})
    check("dict perso: 'zzcustom' est un mot (literalIsWord)",
          r["literalIsWord"] is True, str(r))
    c = cands("zzcus")
    check("dict perso: complétable", "zzcustom" in c, str(c))

    # 7octies) VETO persistant : un revert interdit la paire pour toujours.
    check("veto: avant — 'vias' s'auto-corrige en 'vais'",
          auto("vias") == "vais", repr(auto("vias")))
    req({"veto": {"typed": "vias", "applied": "vais"}})
    c = cands("vias")
    check("veto: après — plus d'auto, mais 'vais' reste candidat",
          auto("vias") == "" and "vais" in c, f"auto={auto('vias')!r} c={c}")

    # 7nonies) LANGUE : le contexte vote, les mots de sa langue remontent.
    c = cands("sol")
    check("langue: sans contexte → 'solar' (plus fréquent) devant",
          c and c[0] == "solar", str(c))
    c = cands("sol", ["bonjour"])
    check("langue: contexte FR → 'soleil' devant", c and c[0] == "soleil",
          str(c))

    # 7decies) MULTI-MOTS : continuation très sûre → expression en FIN de
    #          barre (sans déplacer le top des mots simples).
    c = cands("", ["je", "ne"])
    check("multi-mots: 'je ne' → 'sais pas' proposé (fin de barre)",
          "sais pas" in c and c[0] == "sais", str(c))

    # 7undecies) STATS : la boîte noire est inspectable.
    r = req({"stats": True})
    check("stats: vocab/userWords/snippets exposés",
          r.get("ok") and r.get("vocab", 0) > 20 and r.get("snippets") == 2,
          str({k: r.get(k) for k in ("ok", "vocab", "snippets")}))

    # 8) EMOJI PICKER (préfixe ':') — mots-clés CLDR repliés, favoris appris.
    c = cands(":coeur")
    check("emoji: ':coeur' → ❤️ en tête", c and c[0] == "❤️", str(c))
    check("emoji: autocomplete ':coeur' → ❤️ (Espace committe l'emoji)",
          auto(":coeur") == "❤️", auto(":coeur"))
    c = cands(":cœur")
    check("emoji: ':cœur' (ligature) → ❤️", "❤️" in c, str(c))
    c = cands(":sour")
    check("emoji: préfixe ':sour' → sourire ET souris", "😊" in c and "🐭" in c,
          str(c))
    check("emoji: ':' seul → pas d'autocomplete (Espace garde le littéral)",
          auto(":") == "", repr(auto(":")))
    req({"learn": {"prev": "", "word": "⭐"}})
    req({"learn": {"prev": "", "word": "⭐"}})
    c = cands(":")
    check("emoji: favoris — ':' liste ⭐ après usage", c and c[0] == "⭐", str(c))
    check("emoji: grille — ':' remplit avec les populaires (tout le set)",
          len(c) == 5, str(c))
    c = cands(":s")
    check("emoji: favori remonte — ':s' → ⭐ (star) avant 😊", c and c[0] == "⭐",
          str(c))
    c = cands("coeur")
    check("emoji: hint — 'coeur' (mot normal) propose ❤️ en fin de barre",
          "❤️" in c, str(c))

    # 9) ROBUSTESSE SIGPIPE : l'engine envoie "learn" en fire-and-forget (écrit
    #    puis ferme SANS lire la réponse). Sans SIG_IGN, le write du daemon sur
    #    le socket fermé lèverait SIGPIPE et TUERAIT le daemon — les prédictions
    #    mourraient après quelques mots. On simule 200 commits et on vérifie que
    #    le daemon répond toujours.
    def fire_and_forget(obj):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()  # ferme sans lire → write côté daemon sur socket fermé
    for i in range(200):
        fire_and_forget({"learn": {"prev": "mot", "word": f"w{i}"}})
    alive = proc.poll() is None
    still = cands("co", ["je"]) if alive else []
    check("SIGPIPE: daemon survit à 200 learn fire-and-forget",
          alive and bool(still), "daemon mort" if not alive else str(still))
finally:
    proc.terminate()
    proc.wait()

print()
if fails:
    print(f"{len(fails)} test(s) en échec: {fails}")
    sys.exit(1)
print("tous les tests passent ✓")
