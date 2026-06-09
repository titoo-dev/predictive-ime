#!/usr/bin/env python3
# Harness d'ÉVALUATION du modèle de prédiction — chiffres objectifs, mesurés à
# travers le vrai protocole socket du daemon (donc exactement ce que voit
# l'engine). À lancer sur des phrases HELD-OUT (autre année/corpus que le
# training) pour mesurer la généralisation, pas la mémorisation.
#
# Métriques :
#   - MOT-SUIVANT  : hit@1 / hit@3 / hit@6 — le mot réel est-il dans les
#     candidats prédits depuis le contexte (barre mot-suivant) ?
#   - COMPLÉTION   : auto-KSR — % de caractères économisés si l'utilisateur
#     laisse Espace auto-compléter dès que `autocomplete` == le mot visé
#     (Espace est tapé de toute façon → chaque caractère non tapé est gagné) ;
#     top3@2 — % de mots (len>=4) dans le top-3 après 2 caractères tapés.
#   - AUTOCORRECTION : une faute synthétique par mot (transposition, voisin
#     AZERTY, lettre en trop) → le mot original est-il récupéré (top-1/top-3) ?
#   - LATENCE      : p50/p99 du round-trip socket par requête.
#
# usage: eval_model.py <predictord> <words.tsv> <sentences.txt>...
#                      [--max-sentences N] [--label-par-fichier]
import argparse, json, os, re, socket, subprocess, tempfile, time, zlib

TOK = re.compile(r"[^\W\d_]+(?:['-][^\W\d_]+)*", re.UNICODE)
AZERTY = {
    'a': 'zq', 'z': 'aes', 'e': 'zrd', 'r': 'etf', 't': 'ryg', 'y': 'tuh',
    'u': 'yij', 'i': 'uok', 'o': 'ipl', 'p': 'om', 'q': 'asw', 's': 'qdzx',
    'd': 'sfec', 'f': 'dgrv', 'g': 'fhtb', 'h': 'gjyn', 'j': 'hku',
    'k': 'jli', 'l': 'kmo', 'm': 'lp', 'w': 'qx', 'x': 'wcs', 'c': 'xvd',
    'v': 'cbf', 'b': 'vng', 'n': 'bh',
}


def sentences(path, limit):
    out = []
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            text = line[tab + 1:] if tab >= 0 else line
            toks = TOK.findall(text.replace("’", "'").lower())
            if len(toks) >= 3:
                out.append(toks)
            if len(out) >= limit:
                break
    return out


def synth_typo(w, salt):
    """Une faute déterministe par mot (crc32, PAS hash() qui est randomisé
    par processus → les runs seraient incomparables)."""
    h = (zlib.crc32(w.encode()) ^ salt) & 0x7FFFFFFF
    core = [i for i, ch in enumerate(w) if ch.isalpha()]
    if len(core) < 4:
        return None
    kind = h % 3
    i = core[1 + h % (len(core) - 2)]  # jamais la 1re lettre (peu réaliste)
    if kind == 0 and i + 1 in core:  # transposition
        return w[:i] + w[i + 1] + w[i] + w[i + 2:]
    if kind == 1 and w[i] in AZERTY:  # substitution voisin AZERTY
        nb = AZERTY[w[i]]
        return w[:i] + nb[h % len(nb)] + w[i + 1:]
    return w[:i] + w[i] + w[i:]  # doublement de lettre (= lettre en trop)


