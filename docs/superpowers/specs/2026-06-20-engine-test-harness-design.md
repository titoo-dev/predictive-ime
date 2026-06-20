# Harnais de test pour l'engine fcitx5 — Design

Date : 2026-06-20
Statut : approuvé (design), à planifier

## Contexte & motivation

Le daemon de prédiction a une suite comportementale solide (`daemon/test_predict.py`),
mais **l'engine fcitx5 (`engine/predict.cpp`, ~1000 lignes) n'a aucun test**. Toute
la machine à états clavier — composition, barre de complétion, barre mot-suivant,
casse, ponctuation, Backspace, déclencheurs emoji/snippet, auto-application,
fenêtre de revert, frenchSpacing, autoCapitalize, gestion des capacités du client
(preedit/surrounding/password) — n'est validée que par e2e manuel.

Cette lacune a un coût réel : un changement d'engine non vérifié (`setPreedit`
côté serveur selon `CapabilityFlag::Preedit`) déployé en production a **fait
planter fcitx5** (clavier inutilisable sur toute la session). Un test
d'intégration aurait attrapé ce crash avant tout déploiement.

## Objectifs

- Tester le **vrai** `libpredict.so` dans un **vrai** `fcitx::Instance`, en
  **headless** (pas de session graphique, pas de display/dbus).
- Couvrir le comportement observable de l'engine : ce qu'il **committe**, ce
  qu'il affiche (**candidats**, **preedit**), et qu'il **ne plante pas** selon
  les capacités du client.
- Tourner localement et en CI (`nix flake check`).
- Devenir le **filet obligatoire** avant tout futur travail engine : on teste
  ici → on valide en isolation → seulement ensuite on `nixos-rebuild`.

## Non-objectifs

- Re-tester la qualité de prédiction du daemon (déjà couverte par
  `test_predict.py`).
- Tester le rendu graphique du panneau QML (`ui/`) ou le positionnement pixel du
  panneau (dépend du compositeur/app — hors de portée d'un test headless).
- Refactorer `predict.cpp` : le fichier reste **inchangé** (les tests
  l'exercent tel quel par sa surface publique fcitx5).

## Approche retenue

**Harnais TestFrontend (vrai engine) + mock daemon in-process.**

fcitx5 fournit déjà toute l'infra de test, présente dans le paquet nixpkgs :
`libtestfrontend.so`, `libtestim.so`, `fcitx-utils/testing.h`, et les modules
CMake `Fcitx5ModuleTestFrontend`/`Fcitx5ModuleTestIM`. C'est le pattern officiel
des tests d'addons fcitx5.

Alternatives écartées :
- *Refactor « logique pure » + tests unitaires* : refactor risqué du fichier
  sensible, ET raterait les bugs d'**intégration** fcitx5 (exactement la classe
  du crash clavier). Rejeté.
- *e2e fcitx5 réel via dbus/wayland* : lourd, flaky, nécessite display/dbus.
  Rejeté.

### Pourquoi un mock daemon (et pas le vrai `predictord`)

L'engine parle au daemon via socket Unix JSON et lit `IME_PREDICTORD_SOCK`
(déjà supporté dans `connectDaemon`). Un **mock in-process** qui renvoie du JSON
**scripté par test** :
- **isole** le comportement de l'engine (c'est lui qu'on teste) ;
- rend chaque test **déterministe** : on contrôle exactement les champs renvoyés
  (`candidates`, `autocomplete`, `literalIsWord`) sans dépendre du scoring/modèle ;
- évite de builder + lancer `predictord` dans le test.

Le couplage engine↔daemon réel reste couvert indirectement (mêmes messages JSON)
et le daemon a sa propre suite.

## Architecture

Nouvel exécutable de test C++ : `engine/test_predict_engine.cpp`.

### Composants

1. **MockDaemon** — serveur socket Unix in-process (thread dédié).
   - Écoute sur un socket temporaire ; à chaque connexion : lit la requête JSON
     (`{context, prefix}` ou `{learn|forget|veto|stats}`), renvoie une réponse.
   - **Programmable** : file de réponses, ou règle par défaut (renvoie des
     candidats canoniques pour un préfixe), surchargée par test.
   - Robuste au fire-and-forget (`learn` : l'engine ferme sans lire) — ignore
     SIGPIPE, comme le vrai daemon.
   - Exposé au test : `mockReply(DaemonReply)` / `mockRespond(callback)`,
     et `lastRequest()` pour asserter ce que l'engine a envoyé (contexte,
     préfixe, learn/veto).
   - Branché via `setenv("IME_PREDICTORD_SOCK", tmpSock)` avant le démarrage de
     l'Instance.

2. **Bootstrap fcitx5** — calqué sur le pattern amont :
   - `setupTestingEnvironment(buildDir, {dir de libpredict.so, fcitx5 addon dir},
     {dir des .conf de test})` : addonDirs pour trouver `predict`/`testfrontend`/
     `testim`, dataDirs pour les `addon/*.conf` et `inputmethod/predict.conf`.
   - `Instance` avec `--disable=all --enable=testim,testfrontend,predict`,
     UI `testui`.
   - Bascule l'input method courant sur `predict` (groupe d'IM de test).
   - Tout le pilotage se fait dans un événement planifié sur l'`EventLoop`
     (`EventDispatcher::schedule`), puis `instance.exit()`.

