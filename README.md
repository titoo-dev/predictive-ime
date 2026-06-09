# IME prédictif maison (fcitx5 engine + daemon n-gram)

Implémentation de l'**Option A** validée dans
[`../docs/wiki/custom-ime-research.md`](../docs/wiki/custom-ime-research.md) :
un engine fcitx5 fin (réutilise le frontend Wayland éprouvé sur Hyprland 0.55.2)
qui délègue la prédiction à un daemon externe via socket Unix + JSON (patron
`fcitx5-azookeyd`).

## Structure

```
ime/
  flake.nix              # build des 2 paquets + module NixOS
  engine/                # Track A — addon fcitx5 (C++20)
    predict.cpp          #   InputMethodEngineV2: buffer→preedit, query daemon, candidats
    predict-addon.conf   #   enregistrement addon  → share/fcitx5/addon/predict.conf
    predict.conf         #   méthode de saisie     → share/fcitx5/inputmethod/predict.conf
    CMakeLists.txt
  daemon/                # Track B — cerveau + câblage
    predictord.cpp       #   socket Unix + JSON ; complétion préfixe + bigramme (FR+EN)
    bench_ngram.cpp      #   "spike" latence (n-gram synthétique à l'échelle réelle)
    sample-corpus.txt    #   corpus de démo FR+EN
    CMakeLists.txt
```

## Protocole socket (engine ↔ daemon)

```
-> {"context":["je"],"prefix":"v"}
<- {"candidates":["veux","vais","vous","va",...],"literalIsWord":false}
-> {"learn":{"prev":"je","word":"code"}}            <- {"ok":true}
```
`literalIsWord` dit si le préfixe tapé est déjà un mot réel — l'engine ne
remplace alors le mot que sur sélection explicite (jamais d'autocorrection d'un
mot valide). Socket par défaut `/tmp/ime-predictord.sock` (`IME_PREDICTORD_SOCK`).

## Comportement (v2, type Gboard/Windows)

- **Repli accent-insensible** : `francais`→français, `etre`→être, `developpement`→développement.
- **Autocorrection floue** (edit-distance 1, adjacence clavier AZERTY) quand le
  préfixe exact ne donne rien : `bonjuor`→bonjour, `qaund`→quand, `bonjpur`→bonjour.
- **Complétion re-classée par le contexte** (P(mot|précédent)) : `je v…` remonte
  `vais`/`veux` au-dessus de `va`.
