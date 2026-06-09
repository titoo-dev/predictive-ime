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
}
BIGRAMS = [("je", "veux", 800), ("je", "vais", 900), ("je", "vous", 300),
           ("je", "va", 50), ("ne", "pas", 5000), ("il", "faut", 400)]
TRIGRAMS = [("je", "ne", "sais", 200), ("je", "ne", "veux", 50),
            ("ne", "sais", "pas", 300)]

with open(f"{tmp}/words.tsv", "w", encoding="utf-8") as f:
    for w, c in WORDS.items():
        f.write(f"{w} {c}\n")
with open(f"{tmp}/bigrams.tsv", "w", encoding="utf-8") as f:
    for a, b, c in BIGRAMS:
        f.write(f"{a}\t{b}\t{c}\n")
with open(f"{tmp}/trigrams.tsv", "w", encoding="utf-8") as f:
    for a, b, d, c in TRIGRAMS:
        f.write(f"{a}\t{b}\t{d}\t{c}\n")

env = dict(os.environ, XDG_DATA_HOME=f"{tmp}/xdg")
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

    # 8) ROBUSTESSE SIGPIPE : l'engine envoie "learn" en fire-and-forget (écrit
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