3. **XDG_CONFIG_HOME de test** — un répertoire temporaire `ime-predictord/` où
   le test écrit `config.json` (mêmes réglages que le vrai : `frenchSpacing`,
   `autoCapitalize`, `nextWordBar`, `autoApply`, …). Permet de tester les
   chemins opt-in. (`engineCfg()` recharge sur mtime, comme en prod.)

4. **Mini-DSL de test** — helpers au-dessus du TestFrontend :
   - état : `setConfig(json)`, `setCaps(Preedit|SurroundingText|Password|…)`,
     `setSurrounding(text, cursor)`, `mockReply(...)`.
   - entrée : `type("bonjour")` (suite de `keyEvent`), `key(sym, states)`
     (ex. `Ctrl+BackSpace`, `Tab`, `space`, `Escape`).
   - assertions : `expectCommit("bonjour ")` (via `pushCommitExpectation`),
     `expectCandidates({"bonjour", …})`, `expectPreedit(text[, ghost])`,
     `expectNoPanel()`, et **survie du process** (pas de crash/abort).

### Flux d'un test (exemple)

```
setCaps(Preedit | SurroundingText);
mockReply({candidates:["bonjour"], autocomplete:"bonjour", literalIsWord:false});
type("bonjou");
expectPreedit("bonjou", /*ghost=*/"r");      // ghost text
expectCandidates({"bonjour"});
pushCommitExpectation("bonjour ");
key(space);                                   // auto-applique + espace
// le TestFrontend asserte que le commit == "bonjour "
```

## Couverture comportementale (« complet »)

Chaque item = au moins un test :

1. Composition + commit sur Espace (mot + espace finale).
2. Candidats de complétion affichés pour un préfixe.
3. Auto-application sur Espace quand dominant ; littéral gardé sinon (config).
4. Report de casse : `Bonjou→Bonjour`, `FRAN→FRANÇAIS`, `le→le`.
5. `literalIsWord` : un vrai mot n'est jamais écrasé.
6. Backspace en composition édite le buffer ; vide → barre fermée.
7. **Fermeture de barre sur Backspace buffer vide, y compris Ctrl+Backspace**
   (régression du correctif récent).
8. Barre mot-suivant après commit ; absente si `nextWordBar:false`.
9. Déclencheur emoji `:coeur`→❤️ et snippet `;mail`→expansion.
10. Navigation Tab/⇧Tab → sélection du candidat surligné.
11. Ponctuation committe le mot et file à l'app ; `. ! ?` réinitialise le contexte.
12. **frenchSpacing** : U+202F avant `; : ! ?` et `»`, après `«` (opt-in).
13. **autoCapitalize** : capitale en début de champ et après `. ! ?` (opt-in).
14. Escape ferme la barre / annule sans perdre le littéral.
15. Fenêtre de revert : Backspace après auto-application restaure le littéral.
16. Ghost text dans le preedit quand l'auto-complétion prolonge la frappe.
17. **Champ Password/Sensitive → aucune prédiction, aucun preedit.**
18. **CapabilityFlag::Preedit** : client (Preedit présent) vs serveur (absent) —
    pas de crash, preedit routé correctement (**le bug clavier**).
19. Contexte issu du SurroundingText (fenêtre).
20. Multi-mots « sais pas » committé/appris mot à mot.

## Intégration

- `engine/CMakeLists.txt` : cible `test_predict_engine` derrière une option
  `BUILD_TESTING` (OFF par défaut pour le build de prod), liée à `Fcitx5::Core`
  + le module `TestFrontend` + `nlohmann_json` + `pthread`. Copie du
  `predict.conf` dans l'arbo attendue par `setupTestingEnvironment`.
- `flake.nix` : nouvelle sortie **`checks.${system}.engine`** qui build la cible
  test et l'exécute. Le check `daemon` (test_predict.py) est exposé en parallèle
  (formalise l'existant). `nix flake check` lance les deux ; la CK CI les appelle.

## Fichiers touchés

- `engine/test_predict_engine.cpp` — **nouveau** (harnais + tests).
- `engine/CMakeLists.txt` — cible test optionnelle + copie .conf de test.
- `flake.nix` — sorties `checks` (engine + daemon).
- éventuel `engine/test/addon/` — .conf copiés pour l'environnement de test.
- `predict.cpp` — **inchangé**.

## Risques & mitigations

- *Wiring fcitx5 de test délicat* (addonDirs/dataDirs, activation de l'IM
  `predict`) → suivre le pattern amont (tests d'addons fcitx5) ; itérer dans le
  worktree isolé.
- *Accès au preedit/candidats pour les assertions* → via
  `instance.inputContextManager().findByUUID()` puis `ic->inputPanel()`.
- *Timing du mock socket* → connexion synchrone bornée côté engine (timeout
  150 ms déjà en place) ; le mock répond immédiatement sur son thread.

## Critères de succès

- `nix flake check` build et exécute le harnais ; tous les tests passent.
- Le harnais exerce **les deux chemins de capacité** (Preedit présent / absent)
  et asserte commit correct + **survie du process** (pas de crash/abort). Une
  régression qui ferait planter fcitx5 sur l'un de ces chemins ferait alors
  échouer le check — c'est la classe de bug qui a cassé le clavier. (Le
  mécanisme exact du crash n'avait pas été confirmé avant le revert ; l'objectif
  est de couvrir ces chemins, pas de rejouer un crash précis.)
- Le harnais tourne sans session graphique.
