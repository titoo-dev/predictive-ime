# Contexte de phrase + accord grammatical — Plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Exploiter toute la phrase (via SurroundingText + lexique Lefff) pour accorder les suggestions en nombre et genre, en restant dans le moteur n-gramme rapide.

**Architecture:** Le moteur envoie une fenêtre de contexte large issue de SurroundingText. Le daemon charge une table morphologique (dérivée de Lefff, filtrée au vocab), détecte le déterminant gouverneur dans la fenêtre, et booste (jamais ne filtre) les candidats accordés. Le n-gramme et la langue stricte restent inchangés.

**Tech Stack:** C++17 (daemon + engine fcitx5), Nix (flake build), Python3 (parsing build-time), test via `nc -U` socket + `nix build`.

## Global Constraints

- Lexique Lefff : `https://huggingface.co/datasets/sagot/lefff_morpho/resolve/main/lefff_morpho-3.5.json`, licence **LGPL-LR** (redistribuable ; attribuer dans `NOTICE-DATASETS.md`).
- **Jamais de filtre dur** : l'accord réordonne par multiplication de score, ne supprime aucun candidat.
- Compatibilité protocole socket inchangée : `{"context":[...],"prefix":"..."}`.
- La langue stricte (`langFactor`→0) prime : l'accord ne ressuscite jamais un score ≤ 0.
- Réglages rechargés à chaud via `config.json` (pas de restart).
- Daemon mono-thread `poll()` : pas de blocage ; chargements au boot uniquement.

---

### Task 1 : Lexique Lefff → `morph.tsv` (build du modèle)

**Files:**
- Modify: `flake.nix` (let-bindings fetchurl + dérivation `ime-model`)
- Modify: `NOTICE-DATASETS.md`

**Interfaces:**
- Produces: fichier `$out/morph.tsv` dans la sortie `ime-model`, lignes
  `forme<TAB>genre(m|f|-)<TAB>nombre(s|p|-)<TAB>lemme`, restreint aux formes
  présentes dans `words.tsv`.

- [ ] **Step 1 : épingler le hash + inspecter le format**

```sh
nix store prefetch-file --json \
  https://huggingface.co/datasets/sagot/lefff_morpho/resolve/main/lefff_morpho-3.5.json
# → note le champ "hash" (sha256-…) pour fetchurl.
# Inspecter un échantillon nc/adj pour confirmer l'encodage msfeatures :
F=$(nix store prefetch-file --print-path https://huggingface.co/datasets/sagot/lefff_morpho/resolve/main/lefff_morpho-3.5.json 2>/dev/null | tail -1)
python3 -c "import json;d=json.load(open('$F'));[print(e) for e in d if e['category'] in ('nc','adj')][:0];print([e for e in d if e['category']=='nc'][:5])"
```
Attendu : entrées type `{'form':'chats','lemma':'chat','category':'nc','msfeatures':'mp',...}` (codes `ms/mp/fs/fp`). Si l'encodage diffère, adapter l'extraction au Step 3.

- [ ] **Step 2 : ajouter le fetchurl dans flake.nix**

Dans le bloc `let … in` des sources (à côté de `fr50k`, `fraNews`…) :
```nix
      lefff = pkgs.fetchurl {
        url = "https://huggingface.co/datasets/sagot/lefff_morpho/resolve/main/lefff_morpho-3.5.json";
        hash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="; # ← hash du Step 1
      };
```

- [ ] **Step 3 : générer morph.tsv dans la dérivation ime-model**

