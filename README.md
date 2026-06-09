# IME prédictif maison (fcitx5 engine + daemon Kneser-Ney)

Implémentation de l'**Option A** validée dans
[`../docs/wiki/custom-ime-research.md`](../docs/wiki/custom-ime-research.md) :
un engine fcitx5 fin (réutilise le frontend Wayland éprouvé sur Hyprland 0.55.2)
qui délègue la prédiction à un daemon externe via socket Unix + JSON (patron
`fcitx5-azookeyd`).

## Structure

```
ime/
  flake.nix              # build des paquets + modèle (corpus épinglés) + module NixOS
  engine/                # Track A — addon fcitx5 (C++20)
    predict.cpp          #   InputMethodEngineV2: buffer→preedit, query daemon, candidats
    predict-addon.conf   #   enregistrement addon  → share/fcitx5/addon/predict.conf
    predict.conf         #   méthode de saisie     → share/fcitx5/inputmethod/predict.conf
    CMakeLists.txt
  daemon/                # Track B — cerveau + câblage
    predictord.cpp       #   socket Unix + JSON ; modèle KN précalculé + emoji + apprentissage
    build_ngrams.py      #   corpus → modèle Kneser-Ney interpolé (offline, format TSV)
    build_emoji.py       #   CLDR fr+en + emoji-test.txt → emoji.tsv (picker ':')
    eval_model.py        #   harness d'évaluation (hit@k, auto-KSR, autocorrection, latence)
    test_predict.py      #   tests comportementaux du daemon (27 cas)
    bench_ngram.cpp      #   "spike" latence (n-gram synthétique à l'échelle réelle)
    CMakeLists.txt
  ui/                    # barre de candidats Qt Quick (cf section UI QML)
  test-e2e.sh            # e2e assertif headless (sway + fcitx5 + wtype)
  test-ui.sh             # captures visuelles headless (grim) + assertions d'animation
```

## Protocole socket (engine ↔ daemon)

```
-> {"context":["je"],"prefix":"v"}
<- {"candidates":["veux","vais",...],"literalIsWord":false,"autocomplete":"veux"}
-> {"learn":{"prev":"je","word":"code"}}            <- {"ok":true}
```
`literalIsWord` dit si le préfixe tapé est déjà un mot réel — l'engine ne
remplace alors le mot que sur sélection explicite (jamais d'autocorrection d'un
mot valide). `autocomplete` est le mot que l'Espace applique (haute confiance).
Socket par défaut `/tmp/ime-predictord.sock` (`IME_PREDICTORD_SOCK`).

## Le modèle (v3) — Kneser-Ney interpolé précalculé

Le cerveau ne fait que des **lookups** (latence µs) ; toutes les maths sont
offline dans `build_ngrams.py` :

```
P(w|u,v) = p3(uvw)          si stocké        p stockés = probabilités
         = γ3(uv)·P(w|v)    sinon            INTERPOLÉES finales (ARPA)
P(w|v)   = p2(vw)           si stocké
         = γ2(v)·P1(w)      sinon
P1(w)    = 0.7·Pcont(w) + 0.3·Pfreq(w)
```

- **Kneser-Ney interpolé** : décote D estimée sur le corpus (Good-Turing,
  `D = n1/(n1+2·n2)` par ordre), masse redistribuée exactement via `γ(ctx)`,
  ordre inférieur en **comptes de continuation** (« york » est fréquent mais
  presque toujours après « new » → sa probabilité de continuation est faible).
  Remplace le stupid-backoff (qui donnait P=1.0 à un trigramme vu 2 fois).
- **Complétion** scorée par `P_KN(w|ctx)` restreinte aux mots du préfixe
  (replié accent-insensible : `francais`→français) — plus d'interpolation
  ad-hoc λ. Sans contexte : fréquence brute (bon prior en début de phrase).
