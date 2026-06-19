# Contexte de phrase + accord grammatical (nombre & genre)

**Date** : 2026-06-19
**Statut** : validé, prêt pour plan d'implémentation

## Problème

L'IME prédictif perd le contexte de la phrase :

1. Le moteur n'envoie au daemon que ses propres mots committés (`state->ctx`), et
   ne se rabat sur le texte réel (`SurroundingText`) que si `state->ctx` est vide
   → contexte périmé/incomplet après le 1er mot committé.
2. Le daemon est un modèle **trigramme** : il n'utilise que les 2 derniers mots.
   L'accord grammatical se perd dès qu'un mot s'intercale entre le déterminant et
   le nom : `les petits chat` propose `chat` (singulier) au lieu de `chats`.
   (Vérifié : `{"context":["les","petits"],"prefix":"chat"}` → `chat` en tête.)
3. Conséquence : suggestions non accordées (pluriel/genre) et mot-suivant
   sous-optimal car le signal de tête de groupe nominal est hors de portée.

## Objectif

Exploiter **toute la phrase** pour : (1) un contexte fiable, (2) l'accord en
nombre **et** genre, (3) un meilleur mot-suivant — **en restant dans le moteur
n-gramme rapide et déterministe** (pas de rerank neuronal, latence par frappe
négligeable).

## Décisions de design (validées)

- Architecture : n-gramme + **règles d'accord** (pas de modèle neuronal).
- Portée de l'accord : **nombre + genre** (best-effort sur le genre).
- Source morphologique : **lexique externe Lefff** (form → genre/nombre/lemme),
  licence **LGPL-LR** (libre, redistribuable). Morphalou (CC-BY 4.0) reste un
  repli si Lefff pose souci.
- Boost, **jamais de filtre dur** : on réordonne les candidats, on n'en supprime
  aucun (un mot non accordé ou hors-lexique reste proposable).

## Architecture — 3 composants

### Composant 1 — Moteur (`engine/predict.cpp`) : contexte réel

`contextFor` est inversé : **`SurroundingText` devient la source primaire** (la
vraie phrase avant le curseur), `state->ctx` n'est plus que le repli quand
SurroundingText est indisponible/invalide (terminaux, certains Electron).

- Fenêtre de contexte envoyée au daemon élargie de 4 → **~8 mots** (`lastWords(cps, 8)`).
- Le champ JSON reste `{"context":[...],"prefix":"..."}` : le daemon utilise
  toujours les 2 derniers pour le n-gramme et la fenêtre complète pour l'accord.
- Dégradation propre : sans SurroundingText, on garde le comportement actuel
  (`state->ctx`, fenêtre courte) → accord limité mais rien de cassé.

**Interface inchangée** : aucun changement de protocole socket ; seule la
quantité/qualité du contexte change.

### Composant 2 — Build du modèle (`flake.nix`) : table morphologique

- Nouvelle entrée `fetchurl` : Lefff `.mlex` depuis GitLab INRIA
  `almanach/alexina/lefff` (révision/tag **épinglé**, hash fixe). À noter dans
  `NOTICE-DATASETS.md` (licence LGPL-LR).
- Format `.mlex` (à confirmer en inspectant le fichier au build) : lignes
  `forme<TAB>catégorie<TAB>lemme<TAB>msfeatures`, où `msfeatures` ∈ {`ms`,`mp`,
  `fs`,`fp`,…} pour noms/adjectifs (m/f = genre, s/p = nombre ; formes invariables
  ou verbales → pas de tag nombre/genre nominal).
- Étape de build : produire **`morph.tsv`** compact, **restreint au vocabulaire
  du modèle** (jointure avec les formes de `words.tsv`) :
  `forme<TAB>genre(m|f|-)<TAB>nombre(s|p|-)<TAB>lemme`.
  → table légère (qq dizaines de k lignes, pas les 605k de Lefff).
- Livrée dans `$out/` à côté de `words.tsv` (le daemon charge les voisins par
  `dir`, comme bigrams/trigrams/pcont/emoji).

### Composant 3 — Daemon (`daemon/predictord.cpp`) : couche d'accord