Ajouter `python3` à `nativeBuildInputs` (déjà présent). Après la génération de
`$out/words.tsv`, insérer :
```nix
        # 3) morph.tsv — genre/nombre par forme (Lefff), filtré au vocabulaire.
        python3 ${./scripts/build_morph.py} ${lefff} $out/words.tsv > $out/morph.tsv
        echo "morph.tsv: $(wc -l < $out/morph.tsv) formes" >&2
```
Créer `scripts/build_morph.py` :
```python
import json, sys
lefff_path, words_path = sys.argv[1], sys.argv[2]
vocab = set()
for line in open(words_path, encoding="utf-8"):
    p = line.split()
    if p: vocab.add(p[0])
out = {}
for e in json.load(open(lefff_path, encoding="utf-8")):
    if e.get("category") not in ("nc", "adj"):  # noms communs + adjectifs
        continue
    form = e.get("form", "")
    if form not in vocab:
        continue
    f = e.get("msfeatures", "") or ""
    g = "m" if "m" in f else "f" if "f" in f else "-"
    n = "p" if "p" in f else "s" if "s" in f else "-"
    if g == "-" and n == "-":
        continue
    # une forme peut avoir plusieurs analyses ; on garde la 1re non ambiguë
    out.setdefault(form, (g, n, e.get("lemma", form)))
for form, (g, n, lemma) in sorted(out.items()):
    print(f"{form}\t{g}\t{n}\t{lemma}")
```

- [ ] **Step 4 : build + vérifier**

```sh
M=$(nix build .#model --no-link --print-out-paths)
grep -E '^(chats|chat|chevaux|cheval|belles|belle|grande|grand)\b' "$M/morph.tsv"
```
Attendu : `chats	m	p	chat`, `chat	m	s	chat`, `belles	f	p	beau` (ou `belle`),
`grande	f	s	grand`, etc.

- [ ] **Step 5 : NOTICE + commit**

Ajouter à `NOTICE-DATASETS.md` une entrée Lefff 3.5 (Sagot, INRIA, LGPL-LR, URL HF).
```sh
git add flake.nix scripts/build_morph.py NOTICE-DATASETS.md
git commit -m "feat(model): table morph.tsv (genre/nombre) dérivée de Lefff"
```

---

### Task 2 : Daemon — charger morph.tsv

**Files:**
- Modify: `daemon/predictord.cpp` (struct Model : nouveaux membres + `loadMorph`, appel dans `main`)

**Interfaces:**
- Consumes: `$dir/morph.tsv` (Task 1).
- Produces: `struct Morph { uint8_t g; uint8_t n; };` ; membres
  `std::unordered_map<std::string, Morph> morph_;` (clé = forme, via `lowerKeep`),
  encodage `g`/`n` : 0=indéterminé, 1=masc/sing, 2=fém/plur.
  Méthode `void loadMorph(const std::string &dir);` appelée dans `main` après
  `loadEmoji`.

- [ ] **Step 1 : test (socket) — le daemon démarre toujours avec morph.tsv présent**

(Pas de framework C++ ; test = build + run + `nc`.) On valide d'abord que le
chargement ne casse rien :
```sh
M=$(nix build .#model --no-link --print-out-paths)
D=$(nix build .#predictord --no-link --print-out-paths)
"$D/bin/predictord" "$M/words.tsv" /tmp/t.sock & sleep 3
printf '{"stats":true}\n' | nc -U /tmp/t.sock; kill %1
```
Attendu (après implémentation) : stats contient `"morph":<n>` (formes chargées).

- [ ] **Step 2 : implémenter loadMorph + membres**

Dans `struct Model`, près des autres `load*` :
```cpp
  struct Morph { uint8_t g = 0; uint8_t n = 0; }; // g:1=m,2=f  n:1=s,2=p
  std::unordered_map<std::string, Morph> morph_;

  void loadMorph(const std::string &dir) {
    std::ifstream f(dir + "morph.tsv");
    std::string line;
    while (std::getline(f, line)) {
      size_t a = line.find('\t');
      if (a == std::string::npos) continue;
      size_t b = line.find('\t', a + 1);
      size_t c = line.find('\t', b + 1);
      if (b == std::string::npos || c == std::string::npos) continue;
      Morph m;
      char gc = line[a + 1], nc = line[b + 1];
      m.g = gc == 'm' ? 1 : gc == 'f' ? 2 : 0;
      m.n = nc == 's' ? 1 : nc == 'p' ? 2 : 0;
      morph_[lowerKeep(line.substr(0, a))] = m;
    }
    fprintf(stderr, "[predictord] %zu formes morpho chargées\n", morph_.size());
  }
```
Dans `main`, après `model.loadEmoji(dir);` :
```cpp
  model.loadMorph(dir);
```
Dans `stats()` (méthode existante), ajouter `j["morph"] = morph_.size();`.

- [ ] **Step 3 : build + run + vérifier stats**