- **Autocorrection en noisy-channel** : `P(w|ctx)·P(frappe|w)`, canal pondéré
  par type de faute (transposition 0.12 > voisin AZERTY 0.10 > lettre en trop
  0.07), jamais à travers apostrophe/trait d'union (`j'ai` intouchable).
- **Apprentissage utilisateur** prioritaire et persistant
  (`$XDG_DATA_HOME/ime-predictord/user.log`, rejoué au démarrage).

### Datasets (épinglés par hash → build pur)

- **words.tsv (~84 000 mots)** — OpenSubtitles 2018 (hermitdave) fr_50k+en_50k.
- **n-grammes** : Leipzig **news 2024 300K** phrases/langue (CC BY) + **Tatoeba
  conversationnel** (release OPUS datée v2023-04-12, immuable ; CC BY 2.0 FR) —
  le registre dialogue est bien plus proche de la frappe réelle que la presse.
  → ~950k bigrammes + ~1,2M trigrammes stockés (seuil ≥2), modèle 66 Mo,
  daemon ~100 Mo RSS.
- **emoji.tsv (~26k clés, ~1900 emojis)** — annotations CLDR 48.2.0 fr+en
  filtrées par `emoji-test.txt` (liste autoritaire Unicode, formes
  fully-qualified pour le rendu couleur) + prior de popularité Unicode.

## Emoji picker (préfixe `:`)

- `:` sur buffer vide démarre le picker (jamais en milieu de mot : `10:30`
  et « bonjour : voici » tapent normalement ; `:` + Espace garde le littéral).
- `:coeur`→❤️, `:soleil`→☀️, `:fete`→🎉 — recherche par mot-clé CLDR replié
  (accents/ligatures : `:cœur` marche aussi), le nom canonique (tts) domine
  les mots-clés, les **favoris appris remontent** (`learn` à chaque emoji
  committé). `:` seul liste tes favoris puis une sélection courante.
- Espace committe l'emoji du haut, Tab navigue. En complétion normale, un mot
  qui est exactement un mot-clé (« coeur ») propose l'emoji en fin de barre.

## Échap — fermer/annuler

- **En composition** : Échap ferme la barre et ANNULE la suggestion — le
  littéral tapé est committé tel quel (on ne perd jamais la frappe) et le
  fragment n'est PAS appris (annuler ≠ valider).
- **Barre mot-suivant** : Échap la ferme, la touche est avalée (elle n'atteint
  pas l'application — un 2e Échap, barre fermée, passe normalement).

## Résultats mesurés (i5-1335U, CPU-only)

Harness `eval_model.py`, held-out **Leipzig news 2023** (année ≠ training,
400 phrases/langue, via le vrai protocole socket) :

| métrique (TOTAL fr+en)        | base (backoff, 100K) | KN (100K) | **KN + corpus riches** |
|-------------------------------|----------------------|-----------|------------------------|
| mot-suivant hit@1             | 13,8 %               | 14,3 %    | **15,1 %**             |
| mot-suivant hit@3             | 21,5 %               | 22,8 %    | **24,1 %**             |
| mot-suivant hit@6             | 26,2 %               | 28,6 %    | **30,4 %**             |
| complétion auto-KSR (Espace)  | 17,2 %               | 31,7 %    | **34,7 %**             |
| complétion top3@2             | 18,3 %               | 29,1 %    | **30,5 %**             |
| autocorrection top-1          | 83,5 %               | 89,5 %    | **91,6 %**             |
| latence socket p50            | 80 µs                | 90 µs     | 124 µs                 |

Repères production (litt.) : le 5-gram FST historique de Gboard mesurait
13,0/22,1 % (hit@1/3), son remplaçant neural CIFG-LSTM 16,4/27,0 %. En
in-vocab nous sommes à **16,9/27,0 %** — niveau neural, en lookups µs.
Bench brut n-gram : p99 **0,86 µs** ; round-trip socket réel ~24 µs.

```sh
# re-mesurer :
python3 ime/daemon/eval_model.py \
  "$(nix build ./ime#predictord --no-link --print-out-paths)/bin/predictord" \
  "$(nix build ./ime#model --no-link --print-out-paths)/words.tsv" \
  <phrases-held-out.txt>...
```

## Build & tests

```sh
nix build ./ime#predictord       # daemon + bench
nix build ./ime#fcitx5-predict   # addon fcitx5 (libpredict.so)
nix build ./ime#model            # modèle KN + emoji (corpus épinglés)
# tests comportementaux du cerveau (27 cas : accents, fautes, contexte, KN, emoji) :
python3 ime/daemon/test_predict.py "$(nix build ./ime#predictord --no-link --print-out-paths)/bin/predictord"
./ime/test-e2e.sh                # e2e assertif (sway headless, AZERTY, 15 cas)
./ime/test-ui.sh                 # captures visuelles + assertions d'animation
```

## État

- [x] Track B — cerveau n-gram, latence prouvée
- [x] Câblage — daemon socket Unix + JSON, FR+EN (préfixe + bigramme)
- [x] Track A — engine fcitx5 build (libpredict.so + confs)
- [x] Flake : paquets + module NixOS (`nixosModules.default`)
- [x] **Test live VALIDÉ** (2026-06-09, Hyprland 0.55.2) : pipeline complet OK
      (`./test-live.sh`, Ctrl+Espace pour activer).
- [x] **Vrai modèle FR+EN** (84k mots, fréquences réelles, build pur via hash).
- [x] **Mot-suivant** trigramme + **apprentissage utilisateur** persistant.
- [x] **Refonte robustesse v2** — repli accent-insensible, autocorrection
      AZERTY, `literalIsWord`, capture Unicode complète, suggestions opt-in
      (Tab), report de casse, bypass mot de passe, fix SIGPIPE, contractions
      FR protégées (`j'ai` ≠ jail).
