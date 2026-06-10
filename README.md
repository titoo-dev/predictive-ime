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
- **Garde-fous d'auto-application** (l'Espace ne remplace que sûr de lui ; les
  candidats restent toujours affichés) : préfixe ≥ 3 lettres (`az` ne devient
  pas « aziz »), le top doit dominer le 2ᵉ ×2 (ambigu → littéral, Tab choisit),
  une faute floue ne raccourcit jamais la frappe (`pcq` ↛ « pc »). Un **lexique
  d'abréviations** FR/EN (pcq, bcp, tkt, mdr, btw, imo…) est au vocabulaire →
  jamais « corrigées ».
- **Amorce de phrase** : bigrammes `<s>` comptés au build → suggestions en
  début de champ et après `. ! ?` (le contexte ne traverse jamais une frontière
  de phrase, surrounding-text compris).
- **Apprentissage utilisateur** persistant
  (`$XDG_DATA_HOME/ime-predictord/user.log`, rejoué au démarrage). Un mot
  hors-vocabulaire doit être committé **≥ 2 fois** avant de passer devant le
  modèle (un fragment isolé ne pollue plus), et les compteurs **vieillissent**
  (×¾ tous les 512 commits, journal compacté) — cache-LM : le récent pèse
  plus, l'ancien s'estompe. Hygiène :
  `echo '{"forget":{"word":"x"}}' | nc -U /tmp/ime-predictord.sock` ;
  introspection : `echo '{"stats":true}' | nc -U …`.
- **Veto persistant** : chaque revert (Backspace) journalise la paire
  tapé→appliqué — elle ne sera plus jamais auto-appliquée (`veto.log`).
- **Langue des suggestions** : choisie par l'utilisateur (`lang` : `fr` /
  `en` — déterministe, `off` — aucun boost) via les préférences ; en mode
  `auto` seulement, le contexte (4 mots) vote FR/EN (3ᵉ colonne de
  words.tsv). Les candidats de la langue active remontent (`langBoost`).
  Mesuré (cf benchmark) : choisir sa langue ne coûte rien en qualité, même à
  contre-emploi — c'est de la prévisibilité gratuite.
- **Multi-mots** : si la continuation du meilleur candidat est très sûre
  (P ≥ 0,35), l'expression entière est proposée (« sais pas »).
- **Apostrophe typographique** `’` normalisée en `'` partout (repli + n-grammes).

## Personnalisation (`~/.config/ime-predictord/`, rechargé à chaud)

- **`ime-preferences`** — popover de réglages (paquet du flake, dans
  `environment.systemPackages` via le module ; **SUPER+ALT+I** sous
  Hyprland, règle float dédiée). Édite `config.json` à travers le lien
  stow (le fichier des dotfiles est mis à jour), chaque bascule est
  appliquée à la volée. Section « Langue des suggestions » : Français /
  English / Auto / Aucune. CLI scriptable :
  `ime-preferences --set lang=fr --set ghostText=false`.