```sh
M=$(nix build .#model --no-link --print-out-paths); D=$(nix build .#predictord --no-link --print-out-paths)
"$D/bin/predictord" "$M/words.tsv" /tmp/t.sock & sleep 3
printf '{"stats":true}\n' | nc -U /tmp/t.sock | grep -o '"morph":[0-9]*'; kill %1
```
Attendu : `"morph":<plusieurs milliers>`.

- [ ] **Step 4 : commit**
```sh
git add daemon/predictord.cpp && git commit -m "feat(daemon): charge morph.tsv (genre/nombre)"
```

---

### Task 3 : Daemon — détection du gouverneur d'accord

**Files:**
- Modify: `daemon/predictord.cpp` (méthode `agreementOf` + tables de déterminants)

**Interfaces:**
- Produces: `struct Agree { uint8_t g; uint8_t n; };` (0=libre) et
  `Agree agreementOf(const std::vector<std::string> &context) const;`
  — scanne le contexte de la fin vers le début, renvoie le genre/nombre imposé
  par le déterminant gouverneur le plus proche, en traversant les adjectifs mais
  en s'arrêtant à un briseur de groupe nominal (verbe Lefff / ponctuation /
  conjonction).

- [ ] **Step 1 : test socket (rouge)** — `les petits chat` doit donner `chats` en tête (échouera avant Task 4, mais on fige le cas)

```sh
printf '{"context":["les","petits"],"prefix":"chat"}\n' | nc -U /tmp/t.sock
```
Attendu APRÈS Task 4 : `chats` avant `chat`. (Task 3 ne fait que la détection.)

- [ ] **Step 2 : implémenter agreementOf**

```cpp
  struct Agree { uint8_t g = 0; uint8_t n = 0; };

  // Déterminants gouverneurs (clés en lowerKeep). 0 = pas une contrainte.
  Agree determiner(const std::string &w) const {
    static const std::unordered_map<std::string, Agree> D = {
      {"les",{0,2}}, {"des",{0,2}}, {"mes",{0,2}}, {"tes",{0,2}}, {"ses",{0,2}},
      {"nos",{0,2}}, {"vos",{0,2}}, {"leurs",{0,2}}, {"ces",{0,2}}, {"aux",{0,2}},
      {"quelques",{0,2}}, {"plusieurs",{0,2}}, {"certains",{1,2}}, {"certaines",{2,2}},
      {"deux",{0,2}}, {"trois",{0,2}}, {"quatre",{0,2}}, {"cinq",{0,2}},
      {"le",{1,1}}, {"un",{1,1}}, {"ce",{1,1}}, {"cet",{1,1}}, {"mon",{1,1}},
      {"ton",{1,1}}, {"son",{1,1}}, {"la",{2,1}}, {"une",{2,1}}, {"cette",{2,1}},
      {"ma",{2,1}}, {"ta",{2,1}}, {"sa",{2,1}}, {"l'",{0,1}},
    };
    auto it = D.find(w); return it == D.end() ? Agree{0,0} : it->second;
  }

  bool npBreaker(const std::string &w) const {
    static const std::unordered_set<std::string> C = {"et","ou","mais","donc","car","ni","or"};
    if (C.count(w)) return true;
    auto it = morph_.end(); (void)it;
    auto idit = id_.find(w);                 // verbe Lefff ? → brise le SN
    // (catégorie verbe non stockée dans morph_ ; approx : un mot connu comme
    //  forme verbale courante. Simplification : on ne traite que conj./ponct.)
    return false;
  }

  Agree agreementOf(const std::vector<std::string> &context) const {
    // de la fin vers le début, max 5 mots avant le candidat
    int steps = 0;
    for (auto it = context.rbegin(); it != context.rend() && steps < 5; ++it, ++steps) {
      std::string w = lowerKeep(*it);
      if (npBreaker(w)) break;             // nouvelle proposition → pas d'accord
      Agree a = determiner(w);
      if (a.g || a.n) return a;            // déterminant gouverneur trouvé
      // sinon (adjectif/nom intercalé) on continue de remonter
    }
    return {0, 0};
  }
```

