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
    # élisions : proclitique nu (j') vs formes pleines (j'ai/j'aime) — pour B.
    "j'": 12000, "j'ai": 50000, "j'aime": 8000,
    # restitution d'accent : homographe DOMINANT (être ≫ etre → restitue) et
    # homographe NON dominant (mûr < accentDom×mur → garde le littéral).
    "etre": 50, "mur": 1000, "mûr": 2000,
    # apostrophe oubliée (canal élision) : jai→j'ai, dici→d'ici, cest→c'est,
    # temener→t'emmener (élision + lettre oubliée, canaux composés). « jai »
    # est AUSSI un mot-poubelle du corpus (comme dans le vrai modèle) : la
    # restauration doit passer outre literalIsWord (accentOnly).
    "d'ici": 6000, "c'est": 60000, "t'emmener": 800, "jai": 300,
    # restauration d'accents : graphie brute AUSSI au corpus (dominance
    # accentDom), homographe légitime (cote/côté), ligature (coeur/cœur).
    "francais": 1500, "cote": 5000, "côté": 6000, "coeur": 9000, "cœur": 4000,
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
         ("feu", "🔥", 3), ("fire", "🔥", 3),
         # 7 emojis DISTINCTS au total → la grille ':' en renvoie >5. Sert à
         # prouver que la grille emoji n'est PAS soumise au plafond de 5 mots
         # de la barre de suggestion (cf. section « barre » plus bas).
         ("eau", "💧", 3), ("water", "💧", 3), ("chat", "🐱", 3), ("cat", "🐱", 3)]
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
           XDG_CONFIG_HOME=f"{tmp}/cfg",
           GROQ_API_KEY="clef-de-test")  # cf section reformulation (mock HTTP)
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

    # 2bis) lettre OUBLIÉE + espace oublié (E8)
    c = cands("bonjor")
    check("typo omission: bonjor → bonjour", "bonjour" in c, str(c))
    c = cands("travil")
    check("typo omission: travil → travail", "travail" in c, str(c))
    c = cands("nepas")
    check("espace oublié: nepas → « ne pas »", "ne pas" in c, str(c))
    a = auto("nepas")
    check("espace oublié: jamais auto-appliqué", " " not in a, repr(a))

    # 2ter) APOSTROPHE OUBLIÉE (canal élision, repli sans apostrophe)
    c = cands("jai")
    check("élision: jai → j'ai", "j'ai" in c, str(c))
    r = req({"prefix": "jai", "context": []})
    check("élision: jai → j'ai auto-appliqué (dominant)",
          r.get("autocomplete") == "j'ai", str(r))
    check("élision: restauration pure → accentOnly (malgré « jai » au vocab)",
          r.get("accentOnly") is True and r.get("literalIsWord") is True,
          str(r))
    c = cands("dici")
    check("élision: dici → d'ici", "d'ici" in c, str(c))
    c = cands("cest")
    check("élision: cest → c'est", "c'est" in c, str(c))
    c = cands("temener")
    check("élision composée: temener → t'emmener (apostrophe + lettre)",
          "t'emmener" in c, str(c))

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

    # 5bis) LONGUEUR DE LA BARRE : la barre de mots n'affiche JAMAIS plus de 5
    #       suggestions (UX : seul le top-5 le plus pertinent ; le modèle, trié
    #       par score, garantit que ce sont bien les 5 plus pertinents). Vaut
    #       pour le mot-suivant, l'amorce de phrase et la complétion intra-mot.
    #       La grille emoji (préfixe ':') N'EST PAS concernée (testée plus bas).
    c = cands("", ["ne"])  # bigramme + remplissage topUni → barre pleine
    check("barre: mot-suivant plafonné à 5", len(c) <= 5, str(c))
    c = cands("", [])      # amorce <s> + remplissage → barre pleine
    check("barre: amorce de phrase plafonnée à 5", len(c) <= 5, str(c))
    c = cands("", ["je", "ne"])  # trigramme + multi-mots → barre pleine
    check("barre: contexte riche (+ multi-mots) plafonné à 5", len(c) <= 5, str(c))

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

    # 7nonies-bis) LANGUE CHOISIE (préférences) : cfg "lang" court-circuite la
    #              détection — déterministe quel que soit le contexte.
    _bump = [0]

    def set_config(obj):
        # le rechargement à chaud compare des mtimes en SECONDES : on force un
        # pas distinct à chaque écriture (sinon 2 écritures < 1 s se ratent).
        _bump[0] += 2
        p = f"{cfgdir}/config.json"
        with open(p, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        t = time.time() + _bump[0]
        os.utime(p, (t, t))

    set_config({"lang": "fr"})
    c = cands("sol")
    check("langue choisie fr: 'soleil' devant même sans contexte",
          c and c[0] == "soleil", str(c))
    c = cands("sol", ["the"])
    check("langue choisie fr: un contexte EN ne change rien",
          c and c[0] == "soleil", str(c))
    set_config({"lang": "en"})
    c = cands("sol", ["bonjour"])
    check("langue choisie en: 'solar' devant malgré contexte FR",
          c and c[0] == "solar", str(c))
    set_config({"lang": "off"})
    c = cands("sol", ["bonjour"])
    check("langue off: fréquence brute, 'solar' devant",
          c and c[0] == "solar", str(c))
    set_config({})
    c = cands("sol", ["bonjour"])
    check("langue auto (défaut): le contexte FR revote 'soleil'",
          c and c[0] == "soleil", str(c))
    r = req({"stats": True})
    check("stats: langue active exposée", r.get("lang") == "auto",
          str(r.get("lang")))

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

    # 7duodecies) MOTS APPRIS À L'ÉCHELLE DU MODÈLE (amélioration A) : le boost
    #   appris est multiplicatif (fréquence effective plancher × confiance), plus
    #   de plancher écrasant 1e18. Un mot appris rare passe devant les mots
    #   ordinaires mais PAS devant un mot massivement plus fréquent.
    #   (learnedFloor par défaut vise le VRAI modèle ~10^5 ; on l'abaisse ici à
    #   l'échelle du modèle synthétique de test pour des seuils déterministes.)
    set_config({"learnedFloor": 40000})
    req({"learn": {"prev": "", "word": "verror"}})  # OOV, préfixe 'v'
    req({"learn": {"prev": "", "word": "verror"}})  # 2 commits → de confiance
    c = cands("v")
    check("A: appris 'verror' présent mais SOUS 'vous' (50000 ≫ plancher)",
          "verror" in c and "vous" in c and c.index("vous") < c.index("verror"),
          str(c))
    check("A: appris 'verror' AU-DESSUS d'un mot ordinaire ('va' 9000)",
          "va" in c and c.index("verror") < c.index("va"), str(c))
    for _ in range(8):  # très appris (count=10) → la confiance le fait monter
        req({"learn": {"prev": "", "word": "verror"}})
    c = cands("v")
    check("A: 'verror' fortement appris finit par dépasser 'vous'",
          c.index("verror") < c.index("vous"), str(c))
    req({"forget": {"word": "verror"}})
    # predictNext : un bigramme appris FAIBLE ne court-circuite plus un suiveur
    # modèle très probable (ne→pas .60), mais reste prioritaire sur les moyens.
    req({"learn": {"prev": "ne", "word": "code"}})
    req({"learn": {"prev": "ne", "word": "code"}})
    c = cands("", ["ne"])
    check("A: bigramme appris faible (ne→code) < suiveur fort (ne→pas .60)",
          c and c[0] == "pas" and "code" in c, str(c))
    req({"forget": {"word": "code"}})  # nettoie ne→code ET je→code (test 6)
    set_config({})  # restaure learnedFloor par défaut

    # 7terdecies) PROCLITIQUE D'ÉLISION NU rétrogradé (amélioration B) : taper
    #   « j' » doit proposer j'ai / j'aime AVANT le proclitique nu « j' ».
    c = cands("j'")
    check("B: 'j'' nu rétrogradé sous j'ai ET j'aime",
          "j'" in c and "j'ai" in c and "j'aime" in c and
          c.index("j'") > c.index("j'ai") and c.index("j'") > c.index("j'aime"),
          str(c))

    # 7terdecies-bis) RESTITUTION D'ACCENT SUR ESPACE, INDÉPENDANTE de autoApply.
    #   La config promet : `accentRestore` restaure les accents/ligatures même
    #   quand autoApply=false — on n'AJOUTE que des accents (mêmes lettres), on
    #   ne change/complète/corrige jamais le mot. Homographe (la graphie nue est
    #   AUSSI au corpus) : restitue seulement si la forme accentuée DOMINE par
    #   `accentDom`. Régression : avec autoApply=false l'autocomplete restait vide.
    set_config({"autoApply": False, "accentRestore": True})
    check("accent (autoApply off): hors-corpus restitué — 'developpement'→'développement'",
          auto("developpement") == "développement", repr(auto("developpement")))
    check("accent (autoApply off): homographe DOMINANT restitué — 'etre'→'être'",
          auto("etre") == "être", repr(auto("etre")))
    check("accent: forme accentuée PAS assez dominante → garde le littéral ('mur'↛'mûr')",
          auto("mur") == "", repr(auto("mur")))
    check("accent: mot DÉJÀ accentué jamais touché ('français'→'')",
          auto("français") == "", repr(auto("français")))
    check("accent: complétion (lettres en +) n'est PAS un accent-restore ('bonjou'↛auto)",
          auto("bonjou") == "", repr(auto("bonjou")))
    set_config({"autoApply": False, "accentRestore": False})
    check("accent: accentRestore=false → désactivé ('developpement'→'')",
          auto("developpement") == "", repr(auto("developpement")))
    set_config({})  # restaure les defaults

    # 7quaterdecies) TRIGRAMMES APPRIS : le contexte 2-mots prime sur le bigramme.
    #   Même mot précédent ('vais') mais deux contextes différents → deux suites.
    #   Reconstruit côté daemon depuis la chaîne de commits (aucun prev2 envoyé).
    #   Sans trigrammes, seul le bigramme 'vais'→{manger,dormir} jouerait (à
    #   égalité) → l'un des deux checks échouerait. Avec, chaque contexte gagne.
    for _ in range(2):
        req({"learn": {"prev": "", "word": "je"}})
        req({"learn": {"prev": "je", "word": "vais"}})
        req({"learn": {"prev": "vais", "word": "manger"}})
    for _ in range(2):
        req({"learn": {"prev": "", "word": "tu"}})
        req({"learn": {"prev": "tu", "word": "vais"}})
        req({"learn": {"prev": "vais", "word": "dormir"}})
    c1 = cands("", ["je", "vais"])
    check("trigramme: 'je vais' → 'manger' en tête (pas 'dormir')",
          c1 and c1[0] == "manger", str(c1))
    c2 = cands("", ["tu", "vais"])
    check("trigramme: 'tu vais' → 'dormir' en tête (pas 'manger')",
          c2 and c2[0] == "dormir", str(c2))
    req({"forget": {"word": "manger"}})
    req({"forget": {"word": "dormir"}})

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
    # tolérance aux fautes : transposition et lettre en trop, seulement quand
    # le préfixe exact ne matche rien.
    c = cands(":ceour")
    check("emoji typo: ':ceour' (transposition) → ❤️", "❤️" in c, str(c))
    c = cands(":coeurr")
    check("emoji typo: ':coeurr' (lettre en trop) → ❤️", "❤️" in c, str(c))
    req({"learn": {"prev": "", "word": "⭐"}})
    req({"learn": {"prev": "", "word": "⭐"}})
    c = cands(":")
    check("emoji: favoris — ':' liste ⭐ après usage", c and c[0] == "⭐", str(c))
    check("emoji: grille NON plafonnée à 5 — ':' renvoie tout le set (7 distincts)",
          len(c) == 7, str(c))
    c = cands(":s")
    check("emoji: favori remonte — ':s' → ⭐ (star) avant 😊", c and c[0] == "⭐",
          str(c))
    c = cands("coeur")
    check("emoji: hint — 'coeur' (mot normal) propose ❤️ en fin de barre",
          "❤️" in c, str(c))

    # 8bis) RESTAURATION D'ACCENTS (fold-equal) + ghost découplé + barWords
    r = req({"prefix": "etre", "context": []})
    check("accent: etre → être (accentOnly)",
          r.get("autocomplete") == "être" and r.get("accentOnly") is True,
          str(r))
    r = req({"prefix": "francais", "context": []})
    check("accent: francais → français malgré le corpus (dominance ≥ accentDom)",
          r.get("autocomplete") == "français" and r.get("accentOnly") is True,
          str(r))
    r = req({"prefix": "cote", "context": []})
    check("accent: cote intouché (homographe sous accentDom)",
          r.get("autocomplete", "") in ("", "cote"), str(r))
    r = req({"prefix": "coeur", "context": []})
    check("accent: coeur → cœur (ligature, sans seuil de dominance)",
          r.get("autocomplete") == "cœur" and r.get("accentOnly") is True,
          str(r))

    cfgpath = f"{cfgdir}/config.json"
    def set_config(obj, bump):
        with open(cfgpath, "w", encoding="utf-8") as f:
            json.dump(obj, f)
        t = time.time() + bump  # mtime distinct → rechargement à chaud garanti
        os.utime(cfgpath, (t, t))

    set_config({"autoApply": False}, 2)
    r = req({"prefix": "bonjou", "context": []})
    check("autoApply off: l'Espace garde le littéral (pas d'autocomplete)",
          r.get("autocomplete", "") == "", str(r))
    check("autoApply off: le GHOST reste calculé (→ l'accepte)",
          r.get("ghost") == "bonjour", str(r))
    r = req({"prefix": "etre", "context": []})
    check("autoApply off: l'accent est restauré quand même (accentRestore)",
          r.get("autocomplete") == "être" and r.get("accentOnly") is True,
          str(r))
    # régression : un mot APPRIS (score plancher learnedFloor) ne doit pas
    # tuer sa propre restauration (garde `credible` auto-comparée).
    req({"learn": {"prev": "", "word": "être"}})
    r = req({"prefix": "etre", "context": []})
    check("autoApply off: l'accent survit à l'apprentissage du mot",
          r.get("autocomplete") == "être" and r.get("accentOnly") is True,
          str(r))
    req({"forget": {"word": "être"}})

    set_config({"autoApply": False, "accentRestore": False}, 4)
    r = req({"prefix": "etre", "context": []})
    check("autoApply off + accentRestore off: plus rien ne s'applique",
          r.get("autocomplete", "") == "", str(r))

    set_config({"barWords": 3}, 6)
    c = cands("v")
    check("barWords: la barre est tronquée aux 3 meilleurs", len(c) <= 3, str(c))
    set_config({}, 8)  # retour aux défauts pour la suite

    # 8bis) CACHE DE RÉCENCE : un mot déjà présent dans le texte avant le
    #       curseur (`wide`) est boosté (recencyBoost, défaut 1.3). Contexte
    #       VIDE ou neutre pour ne pas mélanger avec le boost de langue.
    c = cands("sol")
    check("récence: sans wide, solar (3500) devant soleil (3000)",
          c.index("solar") < c.index("soleil"), str(c))
    c = req({"prefix": "sol", "context": [],
             "wide": "le soleil brille"})["candidates"]
    check("récence: 'soleil' dans wide → passe devant solar",
          c.index("soleil") < c.index("solar"), str(c))
    req({"forget": {"word": "vais"}})  # appris plus haut (7) — repartir du modèle
    c = req({"prefix": "", "context": ["je"],
             "wide": "tu veux quoi je"})["candidates"]
    check("récence: mot-suivant, 'veux' (0.30) dans wide → devant vais (0.35)",
          c.index("veux") < c.index("vais"), str(c))

    # 8ter) REFORMULATION — worker différé (A1), cache LRU (A3), non-blocage.
    #       Mock HTTP OpenAI-compatible local ; la clé vient de l'env
    #       (GROQ_API_KEY, cf lancement du daemon).
    import http.server
    import threading
    hits = []
    bodies = []  # corps des requêtes → vérifie le prompt système (langue)
    slow = [False]

    unauth = [False]
    bullets = [False]

    class Mock(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            try:
                bodies.append(json.loads(raw))
            except ValueError:
                pass
            hits.append(1)
            if slow[0]:
                time.sleep(1.2)
            if unauth[0]:
                body = b'{"error":{"message":"Invalid API Key"}}'
                self.send_response(401)
            elif bullets[0]:
                # Puces/ponctuation multi-octets en tête de ligne : le trim ne
                # doit pas couper un caractère UTF-8 en deux.
                body = json.dumps({"choices": [{"message": {"content":
                    "• Variante à puce.\n"
                    "— Variante à tiret cadratin.\n"
                    "… Variante à points de suspension."}}]}).encode()
                self.send_response(200)
            else:
                body = json.dumps({"choices": [{"message": {"content":
                    "Variante numéro un.\nVariante numéro deux.\n"
                    "Variante numéro trois.\nVariante numéro quatre."}}]}
                    ).encode()
                self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", 0), Mock)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    set_config({"reformBaseUrl": f"http://127.0.0.1:{httpd.server_port}",
                "reformTimeoutMs": 3000}, 10)
    r = req({"reformulate": "bonjour tout le monde", "n": 2,
             "mode": "rephrase", "nonce": 0})
    check("reformulate: 2 variantes via worker (réponse différée)",
          len(r.get("variants", [])) == 2 and r.get("source") == "groq",
          str(r))
    r2 = req({"reformulate": "bonjour tout le monde", "n": 2,
              "mode": "rephrase", "nonce": 0})
    check("reformulate: cache LRU — même demande servie sans nouvel appel HTTP",
          r2.get("variants") == r.get("variants") and len(hits) == 1,
          f"hits={len(hits)}")
    r3 = req({"reformulate": "bonjour tout le monde", "n": 2,
              "mode": "rephrase", "nonce": 1})
    check("reformulate: nonce différent → régénère",
          len(hits) == 2 and len(r3.get("variants", [])) == 2,
          f"hits={len(hits)}")
    # NON-BLOCAGE (le cœur de A1) : pendant une reformulation LENTE (mock
    # 1,2 s), la complétion répond en millisecondes sur une autre connexion.
    slow[0] = True
    got = {}

    def bg():
        got["r"] = req({"reformulate": "une autre phrase pour le test",
                        "n": 2, "mode": "rephrase", "nonce": 0})

    t = threading.Thread(target=bg)
    t.start()
    time.sleep(0.15)  # la demande est partie, le worker dort dans le mock
    t0 = time.monotonic()
    c = cands("bonjou")
    dt = time.monotonic() - t0
    check("reformulate: le poll loop ne gèle pas (complétion < 300 ms pendant "
          "une reformulation lente)", "bonjour" in c and dt < 0.3,
          f"dt={dt:.3f}s {c}")
    t.join()
    check("reformulate: la reformulation lente aboutit quand même",
          len(got["r"].get("variants", [])) == 2, str(got.get("r")))
    slow[0] = False
    # Le trim de tête ne travaille qu'en ASCII : les puces multi-octets sont
    # retirées ENTIÈRES ("•", "—"), et un caractère qui n'est pas une puce ("…")
    # est conservé intact. Avant, le jeu de caractères contenait les octets de
    # "•", donc "…" (E2 80 A6) perdait E2 80 et l'octet A6 orphelin faisait
    # rejeter toute la ligne par validUtf8 — variante perdue en silence.
    bullets[0] = True
    r = req({"reformulate": "phrase avec des puces typographiques", "n": 3,
             "mode": "rephrase", "nonce": 0})
    check("reformulate: puces multi-octets retirées entières, pas d'UTF-8 coupé",
          r.get("variants") == ["Variante à puce.",
                                "Variante à tiret cadratin.",
                                "… Variante à points de suspension."], str(r))
    bullets[0] = False
    # Groq-only + kinds d'erreur : 401 → error "auth" (l'engine affiche le
    # panneau « clé refusée — Entrée : reconfigurer »), jamais mis en cache.
    unauth[0] = True
    r = req({"reformulate": "phrase pour tester l'auth", "n": 2,
             "mode": "rephrase", "nonce": 0})
    check("reformulate: 401 → error 'auth', aucune variante",
          r.get("error") == "auth" and r.get("variants") == [], str(r))
    # Le header Authorization ne doit jamais partir en clair : un reformBaseUrl
    # http:// hors boucle locale est refusé avant même la lecture de la clé.
    # (127.0.0.1 reste autorisé — c'est ce que fait le mock ci-dessus.)
    before = len(hits)
    set_config({"reformBaseUrl": "http://api.groq.com/openai/v1/chat/completions",
                "reformTimeoutMs": 3000}, 11)
    r = req({"reformulate": "phrase envoyée en clair", "n": 2,
             "mode": "rephrase", "nonce": 0})
    check("reformulate: baseUrl http:// distant → error 'bad_url', aucun appel",
          r.get("error") == "bad_url" and r.get("variants") == [] and
          len(hits) == before, str(r))
    set_config({"reformBaseUrl": f"http://127.0.0.1:{httpd.server_port}",
                "reformTimeoutMs": 3000}, 12)
    # reformCheck : validation de clé (dialogue --groq-key). Clé refusée…
    r = req({"reformCheck": True})
    check("reformCheck: clé refusée → keyValid false, error auth",
          r.get("keyPresent") is True and r.get("keyValid") is False and
          r.get("error") == "auth", str(r))
    # …puis acceptée (l'API répond 200) : keyValid true.
    unauth[0] = False
    r = req({"reformCheck": True})
    check("reformCheck: clé acceptée → keyValid true",
          r.get("keyValid") is True and r.get("keyPresent") is True, str(r))
    # ÉPINGLAGE DE LANGUE : cfg.lang choisi ("fr"/"en") PRIME sur l'heuristique
    # de détection de la phrase ; "auto" (défaut) garde l'heuristique.
    base = {"reformBaseUrl": f"http://127.0.0.1:{httpd.server_port}",
            "reformTimeoutMs": 3000}
    set_config({**base, "lang": "en"}, 13)
    req({"reformulate": "bonjour à tous les amis", "n": 2,
         "mode": "rephrase", "nonce": 0})
    sysp = bodies[-1]["messages"][0]["content"]
    check("reformulate: lang=en épingle l'anglais malgré une phrase FR",
          "English" in sysp, sysp[:70])
    set_config({**base, "lang": "fr"}, 14)
    req({"reformulate": "the quick brown fox jumps over the fence", "n": 2,
         "mode": "rephrase", "nonce": 0})
    sysp = bodies[-1]["messages"][0]["content"]
    check("reformulate: lang=fr épingle le français malgré une phrase EN",
          "français" in sysp, sysp[:70])
    set_config({**base, "lang": "auto"}, 15)
    req({"reformulate": "the quick brown fox jumps over the fence", "n": 2,
         "mode": "rephrase", "nonce": 1})
    sysp = bodies[-1]["messages"][0]["content"]
    check("reformulate: lang=auto garde l'heuristique (EN détecté)",
          "English" in sysp, sysp[:70])
    httpd.shutdown()
    set_config({}, 16)

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