- **`config.json`** — daemon : `lang` (`fr`/`en`/`auto`/`off`), `autoApply`,
  `autoDom`, `autoMinLen`,
  `langBoost`, `multiWord` ; engine : `ghostText`, `frenchSpacing` (espace
  fine insécable U+202F avant `; : ! ?`), `autoCapitalize` (majuscule en
  début de phrase), `nextWordBar` (false = pas de barre spéculative après
  Espace — mode calme), `autoApplyNeedsRevert` (défaut true : l'Espace ne
  remplace que si l'app permet le revert Backspace), `escapeForward` (défaut
  true : Échap atteint l'application après avoir fermé la barre). Tout
  changement est pris en compte sans redémarrage.
- **`snippets.tsv`** — `déclencheur<TAB>expansion` : `;mail ` → ton adresse,
  `;shrug ` → ¯\\_(ツ)_/¯. Le déclencheur exact s'auto-applique sur Espace,
  un préfixe l'affiche dans la barre.
- **`dict.txt`** — dictionnaire personnel déclaratif (un mot par ligne,
  fréquence optionnelle) : prénoms, jargon — complétables, jamais corrigés.
  Les trois fichiers sont stow-ables (`dotfiles/ime-predictord/`).

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
- **Grille 3×8** : le mode emoji affiche jusqu'à 24 candidats en grille —
  Tab/⇧Tab et ←/→ se déplacent de case en case, **↑/↓ sautent d'une ligne**
  (wrap), 1-6 sélectionnent sur la première ligne, taper affine en live.
  `:` seul = tes favoris puis les populaires (recently-used, façon Win+.).
  Le browsing par catégorie est gratuit : `:animal`, `:fete`, `:main`…
- Espace committe l'emoji du haut, Entrée le surligné. En complétion normale,
  un mot qui est exactement un mot-clé (« coeur ») propose l'emoji en fin de
  barre.

## Interactions

- **Navigation** : Tab entre dans la barre par la **gauche**, ⇧Tab par la
  **droite** ; en navigation, ←/→ se déplacent et **1-6** sélectionnent
  directement. Espace/Entrée valident le surligné. ↑/↓ ne sont **pas**
  capturés (ils déplacent le curseur dans un éditeur multi-lignes) — sauf en
  **grille emoji** (`:`), où ils sautent d'une ligne (±8, wrap) — et les
  raccourcis à modificateur (Ctrl+Tab, Ctrl+Entrée…) committent le littéral
  puis **passent** à l'application. Un appui de modificateur seul (Shift…)
  ne touche à rien — ni au mot en cours, ni à la barre, ni au revert.
- **Ghost text** : quand l'Espace va compléter, le reste du mot s'affiche déjà
  dans le préedit, curseur entre le tapé et le fantôme (`bonjou‸r`) — jamais
  pour une correction floue (la barre + liseré s'en chargent).
- **Espace** : complète/corrige (garde-fous ci-dessus) — le candidat qui sera
  appliqué porte un **liseré accent** dans la barre ; sans marquage, Espace
  garde le littéral. La **ponctuation** (`. , ; : ! ?`) corrige aussi
  (« teh. » → « the. »). L'auto-application exige que l'app expose le
  *surrounding text* (sinon le revert Backspace serait impossible — les
  candidats restent, Tab choisit) ; opt-out : `autoApplyNeedsRevert: false`.
- **Backspace juste après une auto-application** : REVERT — le mot remplacé
  est effacé, le littéral tapé revient en composition, et l'Espace suivant le
  respecte (pas de re-correction). La fenêtre survit aux modificateurs.
- **Ctrl+Backspace en composition** : ABANDONNE le mot en cours — rien n'est
  committé, rien n'est appris.
- **Échap** : ANNULE la suggestion (en composition le littéral est committé
  tel quel, rien n'est appris — annuler ≠ valider) ou ferme la barre
  mot-suivant, puis la touche **file à l'application** (vim sort du mode
  insertion au premier Échap). `escapeForward: false` pour l'avaler.

## Résultats mesurés (i5-1335U, CPU-only)

Harness `eval_model.py`, held-out **Leipzig news 2023** (année ≠ training,
400 phrases/langue, via le vrai protocole socket) :

| métrique (TOTAL fr+en)        | base (backoff, 100K) | KN (100K) | **v5 (KN+corpus+langue)** |
|-------------------------------|----------------------|-----------|---------------------------|
| mot-suivant hit@1             | 13,8 %               | 14,3 %    | **15,1 %**                |
| mot-suivant hit@3             | 21,5 %               | 22,8 %    | **24,0 %**                |
| mot-suivant hit@6             | 26,2 %               | 28,6 %    | **30,1 %** ²              |
| complétion auto-KSR (Espace)  | 17,2 %               | 31,7 %    | **21,9 %** ¹              |
| complétion top3@2             | 18,3 %               | 29,1 %    | **50,3 %**                |
| autocorrection top-1          | 83,5 %               | 89,5 %    | **91,4 %**                |
| latence socket p50            | 80 µs                | 90 µs     | ~210 µs                   |

¹ l'auto-KSR descend de 34,7 % (sans garde-fous) à ~22 % : choix assumé —
l'Espace ne remplace que quand le top domine ×2 (zéro « az »→aziz, « pcq »→pc),
le reste se prend au Tab (un mot sur deux est dans le top-3 dès 2 lettres) et
le revert Backspace couvre les erreurs résiduelles. Précision > agressivité.
² le slot de rang 6 est parfois occupé par l'expression multi-mots (« sais
pas ») — en rang 2 elle coûtait 0,7 pt de hit@3, en fin de barre ~0,1 pt de
hit@6 ; toute insertion déplace le top-k mesuré.

### Benchmark vs Gboard (2026-06-10, v6)

Mot-suivant, held-out Leipzig **news 2023** (année ≠ training, 400
phrases/langue, vrai protocole socket). Référence Gboard : les chiffres
**publiés par Google** (Hard et al. 2018, arXiv 1811.03604 — production
Gboard EN, vocab 10k) :

| modèle                              | hit@1      | hit@3      |
|-------------------------------------|------------|------------|
| Gboard 5-gram FST (historique)      | 13,0 %     | 22,1 %     |
| Gboard CIFG-LSTM (déployé, fédéré)  | 16,4 %     | 27,0 %     |
| **nous — TOTAL fr+en**              | **15,1 %** | **24,0 %** |
| **nous — in-vocab seulement**       | **16,9 %** | **27,0 %** |
| nous — FR seul                      | 13,7 %     | 23,4 %     |
| nous — EN seul                      | 16,3 %     | 24,7 %     |

En in-vocab (la comparaison la plus proche : Gboard mesure sur son vocab
10k, nous sur 84k) on est **au niveau du CIFG-LSTM déployé**, en lookups
n-gram CPU. Lecture honnête des limites : Gboard mesure sur ses **logs de
frappe mobile EN** (registre chat), nous sur de la **presse** — c'est
indicatif, pas du strict apples-to-apples ; notre hit@1 TOTAL (15,1 %) reste
sous leur CIFG (16,4 %) car ~11 % des tokens held-out sont hors-vocab ;
l'autocorrection (91,4 % top-1) et l'auto-KSR (21,9 %) n'ont pas
d'équivalent publié comparable côté Gboard. Latence par requête : p50
**40-67 µs**, p99 1,5-2,5 ms (socket compris) — l'inférence CIFG on-device
de Gboard est de l'ordre de la dizaine de ms.

**Langue choisie vs auto-détection** (la détection est désactivable —
préférences) : forcer la langue ne coûte **rien** sur sa propre langue
(FR forcé sur corpus FR : identique à l'auto à ±0,1 pt) et presque rien à
contre-emploi (FR forcé sur corpus EN : hit@3 −0,3 pt, auto-KSR −0,5 pt,
top3@2 −0,7 pt) — les n-grammes du contexte dominent largement le boost
×1,6. Choisir sa langue est donc un choix de **prévisibilité**, pas un
sacrifice de qualité.

```sh
# reproduire (--config injecte le réglage dans le daemon isolé) :
python3 ime/daemon/eval_model.py <predictord> <words.tsv> \
  fra-sentences.txt eng-sentences.txt --config '{"lang":"fr"}'
```

Bench brut n-gram : p99 **0,86 µs** ; round-trip socket réel ~24 µs.

**v6 (latence du chemin chaud)** : le mot-suivant recopiait les listes de
suiveurs en hash map à chaque requête — 1,3-4 ms round-trip mesurés sur les
contextes fréquents (« de », début de phrase), payés en synchrone par
l'engine à chaque frappe. Le `CtxScorer` pointe maintenant les listes triées
(dichotomie, zéro copie) : round-trip mesuré p50 **0,05-0,4 ms** sur les
mêmes cas (×25-40). Le daemon sert plusieurs clients en `poll()` (un client
resté ouvert — `nc -U` interactif — ne bloque plus personne) et l'engine
borne toutes ses E/S socket à 150 ms : le clavier ne peut plus geler.

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
- [x] **v6 — fluidité & robustesse** (audit 2026-06-10) : garde `isModifier`
      (un Shift seul ne committe plus le mot en plein milieu, ne fait plus
      clignoter la barre à chaque majuscule et ne désarme plus le revert),
      timeouts socket engine (150 ms — le clavier ne gèle jamais derrière le
      daemon), serveur `poll()` multi-clients, mot-suivant sans copie
      (×25-40 en latence), fondu de fermeture borné (perte de focus = unmap
      immédiat, garde-fou minuté contre les fondus orphelins → plus de barre
      fantôme au refocus), ré-apparition sans blink (reprise à l'opacité
      courante), emojis keycap/©️/‼️ rendus en fonte couleur (détection par
      contenu, plus par 1ᵉʳ point de code), Ctrl+Backspace abandonne le mot,
      Échap traverse vers l'app (`escapeForward`), ↑/↓ et raccourcis à
      modificateur libérés, auto-application seulement si le revert est
      possible (`autoApplyNeedsRevert`), barre mot-suivant désactivable
      (`nextWordBar`). e2e : +3 cas (Shift en plein mot, revert après Shift,
      Échap traversant).
- [x] **Langue choisie + préférences + benchmark** (2026-06-10) :
      l'auto-détection de langue est désactivée au profit d'une langue
      CHOISIE (`lang: "fr"` dans les dotfiles ; `auto` reste disponible),
      popover `ime-preferences` (Qt Quick, couleurs DMS, SUPER+ALT+I,
      écrit la config à chaud, CLI `--set`), benchmark held-out 2023 vs
      chiffres publiés Gboard (cf section) : in-vocab **16,9/27,0 %** =
      niveau CIFG-LSTM déployé ; forcer la langue ≈ gratuit (−0,3 pt hit@3
      au pire, à contre-emploi).
- [ ] Rerank neuronal async optionnel (GRU/GPT-2-mini int8 via ONNX, patron
      SwiftKey 2025 : reranke les candidats n-gram, ~+3-5 pts hit@3), non
      bloquant. (Toujours pas prioritaire : à reconsidérer seulement si le
      ressenti de frappe est bon et que la qualité des candidats plafonne.)
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

**Animations** : l'état (apparition, position du pill, fondu de fermeture)
est avancé côté C++ (steady_clock + ease-out cubic), exposé au QML en context
properties ; tant qu'une animation court, `qmlui.cpp` redemande une frame au
compositeur (`wl_surface_frame`) et re-rend — le contrat Wayland exact, zéro
timer. Apparition 140 ms, pill 110 ms, **fade-out 90 ms** avant le démappage.
La surface/popup **persistent** entre les mots (unmap par buffer NULL) — zéro
churn pendant la frappe. Un **indicateur de mode** discret (barrette accent =
composition, neutre = suggestion passive) ouvre la barre à gauche.
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

**Une seule source de lancement de fcitx5.** Le paquet fcitx5 installe un
autostart XDG (`/etc/xdg/autostart/org.fcitx.Fcitx5.desktop`, sans
`--ui qmlpanel`) qui court contre le `fcitx5 -d --ui qmlpanel` de
`hyprland.lua` — le perdant de la course dbus variait par session (sessions
sur classicui au lieu de la barre QML). L'entrée utilisateur
`dotfiles/autostart/org.fcitx.Fcitx5.desktop` (`Hidden=true`, stowée) masque
l'autostart système : seul le lancement Hyprland reste.