- [ ] **Step 3 : build (compile only)**
```sh
nix build .#predictord --no-link 2>&1 | tail -3   # doit compiler
```
- [ ] **Step 4 : commit**
```sh
git add daemon/predictord.cpp && git commit -m "feat(daemon): détection du gouverneur d'accord (déterminants)"
```

---

### Task 4 : Daemon — appliquer le boost d'accord

**Files:**
- Modify: `daemon/predictord.cpp` (`agreeFactor`, intégration dans `completePrefix` et `predictNext`, config `agreeBoost`)

**Interfaces:**
- Consumes: `morph_` (T2), `agreementOf` (T3), `langFactor` (existant).
- Produces: `double agreeFactor(const Agree &want, uint32_t wid) const;`
  multiplié dans `scoreOf` (completePrefix) et dans le scoring de `predictNext`.
  Config : `double agreeBoost = 2.0;` (rechargée à chaud comme `langBoost`).

- [ ] **Step 1 : implémenter agreeFactor**
```cpp
  // Facteur d'accord : ×agreeBoost si la forme du candidat s'accorde, ÷agreeBoost
  // si elle est connue ET ne s'accorde pas. Neutre (1.0) si hors-morpho ou si la
  // contrainte est libre sur cette dimension. Jamais 0 (pas de filtre dur).
  double agreeFactor(const Agree &want, uint32_t wid) const {
    if ((!want.g && !want.n) || wid >= words.size()) return 1.0;
    auto it = morph_.find(words[wid]);
    if (it == morph_.end()) return 1.0;     // hors-lexique → neutre
    const Morph &m = it->second;
    double f = 1.0;
    if (want.n && m.n) f *= (m.n == want.n) ? cfg.agreeBoost : 1.0 / cfg.agreeBoost;
    if (want.g && m.g) f *= (m.g == want.g) ? cfg.agreeBoost : 1.0 / cfg.agreeBoost;
    return f;
  }
```
Ajouter `double agreeBoost = 2.0;` dans la struct `Config`, et le rechargement
dans `maybeReload` à côté de `langBoost` :
```cpp
          fresh.agreeBoost = j.value("agreeBoost", fresh.agreeBoost);
```

- [ ] **Step 2 : intégrer dans completePrefix**

Calculer `Agree want = agreementOf(context);` près de `uint8_t ctxL = ctxLang(context);`,
puis dans `scoreOf` multiplier aussi par `agreeFactor` :
```cpp
    Agree want = agreementOf(context);
    auto scoreOf = [&](uint32_t wid) -> double {
      double s = hasCtx ? ctxScore(wid) : (double(freq[wid]) + 1.0) / freqTot_;
      return s * langFactor(ctxL, wid) * agreeFactor(want, wid);
    };
```

- [ ] **Step 3 : intégrer dans predictNext**

Après `const uint8_t ctxL = ctxLang(ctx);` ajouter `const Agree want = agreementOf(ctx);`
et multiplier les scores des suiveurs modèle par `agreeFactor(want, pr.first)` /
`agreeFactor(want, w)` (mêmes 3 emplacements que `langFactor`).

- [ ] **Step 4 : test socket (vert)**
```sh
M=$(nix build .#model --no-link --print-out-paths); D=$(nix build .#predictord --no-link --print-out-paths)
"$D/bin/predictord" "$M/words.tsv" /tmp/t.sock & sleep 3
for r in '{"context":["les","petits"],"prefix":"chat"}' '{"context":["des"],"prefix":"belle"}' '{"context":["une"],"prefix":"grand"}' '{"context":["un"],"prefix":"enfant"}'; do
  printf '%s -> ' "$r"; printf '%s\n' "$r" | nc -U /tmp/t.sock; done; kill %1
```
Attendu : `chats` devant `chat` ; `belles` devant `belle` ; `grande` devant `grand` ;
`un enfant` reste singulier (`enfant` devant `enfants`).

- [ ] **Step 5 : non-régression langue + commit**
```sh
printf '{"context":["le"],"prefix":"t"}\n' | nc -U /tmp/t.sock   # 0 mot anglais
git add daemon/predictord.cpp && git commit -m "feat(daemon): boost d'accord nombre/genre (Lefff)"
```

---

### Task 5 : Moteur — contexte SurroundingText primaire + fenêtre 8