- **Chargement** : `loadMorph(dir)` → deux maps : `form → {genre, nombre, lemme}`
  et `lemme → [formes]` (pour retrouver la forme accordée d'un candidat).
- **Détection des contraintes d'accord** (`agreementOf(context)`) : scan **arrière**
  de la fenêtre de contexte (la grande, pas le trigramme) jusqu'au déterminant
  gouverneur ; renvoie `{nombre voulu, genre voulu}` ou « aucun » :
  - Pluriel : `les des mes tes ses nos vos leurs ces aux quelques plusieurs
    certains/certaines divers/diverses` + cardinaux ≥ 2 (`deux trois …`).
  - Singulier + genre : `le/un/ce/cet/mon/ton/son` (masc) ; `la/une/cette/ma/ta/sa`
    (fém) ; `l'` (genre indéterminé).
  - On s'arrête (aucun accord) si un **briseur de groupe nominal** intervient
    avant le déterminant : verbe (via Lefff catégorie `v`), ponctuation forte,
    conjonction (`et ou mais donc car`), ou simplement > N mots sans déterminant.
    → l'accord traverse les **adjectifs** intercalés mais pas une nouvelle
    proposition.
- **Application** (`applyAgreement`) sur les candidats de `completePrefix` ET
  `predictNext` :
  - Pour chaque candidat connu de `morph` comme nom/adjectif :
    `match nombre` et/ou `match genre` → **boost ×K** ; mismatch → **÷K**.
  - Genre best-effort : n'applique le malus de genre que si la forme du genre
    voulu **existe** pour ce lemme (évite de pénaliser un mot épicène/invariable).
  - Candidat hors-`morph` (nom propre, abréviation, emoji, snippet) → **neutre**.
  - Facteur K configurable (`agreeBoost`, défaut ~2.0) ; réglable à chaud via
    `config.json` comme les autres paramètres.

## Flux de données

```
frappe
  → engine: SurroundingText (≤8 mots avant curseur) → {context, prefix}
  → daemon:
       n-gramme P_KN(w | 2 derniers mots)           [inchangé]
       × langFactor (langue stricte)                 [existant]
       × agreementFactor (nombre/genre, fenêtre complète × Lefff)  [NOUVEAU]
  → candidats réordonnés
```

## Cas limites

- Mot candidat hors-Lefff → non affecté (neutre).
- Aucun déterminant gouverneur dans la fenêtre → aucun boost d'accord.
- Sans SurroundingText (terminal) → fenêtre courte via `state->ctx` ; accord
  dégradé mais fonctionnel sur l'adjacent.
- Conflit langue/accord : la langue stricte s'applique d'abord (score 0 = exclu) ;
  l'accord ne ressuscite jamais un mot exclu (multiplication).
- Genre indéterminable (`l'`, lemme sans forme féminine connue) → pas de malus de
  genre, seul le nombre s'applique.

## Tests

- `daemon/test_predict.py` : cas d'accord
  - `["les","petits"] "chat"` → `chats` devant `chat`
  - `["des"] "belle"` (adj) → `belles`
  - `["une"] "grand"` → `grande` devant `grand`
  - `["un"] "enfant"` → `enfant` (singulier) conservé
  - `["les"] ""` (mot-suivant) → noms pluriels remontés
  - candidat hors-lexique inchangé (régression neutre)
- Requêtes `nc -U /tmp/…sock` avant/après (méthode socket déjà utilisée).
- e2e `kwrite` + `wtype` sur quelques phrases (`les petits ` + complétion).
- Non-régression : langue stricte intacte, latence par frappe stable.

## Compromis acceptés

- Dépendance données Lefff (qq Mo, filtrée au vocab après build → table légère).
- +~1 s au démarrage du daemon (parse `morph.tsv` au boot) — acceptable.
- Genre best-effort : quelques cas ambigus non couverts (épicènes, exceptions).

## Déploiement

Comme les correctifs précédents : commit/push `titoo-dev/predictive-ime`, puis
`nix flake update predictive-ime` + `make rebuild` côté nix-config.
