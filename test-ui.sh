#!/usr/bin/env bash
# Test VISUEL headless de la barre QML (qmlpanel), sans toucher à la session :
# sway headless + fcitx5 PATCHÉ (--ui qmlpanel) + zenity (text-input-v3) +
# injection wtype + captures grim. Produit des PNG dans /tmp/ime-ui/ :
#   1-completion.png      barre de complétion ("bonjou")
#   2-nav-pill.png        pill accent sur le candidat surligné (Tab)
#   3-pill-morph-mid.png  pill EN COURS de glissement (anim ralentie ×20)
#   4-nextword.png        barre mot-suivant après commit ("je ")
#   5-emoji.png           picker emoji (Super+; puis "coeur")
#   6-appear-mid.png      barre EN COURS d'apparition (fade+slide, anim ×20)
# Vérifie aussi (assertions) que les mid-frames diffèrent des états stables —
# preuve que la boucle de frames Wayland anime réellement.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=/tmp/ime-ui; rm -rf "$OUT"; mkdir -p "$OUT"
WT="$OUT/wt"; mkdir -p "$WT/config/fcitx5" "$WT/cache" "$WT/xdg"

echo "==> build paquets + outils"
ENGINE=$(nix build "$REPO/ime#fcitx5-predict" --no-link --print-out-paths 2>/dev/null)
QMLUI=$(nix build "$REPO/ime#qmlpanel" --no-link --print-out-paths 2>/dev/null)
FCITX5=$(nix build "$REPO/ime#fcitx5-patched" --no-link --print-out-paths 2>/dev/null)
DAEMON=$(nix build "$REPO/ime#predictord" --no-link --print-out-paths 2>/dev/null)
MODEL=$(nix build  "$REPO/ime#model" --no-link --print-out-paths 2>/dev/null)
SWAY=$(nix build  nixpkgs#sway   --no-link --print-out-paths 2>/dev/null)
ZENITY=$(nix build nixpkgs#zenity --no-link --print-out-paths 2>/dev/null)
WTYPE=$(nix build nixpkgs#wtype --no-link --print-out-paths 2>/dev/null)/bin/wtype
GRIM=$(nix build nixpkgs#grim --no-link --print-out-paths 2>/dev/null)/bin/grim
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
output HEADLESS-1 resolution 900x300
default_border none
input type:keyboard xkb_layout fr
exec "$WT/inner.sh"
EOF

# Échap AVALÉ ici (escapeForward=false) : le scénario l'utilise pour fermer la
# barre sans fermer zenity (par défaut la touche traverse vers l'application).
mkdir -p "$WT/config/ime-predictord"
printf '{"escapeForward": false}\n' > "$WT/config/ime-predictord/config.json"

echo "==> daemon de prédiction (XDG_DATA_HOME isolé : pas d'apprentissage réel)"
XDG_DATA_HOME="$WT/xdg" "$DAEMON/bin/predictord" "$MODEL/words.tsv" "$SOCK" >"$WT/daemon.log" 2>&1 &
DPID=$!
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.05; done

cat > "$WT/inner.sh" <<EOF
#!/usr/bin/env bash
export XDG_CONFIG_HOME="$WT/config" XDG_CACHE_HOME="$WT/cache" XDG_DATA_HOME="$WT/xdg"
export XDG_DATA_DIRS="$ENGINE/share:$QMLUI/share:$FCITX5/share:/run/current-system/sw/share"
export FCITX_ADDON_DIRS="$FCITX5/lib/fcitx5:$ENGINE/lib/fcitx5:$QMLUI/lib/fcitx5"
export IME_PREDICTORD_SOCK="$SOCK" GSK_RENDERER=cairo
export QMLPANEL_ANIM_SCALE="\${QMLPANEL_ANIM_SCALE:-1}"
unset GTK_IM_MODULE QT_IM_MODULE; export XMODIFIERS=@im=fcitx
rm -rf "$WT/cache"; mkdir -p "$WT/cache"
"$FCITX5/bin/fcitx5" --ui qmlpanel >"$WT/fcitx5.log" 2>&1 &
FPID=\$!
sleep 2
"$ZENITY/bin/zenity" --entry --title=ime --entry-text="" >/dev/null 2>&1
kill \$FPID 2>/dev/null
EOF
chmod +x "$WT/inner.sh"

RUN="$WT/run"; mkdir -p "$RUN"; chmod 700 "$RUN"
( unset WAYLAND_DISPLAY DISPLAY
  export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
  export XDG_RUNTIME_DIR="$RUN" QMLPANEL_ANIM_SCALE=20
  dbus-run-session -- "$SWAY/bin/sway" -c "$WT/sway.conf" >"$WT/sway.log" 2>&1 ) &
SWAYPID=$!
SK="$RUN/wayland-1"
for t in $(seq 1 150); do [ -S "$SK" ] && break; sleep 0.1; done
[ -S "$SK" ] || { echo "FAIL: sway KO"; kill $SWAYPID $DPID 2>/dev/null; exit 1; }
sleep 5

inj()  { XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$WTYPE" "$@" 2>>"$WT/wtype.log"; }
shot() { XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$GRIM" "$OUT/$1" 2>>"$WT/grim.log"; }

FAILS=0
differ() { # $1 $2 : les PNG doivent différer (preuve d'une frame intermédiaire)
  if cmp -s "$OUT/$1" "$OUT/$2"; then
    echo " FAIL $1 == $2 (aucune animation visible)"; FAILS=$((FAILS+1))
  else echo "  ok  $1 ≠ $2"; fi
}

echo "==> scénarios (anim ×20 : apparition 2.8s, morph 2.2s)"
inj "q"; sleep 0.3; inj -k BackSpace; sleep 0.5      # warm-up focus

# 6) apparition : frame intermédiaire ~1s après la 1re lettre, stable à 3.5s
inj -d 60 -- "bonjou"
sleep 1.0; shot 6-appear-mid.png
sleep 3.0; shot 1-completion.png
differ 6-appear-mid.png 1-completion.png

# 2+3) navigation : Tab surligne (pill), 2e Tab → morph en cours puis stable
inj -k Tab; sleep 3.0; shot 2-nav-pill.png
inj -k Tab; sleep 0.8; shot 3-pill-morph-mid.png
sleep 2.5; shot 2b-nav-pill-end.png
differ 3-pill-morph-mid.png 2b-nav-pill-end.png
differ 2-nav-pill.png 2b-nav-pill-end.png

# 4) mot-suivant : Échap (littéral), efface, "je " → barre contextuelle
inj -k Escape; sleep 0.3
for _ in 1 2 3 4 5 6; do inj -k BackSpace; sleep 0.1; done
inj -d 60 -- "je "; sleep 3.5; shot 4-nextword.png

# 5) emoji : GRILLE 3×8 + navigation 2D (Tab entre, ↓ saute une LIGNE)
# RÉGRESSION 1er rendu : la TOUTE PREMIÈRE frame de la grille (bascule
# barre→grille, aucune animation en vol → une seule frame) doit déjà être à
# la taille grille. Avant le fix (resize + re-grab dans PanelView::render),
# elle restait CLIPÉE à la hauteur de la barre mot-suivant (2e rangée coupée)
# jusqu'à la frappe suivante — à inspecter : rangées entières visibles.
inj -M logo -k semicolon -m logo; sleep 1.5; shot 5a-emoji-first-frame.png
inj -d 60 -- "coeur"; sleep 2.5; shot 5-emoji.png
inj -k Tab; sleep 3.0; shot 5b-emoji-nav.png
inj -k Down; sleep 3.0; shot 5c-emoji-row2.png
differ 5b-emoji-nav.png 5c-emoji-row2.png
inj -k Escape; sleep 0.3

# 7) FONDU de fermeture : Échap → barre en cours de fondu (anim ×20 = 1.8s)
inj -d 60 -- "bonjou"; sleep 3.5
inj -k Escape; sleep 0.7; shot 7-fadeout-mid.png
sleep 2.5; shot 7b-fadeout-end.png
differ 7-fadeout-mid.png 7b-fadeout-end.png

XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY=wayland-1 "$SWAY/bin/swaymsg" exit >/dev/null 2>&1 || true
kill $SWAYPID $DPID 2>/dev/null; wait $SWAYPID 2>/dev/null
pkill -x zenity 2>/dev/null

echo
ls -la "$OUT"/*.png 2>/dev/null
if [ "$FAILS" -eq 0 ]; then echo "captures OK ✓ (inspecter $OUT/*.png)"; exit 0
else echo "$FAILS assertion(s) en échec"; exit 1; fi