**Files:**
- Modify: `engine/predict.cpp:746-760` (`contextFor`), `:764-768` (`pushCtx` garde 8)

**Interfaces:**
- Produces: `contextFor` renvoie en priorité les ≤8 derniers mots de
  `SurroundingText` (avant le curseur), repli `state->ctx`.

- [ ] **Step 1 : inverser la priorité dans contextFor**
```cpp
  std::vector<std::string> contextFor(fcitx::InputContext *ic,
                                      PredictState *state) {
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
      auto cps = decodeUtf8(ic->surroundingText().text());
      unsigned int cur = ic->surroundingText().cursor();
      if (cur < cps.size()) cps.resize(cur);
      auto ws = lastWords(cps, 8);          // fenêtre élargie pour l'accord
      if (!ws.empty()) return ws;
    }
    return state->ctx;                      // repli : mots committés par l'IME
  }
```
Et `pushCtx` : garder jusqu'à 8 (au lieu de 4) :
```cpp
    if (state->ctx.size() > 8) state->ctx.erase(state->ctx.begin());
```

- [ ] **Step 2 : build (compile)** `nix build .#qmlpanel .#predict --no-link 2>&1 | tail -3` (ou le paquet engine adéquat).

- [ ] **Step 3 : e2e kwrite (manuel via harness validé)**
Lancer le fcitx5 patché (`config.i18n.inputMethod.package` via override-input),
taper « les petits cha » + Tab dans kwrite, vérifier la complétion `chats`.
(Procédure kwrite+wtype déjà éprouvée dans le projet.)

- [ ] **Step 4 : commit**
```sh
git add engine/predict.cpp && git commit -m "feat(engine): contexte primaire SurroundingText, fenêtre 8 mots"
```

---

### Task 6 : Réglage agreeBoost dans config + doc

**Files:**
- Modify: `daemon/predictord.cpp` (commentaire config), `README.md` (doc agreeBoost),
  `dotfiles/ime-predictord/config.json` côté nix-config (ajout `agreeBoost`, optionnel)

**Interfaces:** aucune nouvelle ; expose le réglage.

- [ ] **Step 1 : documenter agreeBoost** dans le bloc de commentaires de la config
  (daemon) et le README (section réglages), valeur défaut 2.0, « monte = accord
  plus agressif ».
- [ ] **Step 2 : commit**
```sh
git add daemon/predictord.cpp README.md && git commit -m "docs: réglage agreeBoost (boost d'accord)"
```

---

### Task 7 : Déploiement

**Files:** push predictive-ime ; nix-config `flake.lock`.

- [ ] **Step 1 : push**
```sh
git push origin HEAD
```
- [ ] **Step 2 : bump nix-config**
```sh
cd ~/personal && nix flake update predictive-ime && nixos-rebuild build --flake .#thorfinn 2>&1 | tail -3
```
- [ ] **Step 3 : activer + vérifier e2e**
`make rebuild`, basculer fcitx5 en `predict`, tester « les petits cha » → `chats`
dans kwrite/Firefox. Vérifier non-régression (anglais toujours exclu, apostrophes OK).

## Self-Review

- **Couverture spec :** C1 contexte→T5 ; C2 modèle→T1 ; C3 daemon (charge/détecte/applique)→T2/T3/T4 ; config→T6 ; déploiement→T7 ; tests→intégrés par tâche. ✓
- **Placeholders :** seul le `hash` Lefff est `AAAA…` — c'est l'objet explicite du Task 1 Step 1 (le hash nix ne peut être connu qu'au fetch). Encodage `msfeatures` confirmé au Task 1 Step 1, parseur adapté si besoin. Pas d'autre TBD.
- **Cohérence types :** `Agree{g,n}`, `Morph{g,n}` (1=m/s, 2=f/p, 0=libre) cohérents T2→T4 ; `agreeFactor(Agree, wid)`, `agreementOf(context)`, `cfg.agreeBoost` cohérents. ✓
- **Risque connu :** `npBreaker` simplifié (conj./ponct. seulement ; pas de détection verbale fine faute de catégorie verbale dans `morph_`). Acceptable best-effort ; raffinable si faux positifs (ajouter une table de formes verbales fréquentes ou stocker la catégorie Lefff).
