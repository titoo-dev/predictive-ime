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
    build_emoji.py       #   CLDR fr+en + emoji-test.txt → emoji.tsv (picker Super+;)
    eval_model.py        #   harness d'évaluation (hit@k, auto-KSR, autocorrection, latence)
    test_predict.py      #   tests comportementaux du daemon (27 cas)
    bench_ngram.cpp      #   "spike" latence (n-gram synthétique à l'échelle réelle)
    CMakeLists.txt
  ui/                    # barre de candidats Qt Quick (cf section UI QML)
  test-e2e.sh            # e2e assertif headless (sway + fcitx5 + wtype)
  test-hotkey.sh         # e2e : picker sans activer l'IME, et méthode rendue après
  test-ui.sh             # captures visuelles headless (grim) + assertions d'animation
```

## Protocole socket (engine ↔ daemon)

```
-> {"context":["je"],"prefix":"v","wide":"Il fait beau. Je"}
<- {"candidates":["veux","vais",...],"literalIsWord":false,
    "autocomplete":"veux","ghost":"veux","accentOnly":false}
-> {"learn":{"prev":"je","word":"code"}}            <- {"ok":true}

# deux phases (mot-suivant neural, E5) :
-> {"context":["je"],"prefix":"","wide":"...","async":true}
<- {"candidates":[n-gram...],"pending":true}        # immédiat
<- {"candidates":[fusion neural...],"refresh":true} # dès que le neural aboutit
```
`literalIsWord` dit si le préfixe tapé est déjà un mot réel — l'engine ne
remplace alors le mot que sur sélection explicite (jamais d'autocorrection d'un
mot valide). `autocomplete` est le mot que l'Espace applique (haute confiance).
`ghost` est la complétion haute-confiance TOUJOURS calculée (mêmes garde-fous),
même quand `autoApply` est off : l'engine l'affiche en texte fantôme et **→ la
committe explicitement** (sans espace). `accentOnly` signale que `autocomplete`
est une pure RESTAURATION D'ACCENTS (fold-equal : francais→français,
oeuvre→œuvre, c'etait→c'était) — l'engine l'applique alors même si le tapé est
un vrai mot du corpus (`literalIsWord`), car elle ne change jamais le mot.
`wide` est le TEXTE BRUT avant le curseur (~240 caractères, phrases précédentes
et ponctuation comprises, via SurroundingText) : seul le neural le lit — son
avantage mesuré exige le contexte long ; le n-gram garde `context` (borné à la
phrase). `async` demande les deux phases : n-gram tout de suite
(`pending:true`), puis une 2e ligne `refresh:true` sur la même connexion
(l'engine la lit depuis la boucle d'événements fcitx — zéro blocage clavier, un
refresh périmé — frappe/commit entre-temps — est jeté).
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
  par type de faute (transposition 0.12 > apostrophe oubliée 0.11 > voisin
  AZERTY 0.10 > lettre oubliée 0.09 > lettre en trop 0.07 — 0.02 seulement en
  TÊTE de mot : « dici » est une élision, pas un « d » parasite devant
  « ici »), jamais à travers apostrophe/trait d'union (`j'ai` intouchable).
  **Espace oublié** : un préfixe qui se coupe en deux vrais mots au bigramme
  observé propose l'expression (« dela » → « de la », 0.10) — affichée
  seulement, jamais auto-appliquée. **Apostrophe oubliée** : les élisions sont
  indexées par repli SANS apostrophe (« jai » → j'ai, « dici » → d'ici,
  « cetait » → c'était ; correspondance mot-entier = boost 0.5) et SYNTHÉTISÉES
  au besoin (proclitique j/c/d/l/m/n/s/t/qu + ' + mot à initiale vocalique :
  « temener » → t'emmener, absent du vocab) — composable avec les autres
  canaux (élision + lettre oubliée), synthèse coupée quand le tapé a des
  correspondances exactes (« les » ne fait pas surgir « l'esprit »). Mesuré
  (held-out 2023, fautes synthétiques 4 types) : récupération top-1
  75→88,5 %, top-3 80→95,2 %.
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
- **Cache de récence** (façon cache-LM Gboard) : les mots déjà présents dans
  le texte avant le curseur (`wide`, ~240 car. via SurroundingText) sont
  boostés (`recencyBoost`, ≤1.0 = off) dans le même pipeline multiplicatif
  que langue/accord — le texte humain se répète (noms propres, vocabulaire
  du sujet). Le dernier mot du contexte est exclu (pas de « le le »).
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
  `autoDom`, `autoMinLen`, `accentRestore` (restauration d'accents/ligatures
  fold-equal sur Espace, même avec `autoApply:false`), `accentDom` (seuil de
  dominance quand la graphie brute est aussi au corpus ; défaut 4.0),
  `barWords` (taille max de la barre, 1-8),
  `langBoost`, `recencyBoost` (boost des mots déjà dans le document ;
  ≤1.0 = off), `multiWord` ; engine : `ghostText`, `frenchSpacing` (espace
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
- **n-grammes** : Leipzig **news 2024 1M** phrases/langue (CC BY) + **Tatoeba
  conversationnel** (release OPUS datée v2023-04-12, immuable ; CC BY 2.0 FR) —
  le registre dialogue est bien plus proche de la frappe réelle que la presse.
  → ~950k bigrammes + ~1,2M trigrammes stockés (seuil ≥2), modèle 66 Mo,
  daemon ~100 Mo RSS.
- **emoji.tsv (~26k clés, ~1900 emojis)** — annotations CLDR 48.2.0 fr+en
  filtrées par `emoji-test.txt` (liste autoritaire Unicode, formes
  fully-qualified pour le rendu couleur) + prior de popularité Unicode.

## Emoji picker (raccourci `Super+;`)

- `Super+;` ouvre le picker **à tout moment** : buffer vide, en plein mot (le
  mot en cours est committé tel quel, sans apprentissage) ou barre
  mot-suivant ouverte. Re-presser referme. `:` **tapé** n'est plus un
  déclencheur : c'est un caractère normal partout (`10:30`, « bonjour :
  voici »). En interne le buffer reste `:requête` (l'UI déduit le mode grille
  de ce préfixe) — seule l'entrée dans le mode a changé.
- **Même quand l'IME prédictif n'est PAS la méthode active** : fcitx n'envoie
  les touches qu'à la méthode COURANTE, donc le raccourci serait mort tant
  qu'on n'a pas fait Ctrl+Espace. L'addon écoute donc les touches en phase
  `PreInputMethod` (`InputContextKeyEvent`, avant toute méthode) : si le
  raccourci tombe alors qu'une autre méthode est active, il bascule sur
  `predict` (`setCurrentInputMethod(ic, …, local=true)`) puis ouvre le picker.
- **C'est un EMPRUNT, pas une bascule** : la méthode d'origine est mémorisée
  (`imBeforePicker`) et RENDUE dès que le picker se referme — emoji committé,
  Échap, re-appui sur le raccourci, perte de focus (`releaseBorrowedIm`, posté
  au dispatcher : basculer d'IME en pleine main de touche rappellerait
  `reset()`). Sans ça, choisir un emoji allumait la prédiction de texte pour
  toute la frappe suivante. Couvert par `./test-hotkey.sh` (profil à deux
  méthodes, attendu « ok❤️ bonjou » : le mot d'après reste littéral).
  **Piège de test** : sous Hyprland, `wtype` téléverse son PROPRE keymap et le
  compositeur résout le keycode dans le sien — le raccourci injecté arrive
  sous une autre touche (ici `Super+Escape`, donc le menu d'alimentation DMS).
  L'injection ne vaut que sous sway ; sous Hyprland, tester à la main.
- `Super+;` puis `coeur`→❤️, `soleil`→☀️, `fete`→🎉 — recherche par mot-clé
  CLDR replié (accents/ligatures : `cœur` marche aussi), le nom canonique
  (tts) domine les mots-clés, les **favoris appris remontent** (`learn` à
  chaque emoji committé). Sans requête : tes favoris puis une sélection
  courante. **Tolérance aux fautes** : si le préfixe exact ne matche rien
  (requête ≥3), transpositions et suppressions d'un caractère sont réessayées
  à score pénalisé — `ceour`→❤️, `etoiel`→⭐, `thumsb`→👍.
- **Champ de recherche** : la requête n'entre PLUS dans l'application. Elle
  part dans le préedit du **panneau** (`inputPanel().setPreedit`), le préedit
  CLIENT reste vide — la barre QML en fait un champ M3 (loupe dessinée,
  placeholder, caret accent) et l'UI fcitx classique l'affiche au-dessus des
  candidats. C'est aussi le marqueur de mode grille pour l'UI (`qmlui.cpp`
  `emojiPicker()`), qui garde donc la barre affichée **même sans candidat**.
- **Grille 3×8** (Material 3) : jusqu'à 24 candidats, cases de 32 px (rayon
  10, sélection en `secondaryContainer`), largeur FIXE 8 colonnes — la grille
  ne saute plus à chaque frappe. Surface `surface_container_high`, champ de
  recherche `surface_container_highest`, textes secondaires
  `on_surface_variant` ; ligne d'aide clavier en bas.
- **Pagination** : le daemon rend jusqu'à 96 emojis (4 pages), la grille en
  montre 24 (`kGridPage`). `pageStart` est l'index absolu du premier emoji
  affiché ; les candidats envoyés à l'UI ne sont QUE la page courante, et tout
  accès passe par `candOf(state, indexLocal)` — sinon Entrée committerait
  l'emoji de la page 1. ↓ sur la dernière ligne et ↑ sur la première
  **tournent la page** en gardant la colonne ; ←/→ débordent aussi ;
  **PgDn/PgUp** sautent d'une page ; Début/Fin vont au tout début / à la toute
  fin. L'indicateur « 2/4 » part en `auxUp` et s'affiche à droite du champ de
  recherche. Toute frappe revient en page 1.
- **Navigation** : Tab/⇧Tab et **←/→ (entrée directe, sans Tab préalable)** de
  case en case, **↑/↓ d'une ligne** (±8), **Début/Fin** aux extrémités, 1-6
  sur la première ligne, taper affine en live. En grille les flèches sont
  **bornées** (`navigate(..., clamp=true)`) au sein d'une page : ← sur la 1re
  case ne renvoie plus à la dernière. Tab, lui, cycle toujours.
- **Aucun résultat** : le picker reste ouvert et affiche « Aucun emoji pour
  … » (fermer la barre à la première faute de frappe est brutal, la frappe
  suivante peut retomber sur des résultats). Le littéral `:zzz` n'est jamais
  proposé en candidat.
- Picker nu = **récents**, puis favoris, puis populaires (façon Win+.). La 1re
  rangée (`kRecentRow` = 8, les colonnes de la grille) liste les **derniers
  emojis choisis**, du plus récent au moins récent ; le reste suit l'usage
  (compteur appris), puis les populaires CLDR remplissent la grille. Borner les
  récents à une rangée est délibéré : en MRU pur, toute la grille se
  réorganisait à chaque insertion et la mémoire des positions sautait ; là,
  seule la 1re ligne bouge. Dans une **recherche**, la récence n'est qu'un
  départage (`kRecencyBonus`, ≤ 2 points contre 10 + compteur d'usage pour
  « déjà utilisé ») : l'habitude reste le signal principal.
  La récence ne coûte **aucun fichier** : `user.log` est rejoué dans l'ordre, donc
  le dernier événement d'un mot donne son rang (`userSeq`). `ageUser` recompacte
  le journal **en ordre de récence** — sinon le MRU repartait dans un ordre
  arbitraire après chaque compaction (tous les 512 commits).
  Le browsing par catégorie est gratuit : `animal`, `fete`, `main`…
- Espace committe l'emoji du haut (avec espace), **Entrée le surligné — ou le
  premier si on n'a pas encore navigué** (sans espace), y compris picker nu
  (le 1er favori). Vaut aussi pour les snippets (`;mail` + Entrée →
  expansion ; `;` nu reste littéral, c'est un caractère TAPÉ). En complétion
  normale, un mot qui est exactement un mot-clé (« coeur ») propose l'emoji en
  fin de barre.
- **Annuler ne colle rien** : Échap, une ponctuation, Super+; à nouveau — tout
  chemin « littéral » FERME le picker au lieu de committer `:requête` (le `:`
  vient du raccourci, pas de la frappe). Garde unique en tête de `commitWord`
  (`isEmojiBuffer && raw == buffer`), donc tous les appelants en héritent.

## Reformulation (Ctrl+Alt+R sur une sélection)

- **Flux** : sélection (souris ou Ctrl+A) → `Ctrl+Alt+R` → bulle verticale de
  variantes LLM (1-9/↑↓/Entrée remplace, Backspace juste après = revert,
  Échap annule). ←/→ **ou les lettres `r/f/s/c/t`** changent de MODE
  (`reformuler/formel/simple/court/corriger/traduire` — `c` = corriger, court
  reste aux flèches), re-Ctrl+Alt+R régénère (nonce). Le **dernier mode**
  utilisé est mémorisé pour le prochain Ctrl+Alt+R (session). Sans sélection
  ni champ court : panneau « Rien à reformuler » (feedback, pas de no-op).
  Sans sélection RAPPORTÉE mais champ court (≤400 cp) : le champ entier est
  reformulé et le commit **remplace le champ** (delete explicite — commitString
  seul insérerait en plus). `reformCount` (config engine, 1-6, défaut 3) fixe
  le nombre de variantes demandées. La langue est épinglée par **cfg.lang**
  (`fr`/`en` ; `auto` = heuristique `reformIsFrench`, partagée
  reform_prompts.h).
- **Source : Groq UNIQUEMENT** (`reformModel`, `reformBaseUrl`,
  `reformTimeoutMs` 8 s ; clé : `$GROQ_API_KEY` ou
  `~/.local/share/ime-predictord/groq.key` — jamais dans le dépôt stow).
  La qualité d'abord : le repli local (GGUF Base du mot-suivant) produisait
  des variantes inutilisables — retiré. **L'échec s'affiche** au lieu de
  disparaître : la réponse porte `error`
  (`no_key`/`auth`/`network`/`http`/`empty`) et l'engine montre un panneau
  compact — clé manquante/refusée → « Entrée : configurer » lance
  **`ime-preferences --groq-key`** (fenêtre où COLLER la clé fonctionne,
  écrite en 0600 dans le data dir, puis **validation automatique** via l'op
  daemon `{"reformCheck":true}` = appel Groq minimal → ✓ et fermeture auto,
  ou ✗ clé refusée). Réseau/API en panne → « ⚠ Reformulation indisponible ».
  `neural.reformulate` reste dans neural.cpp mais n'est plus branché.
- **Architecture (2026-07-04)** : l'endpoint est servi par un **worker
  dédié** du daemon — le poll loop ne gèle JAMAIS (la complétion continue
  pendant les secondes de génération ; mesuré au test : complétion en 1 ms
  pendant une reformulation lente). Réponse **différée** sur la même
  connexion (lignes déposées via `postLine` + pipe de réveil) ; un job du
  même client encore en file est remplacé (cycling de modes). **Streaming**
  local : chaque variante part en `partial:true` dès que sa ligne est
  générée — la 1re s'affiche pendant que les suivantes se calculent (la
  génération s'arrête d'ailleurs dès n variantes acceptées). **Cache LRU**
  (8 entrées, clé texte+mode+nonce+n) : revenir sur un mode déjà visité est
  instantané et gratuit ; les échecs ne sont jamais cachés.

## Interactions

- **Navigation** : Tab entre dans la barre par la **gauche**, ⇧Tab par la
  **droite** ; en navigation, ←/→ se déplacent et **1-6** sélectionnent
  directement. Espace/Entrée valident le surligné. ↑/↓ ne sont **pas**
  capturés (ils déplacent le curseur dans un éditeur multi-lignes) — sauf en
  **grille emoji** (`Super+;`), où ils sautent d'une ligne (±8, borné) — et les
  raccourcis à modificateur (Ctrl+Tab, Ctrl+Entrée…) committent le littéral
  puis **passent** à l'application. Un appui de modificateur seul (Shift…)
  ne touche à rien — ni au mot en cours, ni à la barre, ni au revert.
- **Ghost text** : le reste de la complétion haute-confiance s'affiche dans le
  préedit, curseur entre le tapé et le fantôme (`bonjou‸r`) — jamais pour une
  correction floue (la barre + liseré s'en chargent). **→ l'accepte
  explicitement** (commit sans espace), que `autoApply` soit actif ou non —
  le mode prudent (`autoApply:false`) garde ainsi ses complétions.
- **Espace** : complète/corrige (garde-fous ci-dessus) — le candidat qui sera
  appliqué porte un **liseré accent** dans la barre ; sans marquage, Espace
  garde le littéral. La **ponctuation** (`. , ; : ! ?`) corrige aussi
  (« teh. » → « the. »). L'auto-application exige que l'app expose le
  *surrounding text* (sinon le revert Backspace serait impossible — les
  candidats restent, Tab choisit) ; opt-out : `autoApplyNeedsRevert: false`.
- **Backspace juste après une auto-application** : REVERT — le mot remplacé
  est effacé, le littéral tapé revient en composition, et l'Espace suivant le
  respecte (pas de re-correction). La fenêtre survit aux modificateurs.
- **Effacer n'applique jamais** (`erasing`) : après un Backspace — dans la
  composition comme sur un mot recomposé — plus de **fantôme** ni
  d'**auto-application** tant qu'un caractère n'a pas été retapé. Sans ce frein,
  effacer ne servait à rien : la complétion du préfixe raccourci remettait
  aussitôt ce qu'on venait d'enlever (⌫ sur `bonjour` réaffichait `bonjour`,
  cursor entre `bonjou` et `r`), et l'Espace committait la complétion refusée —
  pire sur une recomposition, où effacer l'espace après `salut` proposait
  `salutation` que l'Espace appliquait. Les **candidats restent** (Tab/1-6
  choisissent encore) : c'est l'application AUTOMATIQUE qu'on retire, pas la
  suggestion. Même durée de vie que `vetoAuto` (le mot en cours).
  `ghostShown()` centralise les conditions d'affichage du fantôme, parce que la
  touche → s'en sert aussi : sinon → accepterait une complétion invisible.
- **Ctrl+Backspace en composition** : ABANDONNE le mot en cours — rien n'est
  committé, rien n'est appris.

### Éditer le texte déjà écrit : `canEditSurrounding` (crash client)

Tout ce qui touche au texte DÉJÀ dans l'application (revert, recomposition,
fine insécable, reformulation) passe par `canEditSurrounding()`, qui exige que
**ce client** ait publié un texte environnant depuis qu'il a le focus
(événement `InputContextSurroundingTextUpdated`, drapeau `sawSurrounding`
remis à zéro au changement de focus).

La capacité annoncée par fcitx ne suffit pas, et ça se paie cher : un terminal
GTK4 (**ghostty**) active text-input-v3 sans jamais appeler
`set_surrounding_text`, donc son tampon côté GTK reste `NULL` — mais le cache
de fcitx, lui, peut contenir le texte d'un AUTRE contexte. La suppression
passait alors la validation de fcitx
(`waylandimserverv2.cpp`, bornes vérifiées contre CE cache), arrivait chez un
client sans tampon, et `text_input_delete_surrounding_text` déréférençait
`NULL` dans `g_utf8_pointer_to_offset` : **SIGSEGV du client**. Symptôme vu :
ghostty disparaît au premier Backspace après avoir tapé un mot, IME actif.
Trace protocole du bug : `delete_surrounding_text(3, 0)` alors que le client
n'avait envoyé aucun `set_surrounding_text`.

`deleteSurroundingBefore` vérifie en plus ses bornes AVANT d'émettre (l'ordre
comptait : la vérification ne protégeait que la copie locale, la requête
partait quand même). GTK4 ne valide rien de son côté, d'où la prudence ici.
- **Échap** : ANNULE la suggestion (en composition le littéral est committé
  tel quel, rien n'est appris — annuler ≠ valider) ou ferme la barre
  mot-suivant, puis la touche **file à l'application** (vim sort du mode
  insertion au premier Échap). `escapeForward: false` pour l'avaler.
- **Ctrl+Shift+L** : PANNEAU DE LANGUE — chips compactes
  [Français|English|Auto|Libre], le choix courant surligné (liste
  `kLangChoices`, extensible : ajouter une langue = une ligne). ←/→/Tab (ou
  re-Ctrl+Shift+L) déplacent, **1-4** ou Entrée/Espace appliquent, Échap
  annule, toute autre touche sort du mode. L'application réécrit la valeur
  de `lang` dans le TEXTE de `config.json` (formatage et clés-commentaires
  préservés, écriture atomique via la cible réelle du lien stow, mtime
  poussé d'une seconde si le rename retombe dans la même seconde — le
  daemon recharge sur mtime à la granularité seconde), puis la barre
  repart dans la nouvelle langue (retour visuel).

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
./ime/test-hotkey.sh             # picker sans activer l'IME + méthode d'entrée rendue
./ime/test-ui.sh                 # captures visuelles + assertions d'animation
# rendu hors ligne de la barre (états QML → PNG, sans compositeur) : le résultat
# est le dossier de captures, à ouvrir pour relire le design.
nix build ./ime#checks.x86_64-linux.panel
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
- [x] **Emoji picker** — `Super+;` + mot-clé (CLDR fr+en, filtre
      emoji-test.txt, formes fully-qualified, prior de popularité, favoris
      appris), hint emoji en complétion normale. e2e : `Super+;` `coeur ` →
      `❤️ `.
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
- [x] **Qualité complétion + UI/UX (2026-07-03) — Q1→Q5.** (1) `accentRestore`
      + `accentDom` : restauration d'accents/ligatures FOLD-EQUAL sur Espace —
      n'ajoute que les signes, jamais un autre mot, à travers les élisions
      (c'etait→c'était), avec seuil de dominance quand la graphie brute est
      aussi au corpus (francais→français ENFIN, ligatures œ/æ sans seuil) —
      marche même avec `autoApply:false` (les clés de la config perso étaient
      jusqu'ici… non implémentées). Nouveau champ réponse `accentOnly` :
      l'engine applique malgré `literalIsWord`. (2) GHOST découplé de
      autoApply : le champ `ghost` est toujours calculé ; **→ l'accepte
      explicitement** (commit sans espace, façon Copilot/fish) — le mode
      prudent garde ses complétions. (3) `barWords` (1-8, défaut 6) : la barre
      ne montre que les N meilleurs. (4) UI : fondu de contenu 80 ms quand la
      barre PASSIVE se rafraîchit (swap asynchrone du neural — plus de
      « pop »). Les indices 1-6 sur les chips (essai) ont été retirés à
      l'usage — les chiffres sélectionnent toujours en navigation, sans
      marquage. (5) `ime-preferences` : bascule
      « Restauration d'accents ». Résout le TODO « auto-accent quand la forme
      sans accent est au dico ».
- [x] **Neural v2 (2026-07-03) — E1→E8.** (1) Contexte LARGE : l'engine envoie
      le texte brut avant le curseur (`wide`, ~240 car., phrases précédentes
      comprises) — le neural prédit sur SON régime gagnant, y compris en début
      de phrase. (2) Expansion multi-token bornée : les fragments BPE
      (« l » → « l'école », élisions françaises) sont complétés jusqu'à la
      frontière de mot (KV restauré ensuite). (3) Rerank neuronal de la
      complétion, opportuniste : cache KV chaud pour ce contexte exact →
      mélange log-linéaire `(1−λ)·log P_KN + λ·logprob` (λ=`rerankWeight`),
      jamais de prefill sur le chemin chaud, `autocomplete` reste 100 % n-gram.
      (4) FUSION par score : candidats neuronaux = probabilité softmax ×
      `neuralBoost`, passés par langFactor/agreeFactor et fusionnés avec
      modèle + appris (fini le préfixage brut qui court-circuitait langue
      stricte, accord et apprentissage). (5) Deux phases ASYNC : n-gram
      instantané + refresh neural poussé (worker thread daemon, event loop
      fcitx côté engine, générations anti-périmé) — plus de hitch après
      Espace. (6) `neuralBudgetMs` : budget temps dur de l'appel neuronal.
      (7) Canal de faute : lettre oubliée + espace oublié (cf plus haut).
      (8) GGUF *base* épinglé (nix-config vinland, fetchurl + hash).
      Config daemon : `neuralBudgetMs` 180, `neuralBoost` 2.0, `neuralRerank`
      true, `rerankWeight` 0.4, `asyncNeural` true ; engine :
      `asyncNextWord` true. Smoke live (base 1.7B) : « allé à » → l'école ;
      « film au » + `cin` → cinéma top-1 ; refresh async +280 ms.
- [x] **Couche NEURONALE (libllama, GGUF) — implémentée 2026-06-23.**
      `daemon/neural.{h,cpp}` (NeuralPredictor : cache KV incrémental + top-k
      tokens initiaux de mot), intégrée dans `predictord` derrière `WITH_NEURAL`
      + config `neural`/`neuralModel`/`neuralThreads`/`neuralTopk`/`neuralOnly`
      (rechargés à chaud). Le neural mène le MOT-SUIVANT (prefix vide), le n-gram
      garde la complétion intra-mot + `literalIsWord`/`autocomplete` ;
      `neuralOnly=true` = mot-suivant 100% neuronal. Paquet flake
      `predictord-neural` (wrappé `GGML_BACKEND_PATH`) ; `predictord` reste pur
      n-gram (service live inchangé). Validé : Qwen3-4B Q4, qualité FR+EN très
      supérieure, liste candidats ~120 ms incrémental ; `test_predict.py` 0
      régression. Spec + bench : `docs/superpowers/specs/2026-06-23-neural-llm-predictor-design.md`.
      RESTE (non fait) : timeout engine 150 ms → relever ou passer ASYNC
      (instant n-gram + refresh neuronal poussé) sinon le 4B (~120-200 ms) est
      coupé côté engine ; GGUF *base* (vs instruct) ; pin du modèle + bascule du
      module ; éval hit@k neural vs n-gram ; test live.
- [x] **Parité anglaise + bascule de langue (2026-07-04).** (1) CONTRACTIONS
      EN au modèle : `extract_elisions.py` généralisé — en plus des élisions
      FR, il extrait du corpus les contractions anglaises (`don't`, `i'm`,
      `it's`, `you're`… — 2,3 % des tokens conversationnels), fréquences
      redistribuées depuis les tokens-ancre scindés de en50k (`'s` 14M,
      `'t` 9,6M…), étiquetées `en`. Avant : hors-vocab → intapables ET chaque
      occurrence cassait ses n-grammes. Après : `don` → don't (complétion),
      `dont`/`im`/`cant` → don't/i'm/can't top-1 (canal apostrophe existant,
      zéro changement daemon), n-grammes traversants (« i don't » → you/have/
      know), 810 bigrammes + 2916 trigrammes via don't. Held-out Tatoeba
      conversationnel : hit@6 31,9→33,0, hit@3 25,2→25,9, in-vocab 93,4→94,8 %
      (métrique pourtant durcie : les contractions comptent désormais comme
      cibles) ; presse 2023 et FR strictement inchangés. (2) CASSE ANGLAISE :
      `applyCase` capitalise toujours « i » et « i'… » (I, I'm, I'll) — sûr
      sans condition de langue (aucun mot FR n'est `i` ni `i'…`). (3)
      **Ctrl+Shift+L** : bascule fr ↔ en à chaud (cf Interactions). Tests :
      +4 cas engine (I'm affiché/committé, bascule aller-retour).
- [x] **Corpus ×3 + cache de récence + autotune (2026-07-04).** (1) Leipzig
      news 2024 passe de 300K à **1M phrases/langue** (URL + hash, rien
      d'autre) : modèle 74→157 Mo, 2,1M bigrammes / 3,2M trigrammes, RSS
      daemon ~200 Mo, latence p50 0,08-0,12 ms (p99 < 3 ms). Held-out 2023 :
      hit@3 FR 24,5→**26,1** (+1,6), EN 25,8→**26,7** (+0,9), auto-KSR FR
      27,1→28,3, autocorrection +0,5-1 pt — le levier classique des n-grammes.
      (2) CACHE DE RÉCENCE (façon cache-LM Gboard) : les mots déjà présents
      dans `wide` (le document en cours) sont boostés (`recencyBoost`) dans le
      même pipeline multiplicatif que langue/accord, dernier mot du contexte
      exclu. Branché sur TOUS les chemins (complétion scoreOf, appris uni/bi/
      tri, suiveurs tri/bi/topUni, fusion neurale, chemins sync/async/non-
      neural). (3) AUTOTUNE : sweep langBoost {1.6, 2.0} × recencyBoost {1.0,
      1.3, 1.6, 2.2} sur les 3 held-out (16 évals). Verdict : langBoost 2.0 =
      bruit (±0,1) → 1.6 conservé ; recencyBoost ~neutre à 1.3, négatif > 2 —
      défaut **1.3**. LIMITE assumée : le held-out est fait de PHRASES
      INDÉPENDANTES mélangées — il ne peut pas mesurer la répétition de
      document (noms propres, vocabulaire du sujet), le vrai bénéfice du
      cache ; le défaut reste donc doux. Tests : +3 cas daemon (récence
      complétion/mot-suivant, piège : `forget` nécessaire — la section 7
      apprend « je→vais » et l'appris re-passe devant).
- [x] **Panneau de langue (2026-07-04).** Ctrl+Shift+L n'est plus une bascule
      aveugle : il ouvre un panneau compact dans la barre (chips
      [Français|English|Auto|Libre], choix courant surligné — mode chips
      standard de qmlui : candidats SANS label → pas de bulle liste).
      Navigation ←/→/Tab/re-Ctrl+Shift+L, application 1-4/Entrée/Espace,
      Échap annule. Liste `kLangChoices` extensible (une langue = une ligne,
      une fois le support modèle/daemon en place). `toggleLang()` scindé en
      `readLang()`/`writeLang(v)` (même écriture textuelle atomique). Tests :
      5 cas engine (chips, chiffre, fermeture, Échap, →+Entrée).
- [x] **Reformulation — batch architecture (2026-07-04).** (A1) L'endpoint
      `reformulate` passe sur un **worker dédié** avec réponse différée
      (postLine + pipe de réveil partagé) : le poll loop ne gèle plus pendant
      les secondes d'appel Groq / génération locale — testé : complétion en
      1 ms pendant une reformulation lente (avant : bloquée derrière, jusqu'à
      20 s). File FIFO, job du même client remplacé s'il attend encore.
      (A2) **Streaming** : `neural.reformulate` accepte un callback par
      variante (parsing ligne-à-ligne incrémental + arrêt dès n variantes) ;
      le daemon pousse des lignes `partial:true`, l'engine met la bulle à
      jour au fil de l'eau (1re variante navigable immédiatement, position
      de navigation préservée). Groq reste non-streamé (répond < 2 s).
      (A3) **Cache LRU** 8 entrées (texte+mode+nonce+n) partagé
      worker/thread principal : cycling de modes déjà visités instantané.
      + `reformTimeoutMs` 20 s → 8 s (l'engine abandonne à 12 s) ; includes
      threading hors du `#ifdef WITH_NEURAL` (le worker existe aussi en
      build pur n-gram). Tests : +5 cas daemon (mock HTTP OpenAI local —
      variantes différées, cache, nonce, non-blocage mesuré, aboutissement).
      Streaming local vérifié en smoke GGUF réel : partial à t+1,1 s, final
      à t+2,2 s. ATTENTION (préexistant, hors batch) : le repli local partage
      le modèle du mot-suivant — avec le GGUF *Base* déployé, les variantes
      locales sont inutilisables (le Base ne suit pas le chat template) ; le
      repli n'a de valeur qu'avec un GGUF instruct, sinon Groq est la seule
      vraie source.
- [x] **Reformulation — Groq-only + UX de panne (2026-07-04).** Le repli
      local est retiré (qualité d'abord : le GGUF Base rendait du charabia).
      `reformulateHttp` remonte un KIND (`ok/no_key/auth/network/http/empty`)
      inclus dans la réponse ; l'engine affiche un panneau COMPACT au lieu
      d'un échec silencieux : clé manquante/refusée → « Entrée : configurer »
      lance `ime-preferences --groq-key` (double fork), réseau/API →
      « ⚠ indisponible ». Le dialogue de clé : champ collable, écrit
      `groq.key` (0600, data dir), puis VALIDATION AUTOMATIQUE via le nouvel
      op `{"reformCheck":true}` (appel Groq minimal sur le worker, différé) —
      ✓ fermeture auto, ✗ message. `no_key` est détecté SANS réseau → le
      panneau apparaît quasi instantanément au hotkey. Au passage : le
      remplacement de jobs en file se fait par PHRASE (l'engine ouvre une
      connexion par demande — l'ancien critère « même client » ne matchait
      jamais), l'évincé reçoit `superseded` immédiatement ; `reformProvider`
      supprimé de la config. Tests : +3 cas daemon (401→auth, reformCheck
      refusée/acceptée) ; smoke offscreen du dialogue. Limite : les panneaux
      engine (rendu async) ne sont pas couverts par le harnais engine.
- [ ] (Polish) auto-accent quand la forme sans accent est au dico (ex. `garcon`
      reste `garcon` car présent dans le corpus) — limite de qualité du corpus.

## UI QML — barre de candidats

La barre de candidats Qt Quick (**Opale**, popup au caret, picker emoji,
mode liste) est un projet séparé — architecture, palette, animations et
évitement du texte documentés dans son propre repo :
<https://github.com/titoo-dev/opale> (`docs/internals.md`).

## Activer (quand on passera au test live)

Dans la config système : importer `nixosModules.default` du flake + s'assurer
de `i18n.inputMethod.type = "fcitx5"`. Le module ajoute l'addon à
`i18n.inputMethod.fcitx5.addons` et lance `predictord` en service utilisateur.
Pour la barre QML au caret, composer en plus le flake
[Opale](https://github.com/titoo-dev/opale) (overlay
fcitx5 patché + addon `opale`, puis `--ui opale`).

**Une seule source de lancement de fcitx5.** Le paquet fcitx5 installe un
autostart XDG (`/etc/xdg/autostart/org.fcitx.Fcitx5.desktop`, sans
`--ui qmlpanel`) qui court contre le `fcitx5 -d --ui qmlpanel` de
`hyprland.lua` — le perdant de la course dbus variait par session (sessions
sur classicui au lieu de la barre QML). L'entrée utilisateur
`dotfiles/autostart/org.fcitx.Fcitx5.desktop` (`Hidden=true`, stowée) masque
l'autostart système : seul le lancement Hyprland reste.
