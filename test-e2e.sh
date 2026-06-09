#!/usr/bin/env bash
# Test e2e HEADLESS de l'IME, à travers le vrai pipeline fcitx5 ↔ Wayland, SANS
# toucher à la session : compositeur sway (wlroots, backend headless) en layout
# AZERTY, fcitx5 sur un bus D-Bus PRIVÉ (donc il ne remplace pas le fcitx5 de la
# session), zenity comme client text-input-v3, injection clavier via wtype
# (virtual-keyboard, sans root/uinput). On tape une séquence et on vérifie le
# texte committé. Chaque cas tourne dans un XDG_RUNTIME_DIR privé (socket
# déterministe) et nettoie son fcitx5 (aucune fuite).
#
# Ce test a attrapé deux vrais bugs : un crash SIGPIPE du daemon (prédictions
# mortes après quelques mots) et la mutilation des contractions (j'ai → jail).
#
# Prérequis (téléchargés par nix au besoin) : sway, zenity, wtype, dbus.
# Usage : ./ime/test-e2e.sh   (depuis la racine du repo)
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WT=/tmp/ime-e2e; rm -rf "$WT"; mkdir -p "$WT/config/fcitx5" "$WT/cache" "$WT/xdg"

echo "==> build paquets + modèle + outils"
ENGINE=$(nix build "$REPO/ime#fcitx5-predict" --no-link --print-out-paths 2>/dev/null)
DAEMON=$(nix build "$REPO/ime#predictord" --no-link --print-out-paths 2>/dev/null)
MODEL=$(nix build  "$REPO/ime#model" --no-link --print-out-paths 2>/dev/null)
FCITX5=$(nix build nixpkgs#fcitx5 --no-link --print-out-paths 2>/dev/null)
SWAY=$(nix build  nixpkgs#sway   --no-link --print-out-paths 2>/dev/null)
ZENITY=$(nix build nixpkgs#zenity --no-link --print-out-paths 2>/dev/null)
WTYPE=$(nix build nixpkgs#wtype --no-link --print-out-paths 2>/dev/null)/bin/wtype
SOCK="$WT/pred.sock"

cat > "$WT/config/fcitx5/profile" <<EOF
[Groups/0]
Name=Default
Default Layout=fr
DefaultIM=predict
[Groups/0/Items/0]
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

echo "==> daemon de prédiction (XDG_DATA_HOME isolé : pas d'apprentissage réel)"
pkill -x predictord 2>/dev/null || true
XDG_DATA_HOME="$WT/xdg" "$DAEMON/bin/predictord" "$MODEL/words.tsv" "$SOCK" >"$WT/daemon.log" 2>&1 &
DPID=$!
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.05; done

cat > "$WT/inner.sh" <<EOF
#!/usr/bin/env bash
export XDG_CONFIG_HOME="$WT/config" XDG_CACHE_HOME="$WT/cache" XDG_DATA_HOME="$WT/xdg"
export XDG_DATA_DIRS="$ENGINE/share:$FCITX5/share:/run/current-system/sw/share"
export FCITX_ADDON_DIRS="$FCITX5/lib/fcitx5:$ENGINE/lib/fcitx5"
export IME_PREDICTORD_SOCK="$SOCK" GSK_RENDERER=cairo
unset GTK_IM_MODULE QT_IM_MODULE; export XMODIFIERS=@im=fcitx
rm -rf "$WT/cache"; mkdir -p "$WT/cache"
"$FCITX5/bin/fcitx5" >"$WT/fcitx5.log" 2>&1 &
FPID=\$!
sleep 2
"$ZENITY/bin/zenity" --entry --title=ime --entry-text="" >"\$IME_RESULT" 2>/dev/null
kill \$FPID 2>/dev/null   # pas de fcitx5 fuité
EOF
chmod +x "$WT/inner.sh"

N=0; FAILS=0
expect() {  # $1=libellé  $2=chaîne tapée  $3=résultat attendu
  N=$((N+1)); local label="$1" keys="$2" want="$3" res="$WT/r$N"
  : > "$res"
  local RUN="$WT/run$N"; mkdir -p "$RUN"; chmod 700 "$RUN"
  ( unset WAYLAND_DISPLAY DISPLAY
    export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
    export XDG_RUNTIME_DIR="$RUN" IME_RESULT="$res"
    dbus-run-session -- "$SWAY/bin/sway" -c "$WT/sway.conf" >"$WT/sway$N.log" 2>&1 ) &
  local swaypid=$! sk="$RUN/wayland-1" t
  for t in $(seq 1 150); do [ -S "$sk" ] && break; sleep 0.1; done
  if [ ! -S "$sk" ]; then echo " FAIL  $label : sway KO"; FAILS=$((FAILS+1)); kill $swaypid 2>/dev/null; return; fi
  sleep 5
  inj() { XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$WTYPE" "$@" 2>>"$WT/wtype.log"; }
  inj "q"; sleep 0.3; inj -k BackSpace; sleep 0.3   # warm-up (drop focus, bug fcitx #5815)
  # la chaîne peut contenir <ESC> → touche Échap injectée entre les segments
  local rest="$keys" seg
  while [ -n "$rest" ]; do
    seg="${rest%%<ESC>*}"
    [ -n "$seg" ] && inj -d 70 -- "$seg"
    if [ "$seg" = "$rest" ]; then rest=""
    else sleep 0.3; inj -k Escape; sleep 0.3; rest="${rest#*<ESC>}"; fi
  done
  sleep 0.4; inj -k Return
  for t in $(seq 1 60); do [ -s "$res" ] && break; sleep 0.1; done
  sleep 0.3
  XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$SWAY/bin/swaymsg" exit >/dev/null 2>&1 || true
  kill $swaypid 2>/dev/null; wait $swaypid 2>/dev/null
  local got; got="$(cat "$res")"
  if [ "$got" = "$want" ]; then printf "  ok  %-32s → '%s'\n" "$label" "$got"
  else printf " FAIL %-32s → '%s' (attendu '%s')\n" "$label" "$got" "$want"; FAILS=$((FAILS+1)); fi
}

echo "==> CAS (clavier AZERTY)"
expect "warmup (jeté)"                 "test "                  "test "
expect "accent é direct"               "café "                  "café "
expect "é+è directs"                   "élève "                 "élève "
expect "ç direct + 2 mots"             "ça va "                 "ça va "
expect "contraction j'"                "j'ai "                  "j'ai "
expect "contraction c'"                "c'est "                 "c'est "
expect "complétion sur espace"         "bonjou "                "bonjour "
expect "autocorrection faute simple"   "teh "                   "the "
expect "no-clobber d'un vrai mot"      "le "                    "le "
expect "chiffres NON avalés"           "code 3 "                "code 3 "
expect "phrase multi-mots + accents"   "je suis allé au café "  "je suis allé au café "
expect "emoji picker ':coeur'+espace"  ":coeur "                "❤️ "
expect "':' nu reste littéral"         ": ok "                  ": ok "
expect "Échap annule la suggestion"    "bonjou<ESC> ok "        "bonjou ok "
expect "Échap ferme la barre (avalé)"  "je <ESC>vais "          "je vais "

echo
[ -d /proc/$DPID ] && echo "daemon survécu (SIGPIPE OK)" || { echo "DAEMON MORT"; FAILS=$((FAILS+1)); }
kill $DPID 2>/dev/null; pkill -x sway 2>/dev/null; pkill -x zenity 2>/dev/null
echo
if [ "$FAILS" -eq 0 ]; then echo "tous les cas passent ✓"; exit 0
else echo "$FAILS cas en échec"; exit 1; fi
