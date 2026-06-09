#!/usr/bin/env bash
# Test live de l'IME maison SANS rien installer dans le système.
# Lance le daemon + une instance fcitx5 de test qui charge notre addon, avec
# "predict" comme méthode par défaut. Bascule avec Ctrl+Espace, tape des lettres
# → la liste de candidats vient du daemon n-gram.
#
# Usage:  ./ime/test-live.sh          (depuis la racine du repo)
#         ./ime/test-live.sh stop      (arrête le banc)
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTDIR=/tmp/ime-live
SOCK=/tmp/ime-predictord.sock

if [ "${1:-}" = "stop" ]; then
  pkill -x fcitx5 2>/dev/null || true
  pkill -x predictord 2>/dev/null || true
  # PIÈGE Wayland: `fcitx5 --replace` (voir plus bas) prend la slot input-method
  # du compositeur à la place de ibus. Le tuer ne restaure PAS ibus, et les
  # clients text-input déjà ouverts (vicinae/Win+R) restent accrochés à un IME
  # mort → clavier "bloqué" jusqu'au login. On répare les deux ici.
  # NB: ibus-daemon n'est pas sur le PATH (wrapper NixOS) → fallback via le store.
  IBUS_BIN="$(command -v ibus-daemon 2>/dev/null \
    || ls /nix/store/*ibus-with-plugins*/bin/ibus-daemon 2>/dev/null | head -1 \
    || true)"
  if [ -n "$IBUS_BIN" ]; then
    "$IBUS_BIN" -drxR && echo "ibus relancé."
  else
    echo "⚠ ibus-daemon introuvable — relance ton IME à la main."
  fi
  # vicinae garde une connexion text-input morte tant qu'il n'est pas relancé.
  if systemctl --user is-enabled vicinae >/dev/null 2>&1; then
    systemctl --user restart vicinae && echo "vicinae redémarré."
  fi
  echo "banc arrêté."
  exit 0
fi

echo "==> build des paquets + modèle FR+EN"
ENGINE=$(nix build "$REPO/ime#fcitx5-predict" --no-link --print-out-paths)
DAEMON=$(nix build "$REPO/ime#predictord" --no-link --print-out-paths)
MODEL=$(nix build "$REPO/ime#model" --no-link --print-out-paths)
# fcitx5 de base (frontend wayland + classicui + keyboard)
FCITX5=$(nix build nixpkgs#fcitx5 --no-link --print-out-paths)

echo "==> (re)démarrage du daemon de prédiction (modèle FR+EN 84k)"
pkill -x predictord 2>/dev/null || true
nohup "$DAEMON/bin/predictord" "$MODEL/words.tsv" "$SOCK" \
  >/tmp/ime-live-daemon.log 2>&1 &
sleep 1

echo "==> config fcitx5 de test (predict par défaut)"
rm -rf "$TESTDIR"; mkdir -p "$TESTDIR/config/fcitx5"
cat > "$TESTDIR/config/fcitx5/profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=predict

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=predict
Layout=

[GroupOrder]
0=Default
EOF

echo "==> lancement de fcitx5 de test (notre addon sur le chemin)"
pkill -x fcitx5 2>/dev/null || true; sleep 1
# Cache frais OBLIGATOIRE: sinon fcitx5 réutilise une liste d'IM périmée et ne
# voit pas notre addon (piège rencontré au debug).
rm -rf "$TESTDIR/cache"; mkdir -p "$TESTDIR/cache"
export XDG_CONFIG_HOME="$TESTDIR/config"
export XDG_CACHE_HOME="$TESTDIR/cache"
export XDG_DATA_DIRS="$ENGINE/share:$FCITX5/share:${XDG_DATA_DIRS:-/usr/share}"
export FCITX_ADDON_DIRS="$FCITX5/lib/fcitx5:$ENGINE/lib/fcitx5"
export IME_PREDICTORD_SOCK="$SOCK"
unset GTK_IM_MODULE QT_IM_MODULE
export XMODIFIERS='@im=fcitx'
nohup "$FCITX5/bin/fcitx5" --replace -d >/tmp/ime-live-fcitx5.log 2>&1 &
sleep 3

echo
echo "================ PRÊT ================"
echo "1. Va dans un champ texte (Chrome, ou KWrite via 'New File')."
echo "2. Appuie sur Ctrl+Espace pour activer 'Predict'."
echo "3. Tape 'comm' → barre de candidats. Tab/⇧Tab choisit, Espace ou Entrée"
echo "   valide le surligné. Backspace corrige."
echo "4. Espace SEUL : complète un fragment ('bonjou '→bonjour) ou corrige une"
echo "   faute ('bonjuor '→bonjour), mais n'écrase jamais un vrai mot ('le ')."
echo "5. Robustesse à tester : 'francais'→français, accents/apostrophe (j'ai),"
echo "   MAJUSCULES ('Bonjou'→Bonjour), chiffres et Entrée passent normalement."
echo "6. Mot-suivant : tape un mot + espace → barre ; Tab pour la prendre."
echo "Logs: /tmp/ime-live-fcitx5.log  /tmp/ime-live-daemon.log"
echo "Arrêt: ./ime/test-live.sh stop"
