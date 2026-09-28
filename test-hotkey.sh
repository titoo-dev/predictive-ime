#!/usr/bin/env bash
# Le PICKER EMOJI est un emprunt, pas une bascule : ce test vérifie les deux
# moitiés du contrat, dans un vrai pipeline headless (sway + fcitx5 + zenity),
# avec un profil à DEUX méthodes dont la courante n'est PAS la nôtre.
#
#   1. Super+; ouvre le picker alors que « keyboard-fr » est active (sans ce
#      chemin, fcitx n'enverrait la touche qu'à la méthode courante et le
#      raccourci serait mort tant qu'on n'a pas fait Ctrl+Espace).
#   2. Une fois l'emoji choisi, la méthode d'origine est RENDUE : la frappe
#      suivante reste littérale. Si l'IME prédictif restait actif, « bonjou »
#      deviendrait « bonjour » — c'est exactement ce qu'on ne veut pas.
#
# Attendu : « ok❤️ bonjou ».
#
# NB : l'injection clavier n'est fidèle que sous sway. Sous Hyprland, wtype
# téléverse son propre keymap et le compositeur résout le keycode dans le sien
# (Super+; y arrive en Super+Escape) : tester à la main sur cette machine-là.
#
# Usage : ./test-hotkey.sh   (depuis le repo)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" # le repo lui-même
WT=$(mktemp -d /tmp/ime-hk.XXXXXX); mkdir -p "$WT/config/fcitx5" "$WT/cache" "$WT/xdg"

ENGINE=$(nix build "$REPO#fcitx5-predict" --no-link --print-out-paths 2>/dev/null)
DAEMON=$(nix build "$REPO#predictord" --no-link --print-out-paths 2>/dev/null)
MODEL=$(nix build  "$REPO#model" --no-link --print-out-paths 2>/dev/null)
FCITX5=$(nix build nixpkgs#fcitx5 --no-link --print-out-paths 2>/dev/null)
SWAY=$(nix build  nixpkgs#sway   --no-link --print-out-paths 2>/dev/null)
ZENITY=$(nix build nixpkgs#zenity --no-link --print-out-paths 2>/dev/null)
WTYPE=$(nix build nixpkgs#wtype --no-link --print-out-paths 2>/dev/null)/bin/wtype
SOCK="$WT/pred.sock"

# DEUX méthodes : clavier d'abord (celle qui sera ACTIVE), predict ensuite.
cat > "$WT/config/fcitx5/profile" <<EOF
[Groups/0]
Name=Default
Default Layout=fr
DefaultIM=keyboard-fr
[Groups/0/Items/0]
Name=keyboard-fr
Layout=
[Groups/0/Items/1]
Name=predict
Layout=
[GroupOrder]
0=Default
EOF
cat > "$WT/sway.conf" <<EOF
output HEADLESS-1 resolution 1024x768
default_border none
input type:keyboard xkb_layout fr
exec "$WT/inner.sh"
EOF

XDG_DATA_HOME="$WT/xdg" XDG_CONFIG_HOME="$WT/config" \
  "$DAEMON/bin/predictord" "$MODEL/words.tsv" "$SOCK" >"$WT/daemon.log" 2>&1 &
DPID=$!
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.05; done

cat > "$WT/inner.sh" <<EOF
#!/usr/bin/env bash
export XDG_CONFIG_HOME="$WT/config" XDG_CACHE_HOME="$WT/cache" XDG_DATA_HOME="$WT/xdg"
export XDG_DATA_DIRS="$ENGINE/share:$FCITX5/share:/run/current-system/sw/share"
export FCITX_ADDON_DIRS="$FCITX5/lib/fcitx5:$ENGINE/lib/fcitx5"
export IME_PREDICTORD_SOCK="$SOCK" GSK_RENDERER=cairo IME_DEBUG=1
unset GTK_IM_MODULE QT_IM_MODULE; export XMODIFIERS=@im=fcitx
rm -rf "$WT/cache"; mkdir -p "$WT/cache"
"$FCITX5/bin/fcitx5" >"$WT/fcitx5.log" 2>&1 &
sleep 2
"$ZENITY/bin/zenity" --entry --title=ime --entry-text="" >"$WT/result" 2>/dev/null
EOF
chmod +x "$WT/inner.sh"

RUN="$WT/run"; mkdir -p "$RUN"; chmod 700 "$RUN"
: > "$WT/result"
( unset WAYLAND_DISPLAY DISPLAY
  export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
  export XDG_RUNTIME_DIR="$RUN"
  dbus-run-session -- "$SWAY/bin/sway" -c "$WT/sway.conf" >"$WT/sway.log" 2>&1 ) &
SWAYPID=$!
for _ in $(seq 1 200); do [ -S "$RUN/wayland-1" ] && break; sleep 0.1; done
sleep 6
inj() { XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$WTYPE" "$@" 2>>"$WT/wtype.log"; }
inj "q"; sleep 0.3; inj -k BackSpace; sleep 0.5   # warm-up focus

# porte de sanité : la frappe atteint bien l'appli (via le clavier simple)
inj -d 70 -- "ok"; sleep 0.8

inj -M logo -k semicolon -m logo; sleep 1.5   # picker SANS activer l'IME
inj -d 70 -- "coeur"; sleep 1.5
inj -k space; sleep 1.0
# APRÈS le picker : la frappe doit redevenir NORMALE (méthode d'origine).
# Si l'IME prédictif restait actif, « bonjou » deviendrait « bonjour ».
inj -d 70 -- "bonjou"; sleep 1.0
inj -k space; sleep 1.2
inj -k Return; sleep 1.5

for _ in $(seq 1 60); do [ -s "$WT/result" ] && break; sleep 0.1; done
GOT="$(cat "$WT/result")"
XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$SWAY/bin/swaymsg" exit >/dev/null 2>&1
kill $SWAYPID $DPID 2>/dev/null; wait $SWAYPID 2>/dev/null

echo "texte obtenu : '$GOT'"
case "$GOT" in
  "ok❤️ bonjou ")
    echo "  ok  picker OK, et la prédiction n'est PAS restée allumée"; exit 0 ;;
  "ok❤️ bonjour ")
    echo " FAIL l'IME prédictif est resté actif après le picker (bonjou→bonjour)"
    exit 1 ;;
  *"❤️"*) echo " FAIL emoji ok mais suite inattendue"; exit 1 ;;
  ok)     echo " FAIL le raccourci n'a rien fait (IME resté inactif)"; exit 1 ;;
  "")     echo " FAIL harnais muet (rien n'est arrivé à l'appli)"; exit 2 ;;
  *)      echo " FAIL sortie inattendue"; exit 1 ;;
esac
