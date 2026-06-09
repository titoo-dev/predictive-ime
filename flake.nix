{
  description = "IME prédictif maison — engine fcitx5 (Track A) + daemon n-gram (Track B)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      # Listes de fréquence OpenSubtitles 2018 (hermitdave/FrequencyWords),
      # ~50k mots/langue, format "mot fréquence". Épinglées par hash → pures.
      fr50k = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/fr/fr_50k.txt";
        hash = "sha256-+B98VwtkM3ZNqZqjD037CNgcWSYwE3e3Ca8joTsflZY=";
      };
      en50k = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt";
        hash = "sha256-U1H/QFsRJu9VV5HdTZeYpI4+mlAan8SBqdqVd1LPtFg=";
      };

      # Corpus de phrases pour les n-grammes (mot-suivant + complétion) :
      #  - Leipzig news 2024, 300k phrases/langue (registre écrit, CC BY) ;
      #  - Tatoeba via OPUS (release DATÉE v2023-04-12 → immuable, contrairement
      #    aux exports hebdomadaires de tatoeba.org ; CC BY 2.0 FR) — registre
      #    CONVERSATIONNEL, bien plus proche de la frappe quotidienne que la
      #    presse. Archives stables → hashables.
      fraNews = pkgs.fetchurl {
        url = "https://downloads.wortschatz-leipzig.de/corpora/fra_news_2024_300K.tar.gz";
        hash = "sha256-ZumUYu++H+txwCOet5X8QF3iIM3vF0lx2HEEdw9sQQM=";
      };
      engNews = pkgs.fetchurl {
        url = "https://downloads.wortschatz-leipzig.de/corpora/eng_news_2024_300K.tar.gz";
        hash = "sha256-NVsRu08GnxeQBElZOM45uhZF6xuG4sPSf/5OA6AR4VQ=";
      };
      tatoebaFr = pkgs.fetchurl {
        url = "https://object.pouta.csc.fi/OPUS-Tatoeba/v2023-04-12/mono/fr.txt.gz";
        hash = "sha256-eaRS30u3OCaYItrYR+pdtEuOtdNi2Nnve2YwXzgJi10=";
      };
      tatoebaEn = pkgs.fetchurl {
        url = "https://object.pouta.csc.fi/OPUS-Tatoeba/v2023-04-12/mono/en.txt.gz";
        hash = "sha256-oyxVAM12uUeYWXZPt4U3pLm1P6uPo73A/ATdcPKL8ps=";
      };

      # Annotations emoji CLDR (mots-clés officiels Unicode), FR + EN, épinglées
      # au tag immuable 48.2.0 — pour l'emoji picker (préfixe ':').
      cldrEmojiFr = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/unicode-org/cldr-json/48.2.0/cldr-json/cldr-annotations-full/annotations/fr/annotations.json";
        hash = "sha256-M9oFL3R4J5GYnmIm1ghxyEZ5ngd6Wn+06H7Pp8o4VzM=";
      };
      cldrEmojiEn = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/unicode-org/cldr-json/48.2.0/cldr-json/cldr-annotations-full/annotations/en/annotations.json";
        hash = "sha256-8iCDy4bf+2OmQ9W6y12YmfgtL6XTiK1POu1yGErO9QU=";
      };
      # Liste autoritaire des emoji + formes fully-qualified (rendu couleur).
      emojiTest = pkgs.fetchurl {
        url = "https://unicode.org/Public/emoji/16.0/emoji-test.txt";
        hash = "sha256-JPDFNOhs8ULiSWlT6PDkaj5wI5KRHt3NKcbM7YUTlpc=";
      };

      # Modèle FR+EN: fusionne les deux listes (fréquences cumulées pour les
      # mots communs), filtre nombres et tokens d'1 caractère. → words.tsv que
      # le daemon charge pour la complétion classée par fréquence.
      ime-model = pkgs.runCommand "ime-model" {
        nativeBuildInputs = [ pkgs.python3 pkgs.gnutar pkgs.gzip ];
      } ''
        mkdir -p $out
        # 1) words.tsv — complétion classée par fréquence
        cat ${fr50k} ${en50k} \
          | awk 'NF==2 && length($1)>=2 && $1 !~ /^[0-9]+$/ { f[$1]+=$2 } END { for (w in f) print w, f[w] }' \
          | sort -k2,2nr > $out/words.tsv
        echo "words.tsv: $(wc -l < $out/words.tsv) mots" >&2

        # 2) n-grammes Kneser-Ney — news (300K/langue) + Tatoeba conversationnel
        #    (l'EN Tatoeba est plafonné pour rester équilibré avec le FR).
        mkdir corpus
        tar xzf ${fraNews} -C corpus
        tar xzf ${engNews} -C corpus
        zcat ${tatoebaFr} | head -n 600000 > corpus/tatoeba-fr.txt || true
        zcat ${tatoebaEn} | head -n 600000 > corpus/tatoeba-en.txt || true
        python3 ${./daemon/build_ngrams.py} $out/words.tsv $out \
          corpus/*/*-sentences.txt corpus/tatoeba-fr.txt corpus/tatoeba-en.txt
        echo "bigrams.tsv:  $(wc -l < $out/bigrams.tsv) bigrammes"  >&2
        echo "trigrams.tsv: $(wc -l < $out/trigrams.tsv) trigrammes" >&2

        # 3) emoji.tsv — index mots-clés → emoji (CLDR fr+en) pour le picker ':'
        python3 ${./daemon/build_emoji.py} ${emojiTest} $out/emoji.tsv \
          ${cldrEmojiFr} ${cldrEmojiEn}
      '';
      # fcitx5 patché : expose l'accès brut à zwp_input_method_v2 aux addons UI
      # externes (installe waylandim_public.h + le module cmake WaylandIM), pour
      # que notre UI QML puisse créer la popup-surface positionnée au caret.
      fcitx5-patched = pkgs.fcitx5.overrideAttrs (old: {
        patches = (old.patches or [ ]) ++ [ ./ui/waylandim-public.patch ];
      });
    in
    {
      packages.${system} = {
        # fcitx5 avec l'API waylandim publique (pour l'UI QML).
        fcitx5-patched = fcitx5-patched;

        # Addon UI : popup-surface input-method au caret, rendu Qt Quick (QML).
        qmlpanel = pkgs.stdenv.mkDerivation {
          pname = "fcitx5-qmlpanel";
          version = "0.1";
          src = ./ui;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.kdePackages.extra-cmake-modules
            pkgs.pkg-config
            pkgs.wayland-scanner
            pkgs.qt6.qtbase
            pkgs.qt6.qtdeclarative
          ];
          buildInputs = [
            fcitx5-patched
            pkgs.wayland
            pkgs.qt6.qtbase
            pkgs.qt6.qtdeclarative
          ];
          dontWrapQtApps = true; # addon (plugin .so), pas une appli : chemins bakés
          cmakeFlags = [
            "-DQMLPANEL_QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/lib/qt-6/plugins:${pkgs.qt6.qtdeclarative}/lib/qt-6/plugins"
            "-DQMLPANEL_QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
          ];
        };

        # Track B + câblage : le daemon de prédiction (socket Unix + JSON).
        predictord = pkgs.stdenv.mkDerivation {
          pname = "ime-predictord";
          version = "0.1";
          src = ./daemon;
          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [ pkgs.nlohmann_json ];
        };

        # Track A : l'engine fcitx5 (réutilise le frontend Wayland éprouvé).
        fcitx5-predict = pkgs.stdenv.mkDerivation {
          pname = "fcitx5-predict";
          version = "0.1";
          src = ./engine;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.kdePackages.extra-cmake-modules
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.fcitx5
            pkgs.nlohmann_json
          ];
        };

        # Le modèle FR+EN (words.tsv) — exposé pour inspection/build direct.
        model = ime-model;

        default = self.packages.${system}.fcitx5-predict;
      };

      # Module NixOS : branche l'addon dans fcitx5 + lance le daemon (service
      # utilisateur). À importer dans la config + `i18n.inputMethod.type="fcitx5"`.
      # Module turn-key : importe-le et l'IME prédictif + sa barre QML sont
      # câblés. Conçu SÛR pour le clavier (cf README, section sécurité).
      #   - Sur Hyprland (WM nu), lance fcitx5 toi-même pour choisir l'UI QML :
      #       hl.exec_cmd("fcitx5 -d --ui qmlpanel")   (dans hyprland.start)
      #   - Pour activer la prédiction en permanence : DefaultIM = "predict".
      nixosModules.default =
        { config, lib, pkgs, ... }:
        {
          # fcitx5 patché : expose getInputMethodV2Raw aux addons UI → qmlpanel
          # peut créer la popup-surface placée au caret. Patch minimal (4 lignes).
          nixpkgs.overlays = [
            (final: prev: {
              fcitx5 = prev.fcitx5.overrideAttrs (old: {
                patches = (old.patches or [ ]) ++ [ ./ui/waylandim-public.patch ];
              });
            })
          ];

          i18n.inputMethod = {
            enable = true;
            type = "fcitx5";
            fcitx5 = {
              # Frontend Wayland : ne force PAS GTK_IM_MODULE/QT_IM_MODULE. Donc
              # si fcitx est absent/planté, les apps reçoivent les touches BRUTES
              # (text-input-v3) → jamais de clavier bloqué.
              waylandFrontend = true;
              addons = [
                self.packages.${system}.fcitx5-predict
                self.packages.${system}.qmlpanel
              ];
              # Profil SÛR par défaut : clavier français d'abord (saisie 100%
              # normale), predict en second → Ctrl+Espace active la prédiction
              # (et la barre QML). Mettre DefaultIM="predict" pour l'avoir d'office.
              settings.inputMethod = {
                "Groups/0" = {
                  Name = "Default";
                  "Default Layout" = "fr";
                  DefaultIM = "keyboard-fr";
                };
                "Groups/0/Items/0" = {
                  Name = "keyboard-fr";
                  Layout = "";
                };
                "Groups/0/Items/1" = {
                  Name = "predict";
                  Layout = "";
                };
                "GroupOrder"."0" = "Default";
              };
            };
          };

          # Daemon de prédiction (n-gram), service utilisateur.
          systemd.user.services.ime-predictord = {
            description = "IME prediction daemon (n-gram)";
            wantedBy = [ "graphical-session.target" ];
            partOf = [ "graphical-session.target" ];
            serviceConfig = {
              ExecStart = "${self.packages.${system}.predictord}/bin/predictord "
                + "${ime-model}/words.tsv";
              Restart = "on-failure";
            };
          };
        };

      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.kdePackages.extra-cmake-modules
          pkgs.pkg-config
          pkgs.fcitx5
          pkgs.nlohmann_json
          pkgs.gcc
        ];
      };
    };
}