class Daemon:
    def __init__(self, binpath, words):
        self.tmp = tempfile.mkdtemp(prefix="ime-eval-")
        self.sock_path = os.path.join(self.tmp, "sock")
        # ISOLATION complète : ni l'apprentissage réel ni la config perso de
        # la machine ne doivent fausser la mesure.
        env = dict(os.environ, XDG_DATA_HOME=f"{self.tmp}/xdg",
                   XDG_CONFIG_HOME=f"{self.tmp}/cfg")
        self.proc = subprocess.Popen([binpath, words, self.sock_path], env=env,
                                     stderr=subprocess.DEVNULL)
        for _ in range(600):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.05)
        time.sleep(0.3)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sock_path)
        self.buf = b""
        self.lat = []

    def query(self, context, prefix):
        t0 = time.perf_counter()
        self.sock.sendall(
            (json.dumps({"context": context, "prefix": prefix}) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("daemon mort")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        self.lat.append(time.perf_counter() - t0)
        return json.loads(line)

    def close(self):
        self.sock.close()
        self.proc.terminate()
        self.proc.wait()


def evaluate(daemon, sents, vocab):
    nw = {"n": 0, "hit1": 0, "hit3": 0, "hit6": 0, "iv": 0,
          "iv_hit1": 0, "iv_hit3": 0}
    comp = {"chars": 0, "saved": 0, "words": 0, "top3at2": 0}
    fix = {"n": 0, "top1": 0, "top3": 0}

    for si, toks in enumerate(sents):
        for i in range(1, len(toks)):
            ctx = toks[max(0, i - 2):i]
            w = toks[i]

            # --- mot-suivant
            r = daemon.query(ctx, "")
            c = r["candidates"]
            nw["n"] += 1
            iv = w in vocab
            nw["iv"] += iv
            if w in c[:1]:
                nw["hit1"] += 1; nw["iv_hit1"] += iv
            if w in c[:3]:
                nw["hit3"] += 1; nw["iv_hit3"] += iv
            if w in c[:6]:
                nw["hit6"] += 1

            if len(w) < 4 or not iv:
                continue

            # --- complétion : plus petit préfixe L où Espace auto-complète
            comp["words"] += 1
            comp["chars"] += len(w)
            for L in range(1, len(w)):
                r = daemon.query(ctx, w[:L])
                if L == 2 and w in r["candidates"][:3]:
                    comp["top3at2"] += 1
                if (r.get("autocomplete") == w
                        and not r.get("literalIsWord", False)):
                    comp["saved"] += len(w) - L
                    break

            # --- autocorrection (1 mot sur 3 pour limiter le coût)
            if (si + i) % 3 == 0:
                typo = synth_typo(w, 0x5EED)
                if typo and typo != w and typo not in vocab:
                    r = daemon.query(ctx, typo)
                    c = r["candidates"]
                    fix["n"] += 1
                    fix["top1"] += w in c[:1]
                    fix["top3"] += w in c[:3]
    return nw, comp, fix


def pct(a, b):
    return f"{100.0 * a / b:5.1f}%" if b else "  n/a"


def report(tag, nw, comp, fix):
    print(f"\n== {tag} ==")
    print(f"  mot-suivant   ({nw['n']} prédictions, "
          f"{pct(nw['iv'], nw['n'])} in-vocab)")
    print(f"    hit@1 {pct(nw['hit1'], nw['n'])}   "
          f"hit@3 {pct(nw['hit3'], nw['n'])}   "
          f"hit@6 {pct(nw['hit6'], nw['n'])}")
    print(f"    in-vocab seulement : hit@1 {pct(nw['iv_hit1'], nw['iv'])}   "
          f"hit@3 {pct(nw['iv_hit3'], nw['iv'])}")
    print(f"  complétion    ({comp['words']} mots len>=4 in-vocab)")
    print(f"    auto-KSR (Espace) {pct(comp['saved'], comp['chars'])} "
          f"des caractères économisés   top3@2 {pct(comp['top3at2'], comp['words'])}")
    print(f"  autocorrection ({fix['n']} fautes synthétiques)")
    print(f"    récupéré top-1 {pct(fix['top1'], fix['n'])}   "
          f"top-3 {pct(fix['top3'], fix['n'])}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("predictord")
    ap.add_argument("words")
    ap.add_argument("sentences", nargs="+")
    ap.add_argument("--max-sentences", type=int, default=400)
    args = ap.parse_args()

    vocab = set()
    with open(args.words, encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if p:
                vocab.add(p[0])

    daemon = Daemon(args.predictord, args.words)
    try:
        tot = None
        for path in args.sentences:
            sents = sentences(path, args.max_sentences)
            res = evaluate(daemon, sents, vocab)
            report(os.path.basename(path), *res)
            if tot is None:
                tot = [dict(r) for r in res]
            else:
                for d, r in zip(tot, res):
                    for k in d:
                        d[k] += r[k]
        if len(args.sentences) > 1:
            report("TOTAL", *tot)
        lat = sorted(daemon.lat)
        print(f"\n  latence socket : p50 "
              f"{1e6 * lat[len(lat) // 2]:.0f}µs   p99 "
              f"{1e6 * lat[int(len(lat) * 0.99)]:.0f}µs   "
              f"({len(lat)} requêtes)")
    finally:
        daemon.close()


if __name__ == "__main__":
    main()