- [x] **Modèle v3 Kneser-Ney** — KN interpolé précalculé (comptes de
      continuation, décotes Good-Turing, γ exacts), complétion par P_KN(w|ctx),
      autocorrection noisy-channel. **auto-KSR ×2** (17→35 %), autocorrection
      top-1 +8 pts, hit@3 mot-suivant +2,6 pts (cf tableau).
- [x] **Datasets enrichis** — Leipzig news 300K + Tatoeba conversationnel
      (OPUS, immuable), harness d'évaluation `eval_model.py` (ablation
      complète baseline → KN → corpus).
- [x] **Emoji picker** — `:mot-clé` (CLDR fr+en, filtre emoji-test.txt, formes
      fully-qualified, prior de popularité, favoris appris), hint emoji en
      complétion normale. e2e : `:coeur ` → `❤️ `.
- [x] **UI QML v2 — design + animations** : apparition fade+slide (140 ms),
      pill accent qui **glisse** entre candidats (110 ms, ease-out cubic),
      boucle de frames pilotée par `wl_callback` (frame callbacks Wayland),
      liseré outline, rendu emoji couleur. Validé headless (captures +
      assertions de frames intermédiaires, `./test-ui.sh`).
- [ ] Rerank neuronal async optionnel (GRU/GPT-2-mini int8 via ONNX, patron
      SwiftKey 2025 : reranke les candidats n-gram, ~+3-5 pts hit@3), non
      bloquant.
- [ ] (Polish) auto-accent quand la forme sans accent est au dico (ex. `garcon`
      reste `garcon` car présent dans le corpus) — limite de qualité du corpus.

## UI QML — barre de candidats (`ui/`)

```sh
nix build ./ime#fcitx5-patched   # fcitx5 + API waylandim publique (patch 4 lignes)
nix build ./ime#qmlpanel         # l'addon UI Qt Quick (libqmlpanel)
```

Architecture : popup-surface input-method (`zwp_input_popup_surface_v2`)
positionnée au caret par le compositeur (impossible avec un addon externe
vanilla → patch fcitx5 `INSTALL` + `getInputMethodV2Raw`,
cf `ui/waylandim-public.patch`). Rendu **software** offscreen
(`QQuickView::grabWindow` → `QImage` → buffer `wl_shm`), sans event-loop Qt
(monothread, comme le module wayland).

**Animations** : l'état (apparition, position du pill) est avancé côté C++
(steady_clock + ease-out cubic), exposé au QML en context properties ; tant
qu'une animation court, `qmlui.cpp` redemande une frame au compositeur
(`wl_surface_frame`) et re-rend — le contrat Wayland exact, zéro timer.
`QMLPANEL_ANIM_SCALE` étire les durées (debug/captures déterministes).

Design **compact** (barre 34 px, chips 26 px, police 14 px) : chips
horizontales denses (façon Gboard), **pill accent matugen** glissant sous le
candidat surligné, liseré `outline`, police Maple Mono NF, emojis en fonte
couleur (17 px). Couleurs lues de
`~/.cache/DankMaterialShell/dms-colors.json` (live-reload au changement de
thème), override via `~/.config/fcitx5/qmlpanel/colors.json`
(`{surface,onSurface,accent,onAccent,outline}`).

Activer : lancer fcitx5 patché avec `--ui qmlpanel` (sinon classicui reste
l'UI). Test visuel headless : `./test-ui.sh` (PNG dans `/tmp/ime-ui/`).

## Activer (quand on passera au test live)

Dans la config système : importer `nixosModules.default` du flake + s'assurer
de `i18n.inputMethod.type = "fcitx5"`. Le module ajoute l'addon à
`i18n.inputMethod.fcitx5.addons` et lance `predictord` en service utilisateur.