- **Mot-suivant** en trigramme + stupid-backoff sur probabilités → bigramme.
- Côté engine : capture tout caractère de mot Unicode (accents, MAJUSCULES,
  apostrophe/trait d'union/chiffres en milieu de mot), report de casse
  (`Bonjou`→Bonjour), suggestions **opt-in via Tab** (chiffres/flèches/Entrée/
  ponctuation passent normalement), rien dans les champs mot de passe.

## Build & tests

```sh
nix build ./ime#predictord       # daemon + bench
nix build ./ime#fcitx5-predict   # addon fcitx5 (libpredict.so)
# tests comportementaux du cerveau (repli accent, autocorrect, contexte, trigramme) :
python3 ime/daemon/test_predict.py "$(nix build ./ime#predictord --no-link --print-out-paths)/bin/predictord"
```

## Modèle FR+EN (réel)

`nix build ./ime#model` produit trois artefacts (épinglés par hash → build pur) :

- **`words.tsv` (~84 000 mots)** — fusion des listes de fréquence OpenSubtitles
  2018 (`hermitdave/FrequencyWords` fr_50k + en_50k). Complétion classée par
  fréquence, re-pondérée par le contexte. `auj`→aujourd'hui, `dévelo`→développement.
- **`bigrams.tsv` (~155 000)** + **`trigrams.tsv` (~207 000)** — construits depuis
  les corpus Leipzig news 2024 (FR+EN 100k phrases) via `daemon/build_ngrams.py`.
  Le daemon charge les deux automatiquement (siblings de words.tsv).

## Résultats mesurés (i5-1335U, CPU-only)

- **Prédiction n-gram** (bench synthétique, vocab 50k, 2M trigrammes) :
  p99 **0,86 µs** → ~35000× sous le budget 30 ms/frappe.
- **Round-trip socket, modèle réel 84k** (IPC + JSON) : **~24 µs/requête**
  → le découplage daemon (azookeyd) ne coûte rien.

## État

- [x] Track B — cerveau n-gram, latence prouvée
- [x] Câblage — daemon socket Unix + JSON, FR+EN (préfixe + bigramme)
- [x] Track A — engine fcitx5 build (libpredict.so + confs)
- [x] Flake : 2 paquets + module NixOS (`nixosModules.default`)
- [x] **Test live VALIDÉ** (2026-06-09, Hyprland 0.55.2) : addon chargé dans
      fcitx5, `cont` tapé dans KWrite (Qt) → candidat **`content`** du daemon
      committé. Pipeline complet OK. Lancer avec `./test-live.sh`, activer avec
      Ctrl+Espace. (Pièges réglés: dépendance addon erronée, cache fcitx5 périmé.)
- [x] **Vrai modèle FR+EN** (84k mots, fréquences réelles, build pur via hash) —
      e2e prouvé : `inte` dans KWrite → **`interesting`** committé.
- [x] **Mot-suivant (bigrammes)** — corpus Leipzig FR+EN → `bigrams.tsv`, engine
      track le mot précédent. Vérifié in-app : `je ` → barre `ne · suis · me ·
      pense · vais · veux` dans KWrite (capture `nextword.png`).
- [x] **Apprentissage utilisateur** — le daemon journalise `prev⇥mot`
      (`$XDG_DATA_HOME/ime-predictord/user.log`, rejoué au démarrage) et fait
      passer tes mots/bigrammes AVANT le modèle de base. Vérifié : après avoir
      appris, `nix`→nixos en tête, `je`→code en tête ; e2e KWrite → log écrit.
- [x] **Refonte robustesse v2** — corrige le « pas robuste vs Gboard/Windows ».
      Côté **cerveau** : repli accent-insensible, autocorrection floue (adjacence
      AZERTY), complétion re-classée par interpolation `λ·P(ctx)+(1-λ)·P(préfixe)`,
      mot-suivant trigramme + stupid-backoff sur probas, `literalIsWord`. Côté
      **engine** : capture Unicode complète (accents/MAJ/apostrophe/chiffres),
      suggestions opt-in (Tab) qui ne mangent plus chiffres/flèches/Entrée/
      ponctuation, espace qui complète/auto-accentue/corrige sans écraser un vrai
      mot, report de casse, amorçage surrounding-text, bypass mot de passe, et
      jamais de perte du mot en cours au changement de focus.
- [x] **Fix SIGPIPE (crash daemon)** — l'engine envoie `learn` en
      fire-and-forget ; sans `signal(SIGPIPE, SIG_IGN)` le daemon mourait au
      write sur socket fermé → prédictions mortes après quelques mots (gros
      contributeur du « pas robuste »). Trouvé par le test e2e. Engine passe en
      `send(MSG_NOSIGNAL)`. Régression couverte (`test_predict.py`).
- [x] **Contractions FR protégées** — les listes OpenSubtitles découpent les
      contractions à l'apostrophe (`j'` existe, pas `j'ai`), donc le fuzzy
      transformait `j'ai`→`jail`. Corrigé : l'Espace n'auto-applique qu'une vraie
      complétion de préfixe (ou une faute simple SANS apostrophe) ; sinon il garde
      le littéral. Le fuzzy ne touche plus aux `'`/`-`. (`j'ai`→j'ai, `c'est`→c'est.)
- [x] **19 tests cerveau** (`daemon/test_predict.py`) + **test e2e assertif**
      (`./test-e2e.sh`, AZERTY) verts : sway headless + fcitx5 (bus D-Bus privé) +
      zenity (text-input-v3) + injection `wtype`, SANS toucher la session. Couvre
      accents (café/élève/ça va), contractions (j'ai/c'est), complétion (bonjou→
      bonjour), autocorrect (teh→the), no-clobber (le), chiffres (code 3),
      casse (Bonjou→Bonjour), phrase multi-mots, et survie du daemon (SIGPIPE).
- [x] **UI QML maison (`ui/`)** — barre de candidats en **Qt Quick** rendue à
      la popup-surface input-method positionnée au caret par le compositeur
      (impossible avec un addon externe vanilla → patch fcitx5 `INSTALL` +
      `getInputMethodV2Raw`, cf `ui/waylandim-public.patch`). Rendu **software**
      offscreen (`QQuickView::grabWindow` → `QImage` → buffer `wl_shm`), sans
      event-loop Qt (monothread, comme le module wayland). Design : **chips
      horizontales** (façon Gboard), **pill accent matugen** sur le candidat
      surligné, police Maple Mono NF. Couleurs lues de
      `~/.cache/DankMaterialShell/dms-colors.json` (live-reload au changement de
      thème), override possible via `~/.config/fcitx5/qmlpanel/colors.json`.
      Validé visuellement en headless (sway + grim) sur les 3 phases.
- [ ] Rerank neuronal async optionnel (CTranslate2), non bloquant.
- [ ] (Polish) auto-accent quand la forme sans accent est au dico (ex. `garcon`
      reste `garcon` car présent dans le corpus) — limite de qualité du corpus.

## UI QML — barre de candidats (`ui/`)

```sh
nix build ./ime#fcitx5-patched   # fcitx5 + API waylandim publique (patch 4 lignes)
nix build ./ime#qmlpanel         # l'addon UI Qt Quick (libqmlpanel)
```

Activer : lancer fcitx5 patché avec `--ui qmlpanel` (sinon classicui reste l'UI).
Couleurs : automatiques depuis DMS/matugen (`dms-colors.json`, clés `primary`/
`on_primary`/`surface_container*`/`on_surface`), ou forcées via
`~/.config/fcitx5/qmlpanel/colors.json` (`{surface,onSurface,accent,onAccent}`).

Test visuel headless (screenshot grim, sans toucher la session) : `/tmp/ime-ui1.sh`.

## Activer (quand on passera au test live)

Dans la config système : importer `nixosModules.default` du flake + s'assurer
de `i18n.inputMethod.type = "fcitx5"`. Le module ajoute l'addon à
`i18n.inputMethod.fcitx5.addons` et lance `predictord` en service utilisateur.
