#include "panelview.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>

// QML : TROIS rendus dans une même surface, choisis par `listMode` / `gridMode` —
//  • COMPACT (mots) : barre de chips horizontales dense (façon Gboard),
//    PILL accent qui GLISSE entre les candidats (hlFrom→hlTo, progression
//    hlT animée en C++ : trajet en ligne droite, diagonale comprise).
//  • PICKER EMOJI (`gridMode`) : surface Material 3 — champ de recherche en
//    tête (la requête ne va PLUS dans l'application, cf engine), grille 8
//    colonnes de cases carrées, case sélectionnée en secondaryContainer, ligne
//    d'aide clavier en bas, état vide quand la recherche ne rend rien. Largeur
//    FIXE (8 colonnes) : la grille ne saute plus à chaque frappe.
//  • LISTE (reformulation) : lignes verticales NUMÉROTÉES pleine largeur, texte
//    qui passe à la ligne (les variantes sont des phrases — illisibles élidées à
//    190px). Header de mode + ligne d'attente avec spinner (`spin`) quand
//    `loading`. Apparition fade + slide-up commune (appear 0→1).
// Couleurs via la context property `colors` (matugen → rôles Material 3, cf
// qmlui.cpp / loadColors).
static const char *kPanelQml = R"QML(
import QtQuick

Item {
    id: root
    implicitWidth: card.width + 10
    implicitHeight: card.height + 10

    // Métriques Material 3 du picker (dp ≈ px ici : la surface est rendue à 1x)
    readonly property int cell: 32          // case de la grille
    readonly property int gap: 4
    readonly property int pad: 10
    readonly property int cols: 8

    Rectangle {
        id: card
        x: 5
        y: 5 + (1 - appear) * 6        // slide-up à l'apparition
        opacity: appear
        // shape scale M3 : large (16) pour la surface picker, medium sinon
        radius: gridMode ? 16 : (listMode ? 14 : 11)
        color: colors.surface
        border.width: 1
        border.color: colors.outline
        width:  listMode ? listLayout.width
                         : (gridMode ? picker.width : compact.width)
        height: listMode ? listLayout.height
                         : (gridMode ? picker.height : compact.height)

        // ===================== COMPACT (mots) =====================
        Item {
            id: compact
            visible: !listMode && !gridMode
            width: row.width + 18
            height: row.height + 8     // une seule ligne de chips : 34

            // indicateur de mode : accent = mot en cours (composition),
            // discret = barre passive (mot-suivant / amorce)
            Rectangle {
                x: 5
                width: 3
                height: 12
                radius: 1.5
                anchors.verticalCenter: parent.verticalCenter
                color: composing ? colors.accent : colors.outline
            }

            // pill de surlignage : interpole position/taille entre les chips —
            // en x ET en y (la grille emoji a plusieurs lignes)
            Rectangle {
                id: pill
                visible: highlight >= 0 && rep.count > 0
                // interpolation DIRECTE entre la case de départ et celle
                // d'arrivée (cf hlT côté C++) : trajet en ligne droite
                property Item a: rep.count > 0
                    ? rep.itemAt(Math.max(0, Math.min(hlFrom, rep.count - 1))) : null
                property Item b: rep.count > 0
                    ? rep.itemAt(Math.max(0, Math.min(hlTo, rep.count - 1))) : null
                property real f: hlT
                x: a && b ? row.x + a.x + (b.x - a.x) * f : 0
                y: a && b ? row.y + a.y + (b.y - a.y) * f : 0
                width: a && b ? a.width + (b.width - a.width) * f : 0
                height: 26
                radius: 8
                color: colors.accent
            }

            Grid {
                id: row
                x: 12
                spacing: 1
                anchors.verticalCenter: parent.verticalCenter
                // fondu doux quand la barre PASSIVE se rafraîchit (refresh neural
                // asynchrone) — la frappe (composition) reste instantanée
                opacity: contentFade
                columns: Math.max(1, rep.count)
                Repeater {
                    id: rep
                    model: (listMode || gridMode) ? [] : candidates
                    delegate: Item {
                        required property int index
                        required property string modelData
                        // décision C++ (cf looksEmoji) : l'heuristique « 1er
                        // point de code > 0x2100 » ratait les keycaps (1️⃣), ©️…
                        readonly property bool emoji:
                            index < emojiMark.length && emojiMark[index] === true
                        readonly property bool isAuto:
                            index < autoMark.length && autoMark[index] === true
                        width: label.width + 16
                        height: 26
                        // liseré accent = « l'Espace appliquera CE candidat »
                        Rectangle {
                            anchors.fill: parent
                            radius: 8
                            color: "transparent"
                            border.width: 1
                            border.color: colors.accent
                            opacity: 0.9
                            visible: isAuto && highlight < 0
                        }
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: modelData
                            width: Math.min(implicitWidth, 190)
                            elide: Text.ElideRight
                            color: index === highlight
                                   ? colors.onAccent : colors.onSurface
                            font.pixelSize: emoji ? 17 : 14
                            // NB: font.families (plural, QStringList) is NOT a
                            // QML-assignable property of the font value type — it
                            // exists only on the C++ QFont. Qt 6.11's QML engine
                            // rejects it ("Cannot assign to non-existent property
                            // families"), so the whole panel component fails to
                            // load and keystrokes get swallowed. Use font.family
                            // (string); glyph fallback is fontconfig's job.
                            font.family: emoji ? "Noto Color Emoji" : "Maple Mono NF"
                        }
                    }
                }
            }
        }

        // ================== PICKER EMOJI (Material 3) ==================
        // Surface autonome : champ de recherche + grille + aide clavier. La
        // largeur est FIXE (8 colonnes) — une grille qui rétrécit à chaque
        // frappe est illisible, et le champ de recherche a besoin d'un rail
        // stable.
        Item {
            id: picker
            visible: gridMode && !listMode
            readonly property bool empty: candidates.length === 0
            width: root.cols * root.cell + (root.cols - 1) * root.gap
                   + 2 * root.pad
            height: hint.y + hint.height + root.pad

            // --- champ de recherche (M3 search bar : pleine rondeur) ---
            Rectangle {
                id: search
                x: root.pad
                y: root.pad
                width: parent.width - 2 * root.pad
                height: 30
                radius: height / 2
                color: colors.surfaceVariant

                // loupe DESSINÉE : le rendu est software et la fonte du
                // panneau n'a pas forcément le glyphe (⌕/nerd-font) — deux
                // rectangles se rendent partout.
                Item {
                    id: lens
                    x: 11
                    width: 13
                    height: 13
                    anchors.verticalCenter: parent.verticalCenter
                    Rectangle {
                        width: 9; height: 9; radius: 4.5
                        color: "transparent"
                        border.width: 1.4
                        border.color: colors.onSurfaceVariant
                    }
                    Rectangle {              // manche, à 45°
                        x: 7.5; y: 7.5
                        width: 5.5; height: 1.4; radius: 0.7
                        transformOrigin: Item.TopLeft
                        rotation: 45
                        color: colors.onSurfaceVariant
                    }
                }

                Text {
                    id: qtext
                    anchors.left: lens.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: searchText.length ? searchText
                                            : "Rechercher un emoji"
                    // requête longue : on élide par la GAUCHE, la fin de la
                    // frappe (ce qu'on vient de taper) reste visible
                    width: Math.min(implicitWidth, search.width - 52)
                    elide: Text.ElideLeft
                    color: searchText.length ? colors.onSurface
                                             : colors.onSurfaceVariant
                    opacity: searchText.length ? 1.0 : 0.75
                    font.pixelSize: 13
                    font.family: "Maple Mono NF"
                }
                Rectangle {                  // caret : point d'insertion
                    visible: searchText.length > 0
                    anchors.left: qtext.right
                    anchors.leftMargin: 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1.5
                    height: 15
                    radius: 0.75
                    color: colors.accent
                }
                Text {                       // page courante (« 2/4 »)
                    visible: headerText.length > 0
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: headerText
                    color: colors.onSurfaceVariant
                    font.pixelSize: 11
                    font.family: "Maple Mono NF"
                }
            }

            // pill de sélection : interpolation directe départ→arrivée en x ET
            // en y (la grille a plusieurs lignes ; un saut de ligne descend
            // en diagonale au lieu de balayer la ligne).
            // M3 : état sélectionné = secondaryContainer.
            Rectangle {
                id: gpill
                visible: highlight >= 0 && grep.count > 0
                property Item a: grep.count > 0
                    ? grep.itemAt(Math.max(0, Math.min(hlFrom, grep.count - 1))) : null
                property Item b: grep.count > 0
                    ? grep.itemAt(Math.max(0, Math.min(hlTo, grep.count - 1))) : null
                property real f: hlT
                x: a && b ? grid.x + a.x + (b.x - a.x) * f : 0
                y: a && b ? grid.y + a.y + (b.y - a.y) * f : 0
                width: root.cell
                height: root.cell
                radius: 10
                color: colors.secondaryContainer
            }

            Grid {
                id: grid
                x: root.pad
                y: search.y + search.height + root.pad
                visible: !picker.empty
                opacity: contentFade
                columns: root.cols
                spacing: root.gap
                Repeater {
                    id: grep
                    model: gridMode ? candidates : []
                    delegate: Item {
                        required property int index
                        required property string modelData
                        readonly property bool isAuto:
                            index < autoMark.length && autoMark[index] === true
                        width: root.cell
                        height: root.cell
                        // liseré = « Entrée/Espace prendra celui-ci » tant
                        // qu'on n'a pas navigué
                        Rectangle {
                            anchors.fill: parent
                            radius: 10
                            color: "transparent"
                            border.width: 1
                            border.color: colors.accent
                            opacity: 0.55
                            visible: isAuto && highlight < 0
                        }
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 19
                            font.family: "Noto Color Emoji"
                        }
                    }
                }
            }

            // état VIDE : la recherche ne rend rien. Le picker reste ouvert
            // (fermer la barre à la première faute de frappe est brutal) et le
            // dit — la frappe suivante peut retomber sur des résultats.
            Text {
                id: none
                visible: picker.empty
                x: root.pad
                y: grid.y
                width: parent.width - 2 * root.pad
                height: 44
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
                text: searchText.length
                      ? "Aucun emoji pour « " + searchText + " »"
                      : "Aucun emoji"
                color: colors.onSurfaceVariant
                font.pixelSize: 12
                font.family: "Maple Mono NF"
            }

            // aide clavier (M3 supporting text) : le picker vient d'un
            // raccourci, ses touches ne s'apprennent nulle part ailleurs.
            Text {
                id: hint
                x: root.pad
                y: (picker.empty ? none.y + none.height
                                 : grid.y + grid.height) + 7
                width: parent.width - 2 * root.pad
                horizontalAlignment: Text.AlignHCenter
                text: "↑↓←→ naviguer · Entrée insérer · Échap fermer"
                color: colors.onSurfaceVariant
                opacity: 0.75
                font.pixelSize: 10
                font.family: "Maple Mono NF"
            }
        }

        // ===================== LISTE (reformulation) =====================
        Column {
            id: listLayout
            visible: listMode
            width: 380
            topPadding: 7
            bottomPadding: 8
            spacing: 2

            // header : titre du mode + (si chargement) spinner qui tourne
            Item {
                width: listLayout.width
                height: 22
                Rectangle {                       // pastille accent du mode
                    id: dot
                    x: 12
                    width: 6; height: 6; radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: colors.accent
                }
                Text {
                    anchors.left: dot.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: loading
                          ? "Reformulation…"
                          : (headerText.length ? headerText
                                               : "Reformuler · 1–" + candidates.length)
                    color: colors.onSurface
                    opacity: 0.65
                    font.pixelSize: 11
                    font.family: "Maple Mono NF"
                }
                Text {                            // spinner (rotation pilotée C++)
                    visible: loading
                    anchors.right: parent.right
                    anchors.rightMargin: 13
                    anchors.verticalCenter: parent.verticalCenter
                    text: "⟳"
                    color: colors.accent
                    font.pixelSize: 14
                    font.family: "Maple Mono NF"
                    transformOrigin: Item.Center
                    rotation: spin
                }
            }

            Rectangle {                           // séparateur
                width: listLayout.width
                height: 1
                opacity: 0.5
                color: colors.outline
            }

            // ligne d'attente pendant la génération (placeholder discret)
            Item {
                visible: loading
                width: listLayout.width
                height: 30
                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "génération des variantes…"
                    color: colors.onSurface
                    opacity: 0.5
                    font.pixelSize: 13
                    font.family: "Maple Mono NF"
                }
            }

            // les variantes, numérotées, pleine largeur, multi-lignes
            Repeater {
                id: lrep
                model: loading ? [] : candidates
                delegate: Item {
                    required property int index
                    required property string modelData
                    readonly property bool sel: index === highlight
                    width: listLayout.width
                    height: Math.max(30, vtxt.implicitHeight + 13)

                    Rectangle {                   // surlignage pleine ligne
                        anchors.fill: parent
                        anchors.leftMargin: 5
                        anchors.rightMargin: 5
                        anchors.topMargin: 1
                        anchors.bottomMargin: 1
                        radius: 8
                        color: sel ? colors.accent : "transparent"
                    }
                    Rectangle {                   // badge numéro
                        id: badge
                        x: 11
                        width: 18; height: 18; radius: 9
                        anchors.verticalCenter: parent.verticalCenter
                        color: sel ? colors.onAccent : colors.accent
                        opacity: sel ? 0.22 : 0.16
                    }
                    Text {
                        anchors.centerIn: badge
                        text: (index < labels.length && labels[index].length)
                              ? labels[index] : (index + 1)
                        color: sel ? colors.onAccent : colors.accent
                        font.pixelSize: 11
                        font.family: "Maple Mono NF"
                    }
                    Text {
                        id: vtxt
                        anchors.left: badge.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 13
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        wrapMode: Text.WordWrap
                        color: sel ? colors.onAccent : colors.onSurface
                        font.pixelSize: 13
                        font.family: "Maple Mono NF"
                    }
                }
            }
        }
    }
}
)QML";

namespace {
QString envPath(const char *var, const QString &fallback) {
    QByteArray v = qgetenv(var);
    return v.isEmpty() ? fallback : QString::fromLocal8Bit(v);
}

QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString dmsColorsPath() {
    QString home = envPath("HOME", "");
    return envPath("XDG_CACHE_HOME", home + "/.cache") +
           "/DankMaterialShell/dms-colors.json";
}
QString overrideColorsPath() {
    QString home = envPath("HOME", "");
    return envPath("XDG_CONFIG_HOME", home + "/.config") +
           "/fcitx5/qmlpanel/colors.json";
}

// Empreinte (somme des mtimes) des fichiers de couleurs → détecte un changement
// de thème pour relire les couleurs à chaud.
qint64 colorsStamp() {
    qint64 s = 0;
    for (const QString &p : {dmsColorsPath(), overrideColorsPath()}) {
        QFileInfo fi(p);
        if (fi.exists())
            s += fi.lastModified().toMSecsSinceEpoch();
    }
    return s;
}

// Palette = défauts Catppuccin → écrasés par DMS (matugen) → écrasés par un
// override explicite. Material You : accent = primary, surface = container.
// Mode CLAIR/SOMBRE : "mode" explicite dans l'override > détection DMS (dans
// dank16, la valeur "default" de chaque couleur suit le mode courant) > sombre.
QVariantMap loadColors() {
    QJsonObject root = readJson(dmsColorsPath());
    QJsonObject ov = readJson(overrideColorsPath());
    bool dark = true;
    QString mode = ov.value("mode").toString();
    if (mode == "light" || mode == "dark") {
        dark = (mode == "dark");
    } else {
        QJsonObject d16 = root.value("dank16").toObject();
        for (const QString &k : d16.keys()) {
            QJsonObject col = d16.value(k).toObject();
            QString dv = col.value("dark").toString();
            QString lv = col.value("light").toString();
            if (!dv.isEmpty() && dv != lv) {
                dark = (col.value("default").toString() == dv);
                break;
            }
        }
    }

    // défauts Catppuccin (mocha/latte) selon le mode détecté
    QVariantMap c;
    c["surface"] = dark ? "#1e1e2e" : "#eff1f5";
    c["onSurface"] = dark ? "#cdd6f4" : "#4c4f69";
    c["accent"] = dark ? "#89b4fa" : "#1e66f5";
    c["onAccent"] = dark ? "#11111b" : "#ffffff";
    c["outline"] = dark ? "#45455a" : "#bcc0cc";
    // rôles Material 3 du picker emoji : fond du champ de recherche, texte
    // secondaire (placeholder, aide clavier), case sélectionnée.
    c["surfaceVariant"] = dark ? "#313244" : "#ccd0da";
    c["onSurfaceVariant"] = dark ? "#a6adc8" : "#6c6f85";
    c["secondaryContainer"] = dark ? "#585b70" : "#acb0be";

    // 1) couleurs DMS/matugen (régénérées à chaque changement de thème)
    QJsonObject d = root.value("colors").toObject()
                        .value(dark ? "dark" : "light").toObject();
    if (!d.isEmpty()) {
        auto pick = [&](std::initializer_list<const char *> keys) -> QString {
            for (auto *k : keys)
                if (d.contains(k))
                    return d.value(k).toString();
            return {};
        };
        QString surface = pick({"surface_container_high", "surface_container",
                                "surface_bright", "surface"});
        QString onSurface = pick({"on_surface"});
        QString accent = pick({"primary"});
        QString onAccent = pick({"on_primary"});
        QString outline = pick({"outline_variant", "outline"});
        QString surfaceVariant = pick({"surface_container_highest",
                                       "surface_variant", "surface_container"});
        QString onSurfaceVariant = pick({"on_surface_variant"});
        QString secondaryContainer = pick({"secondary_container",
                                           "surface_container_highest"});
        if (!surface.isEmpty()) c["surface"] = surface;
        if (!onSurface.isEmpty()) c["onSurface"] = onSurface;
        if (!accent.isEmpty()) c["accent"] = accent;
        if (!onAccent.isEmpty()) c["onAccent"] = onAccent;
        if (!outline.isEmpty()) c["outline"] = outline;
        if (!surfaceVariant.isEmpty()) c["surfaceVariant"] = surfaceVariant;
        if (!onSurfaceVariant.isEmpty())
            c["onSurfaceVariant"] = onSurfaceVariant;
        if (!secondaryContainer.isEmpty())
            c["secondaryContainer"] = secondaryContainer;
    }
    // 2) override explicite (mêmes clés + "mode")
    for (const QString &k :
         {"surface", "onSurface", "accent", "onAccent", "outline",
          "surfaceVariant", "onSurfaceVariant", "secondaryContainer"})
        if (ov.contains(k))
            c[k] = ov.value(k).toString();
    return c;
}

void ensureApp() {
    if (qGuiApp) {
        return;
    }
    // Plateforme offscreen + chemins plugins/QML bakés (cf flake) AVANT
    // QGuiApplication : fcitx n'est pas une appli Qt.
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef QMLPANEL_QT_PLUGIN_PATH
    qputenv("QT_PLUGIN_PATH", QMLPANEL_QT_PLUGIN_PATH);
#endif
#ifdef QMLPANEL_QML_IMPORT_PATH
    qputenv("QML_IMPORT_PATH", QMLPANEL_QML_IMPORT_PATH);
    qputenv("QML2_IMPORT_PATH", QMLPANEL_QML_IMPORT_PATH);
#endif
    static int argc = 1;
    static char arg0[] = "fcitx5-qmlpanel";
    static char *argv[] = {arg0, nullptr};
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    new QGuiApplication(argc, argv);
}

// Un candidat est-il un emoji (fonte couleur, corps plus grand) ? Détection
// par CONTENU, pas par premier point de code : les keycaps (« 1️⃣ » commence
// par '1'), ©️/®️ (préfixe latin-1) et ‼️/⁉️ n'étaient pas reconnus et se
// rendaient en tofu Maple Mono. VS16 (FE0F), ZWJ (200D) et keycap (20E3)
// n'apparaissent jamais dans un mot ordinaire.
bool looksEmoji(const QString &s) {
    for (char32_t cp : s.toUcs4()) {
        if (cp >= 0x1F000 || cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 ||
            (cp >= 0x2190 && cp <= 0x2BFF))
            return true;
    }
    return false;
}

// ease-out cubic : départ vif, arrivée douce — le bon feel pour une popup.
double easeOutCubic(double t) {
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

// QMLPANEL_ANIM_SCALE (float, défaut 1) : étire les durées d'animation — pour
// les capturer de façon déterministe en test headless (et déboguer le feel).
int animMs(int ms) {
    static const double scale = [] {
        QByteArray v = qgetenv("QMLPANEL_ANIM_SCALE");
        bool ok = false;
        double s = v.toDouble(&ok);
        return ok && s > 0 ? s : 1.0;
    }();
    return int(ms * scale);
}
} // namespace

double PanelView::Anim::at(Clock::time_point now) const {
    if (durMs <= 0)
        return to;
    double t = std::chrono::duration<double, std::milli>(now - start).count() /
               double(durMs);
    t = std::clamp(t, 0.0, 1.0);
    return from + (to - from) * easeOutCubic(t);
}

bool PanelView::Anim::done(Clock::time_point now) const {
    return durMs <= 0 ||
           now - start >= std::chrono::milliseconds(durMs);
}

PanelView::PanelView() {
    ensureApp();
    view_ = new QQuickView();
    view_->setColor(Qt::transparent); // coins arrondis transparents
    view_->setResizeMode(QQuickView::SizeViewToRootObject);

    auto *ctx = view_->engine()->rootContext();
    ctx->setContextProperty("colors", loadColors());
    colorsStamp_ = colorsStamp();
    ctx->setContextProperty("candidates", QStringList{});
    ctx->setContextProperty("highlight", -1);
    ctx->setContextProperty("hlFrom", -1);
    ctx->setContextProperty("hlTo", -1);
    ctx->setContextProperty("hlT", 1.0);
    ctx->setContextProperty("appear", 1.0);
    ctx->setContextProperty("contentFade", 1.0);
    ctx->setContextProperty("autoMark", QVariantList{});
    ctx->setContextProperty("emojiMark", QVariantList{});
    ctx->setContextProperty("composing", false);
    ctx->setContextProperty("gridMode", false);
    ctx->setContextProperty("labels", QStringList{});
    ctx->setContextProperty("listMode", false);
    ctx->setContextProperty("loading", false);
    ctx->setContextProperty("spin", 0.0);
    ctx->setContextProperty("headerText", QString{});
    ctx->setContextProperty("searchText", QString{});

    component_ = new QQmlComponent(view_->engine());
    component_->setData(kPanelQml, QUrl());
    auto *obj = component_->create(ctx);
    root_ = qobject_cast<QQuickItem *>(obj);
    if (!root_) {
        qWarning("qmlpanel: QML error: %s",
                 component_->errorString().toUtf8().constData());
        return;
    }
    view_->setContent(QUrl(), component_, root_);
    view_->show(); // offscreen → invisible, mais expose la fenêtre pour grab
}

PanelView::~PanelView() {
    delete view_; // détruit aussi le contenu QML
}

void PanelView::setColors(const QVariantMap &colors) {
    if (view_) {
        view_->engine()->rootContext()->setContextProperty("colors", colors);
    }
}

void PanelView::update(const QStringList &candidates, int highlight,
                       const QVariantList &autoMark, bool composing,
                       bool grid, const QStringList &labels, bool listMode,
                       bool loading, const QString &headerText,
                       const QString &searchText) {
    auto now = Clock::now();
    if (loading && !loading_)
        spinStart_ = now; // (re)démarre la rotation du spinner
    if (!shown_) {
        // si un fondu de fermeture court encore, on repart de l'opacité
        // COURANTE (durée au prorata du chemin restant) — sinon la barre
        // CLIGNOTAIT : retombée à 0 puis remontée, visible dès qu'un mot
        // suit une ponctuation en moins de 90 ms.
        double from = hiding_ ? hide_.at(now) : 0.0;
        appear_ = {from, 1.0, now, int(animMs(140) * (1.0 - from))};
        shown_ = true;
    }
    hiding_ = false; // un nouveau contenu annule un fondu de fermeture en cours
    // Refresh de la barre PASSIVE (mot-suivant asynchrone : le neural remplace
    // la liste n-gram déjà affichée) : petit fondu de contenu pour éviter le
    // « pop » — jamais pendant la composition (la frappe doit rester crue).
    if (shown_ && !composing && !cands_.isEmpty() && candidates != cands_ &&
        appear_.done(now))
        fade_ = {0.35, 1.0, now, animMs(80)};
    composing_ = composing;
    // Surlignage : on anime une PROGRESSION 0→1 entre la case de DÉPART et
    // celle d'ARRIVÉE, pas un index flottant. Avec un index flottant, un saut
    // de ligne (±8 dans la grille) faisait défiler le pill par toutes les
    // cases intermédiaires — il traversait l'écran de gauche à droite au lieu
    // de descendre. Ici QML interpole directement entre deux cases : le
    // déplacement est une droite, y compris en diagonale.
    if (candidates != cands_ || highlight < 0 || highlight_ < 0) {
        // nouveau contenu (frappe) ou pas de surlignage de départ : pas de
        // morph — le pill saute (ou disparaît), seul Tab→Tab anime.
        hlFrom_ = hlTo_ = highlight;
        hl_ = {1.0, 1.0, now, 0};
    } else if (highlight != highlight_) {
        // morph interrompu en vol : on repart de la case VISÉE (110 ms, la
        // reprise ne se voit pas) plutôt que de plomber le code avec les
        // positions en pixels, que seul QML connaît.
        hlFrom_ = hl_.done(now) ? hlTo_ : hlTo_;
        hlTo_ = highlight;
        hl_ = {0.0, 1.0, now, animMs(110)};
    }
    if (candidates != cands_) {
        emojiMark_.clear();
        for (const QString &c : candidates)
            emojiMark_ << looksEmoji(c);
    }
    cands_ = candidates;
    highlight_ = highlight;
    autoMark_ = autoMark;
    grid_ = grid;
    labels_ = labels;
    listMode_ = listMode;
    loading_ = loading;
    headerText_ = headerText;
    searchText_ = searchText;
}

int PanelView::hideGraceMs() const { return animMs(90) + 80; }

bool PanelView::startHide() {
    if (!shown_ || hiding_)
        return hiding_;
    auto now = Clock::now();
    hide_ = {appear_.at(now), 0.0, now, animMs(90)};
    shown_ = false;
    hiding_ = true;
    return true;
}

bool PanelView::hideDone() const {
    return hiding_ && hide_.done(Clock::now());
}

void PanelView::hidden() {
    shown_ = false;
    hiding_ = false;
    highlight_ = -1;
    cands_.clear();
    emojiMark_.clear();
    labels_.clear();
    listMode_ = false;
    loading_ = false;
    headerText_.clear();
    searchText_.clear();
    hl_ = {};
    hlFrom_ = hlTo_ = -1;
    fade_ = {1.0, 1.0, {}, 0};
}

bool PanelView::animating() const {
    auto now = Clock::now();
    if (hiding_)
        return !hide_.done(now);
    // le spinner de chargement tourne en continu → garder la boucle de frames
    // vivante tant que la génération n'est pas finie.
    if (shown_ && loading_)
        return true;
    return shown_ &&
           (!appear_.done(now) || !hl_.done(now) || !fade_.done(now));
}

QImage PanelView::render() {
    if (!root_) {
        return {};
    }
    auto now = Clock::now();
    auto *ctx = view_->engine()->rootContext();
    // relit les couleurs si le thème (matugen/DMS) a changé
    qint64 s = colorsStamp();
    if (s != colorsStamp_) {
        colorsStamp_ = s;
        ctx->setContextProperty("colors", loadColors());
    }
    ctx->setContextProperty("candidates", cands_);
    ctx->setContextProperty("highlight", highlight_);
    ctx->setContextProperty("hlFrom", hlFrom_);
    ctx->setContextProperty("hlTo", hlTo_);
    ctx->setContextProperty("hlT", hl_.at(now));
    ctx->setContextProperty("appear",
                            hiding_ ? hide_.at(now) : appear_.at(now));
    ctx->setContextProperty("contentFade", fade_.at(now));
    ctx->setContextProperty("autoMark", autoMark_);
    ctx->setContextProperty("emojiMark", emojiMark_);
    ctx->setContextProperty("composing", composing_);
    ctx->setContextProperty("gridMode", grid_);
    ctx->setContextProperty("labels", labels_);
    ctx->setContextProperty("listMode", listMode_);
    ctx->setContextProperty("loading", loading_);
    ctx->setContextProperty("headerText", headerText_);
    ctx->setContextProperty("searchText", searchText_);
    // rotation du spinner : ~1 tour / 0,9 s tant que loading_
    double spin = 0.0;
    if (loading_) {
        double ms = std::chrono::duration<double, std::milli>(now - spinStart_)
                        .count();
        spin = std::fmod(ms / 900.0 * 360.0, 360.0);
    }
    ctx->setContextProperty("spin", spin);
    QCoreApplication::processEvents(); // laisse bindings + resize se résoudre
    QImage img = view_->grabWindow();
    // PREMIER rendu après un changement de taille du contenu (barre → grille
    // emoji) : la fenêtre (SizeViewToRootObject) n'a pas encore suivi la
    // nouvelle taille implicite du root — celle-ci n'est résolue que par le
    // polish déclenché PAR le grab. Résultat : grille clipée à la hauteur de
    // la barre, et si aucune animation ne court, aucune frame suivante ne
    // corrige (bug visible à la 1re ouverture du picker emoji). Si la taille
    // implicite diffère de la fenêtre : resize + re-grab, une fois.
    QSize want(int(std::ceil(root_->implicitWidth())),
               int(std::ceil(root_->implicitHeight())));
    if (want.width() > 0 && want.height() > 0 && want != view_->size()) {
        view_->resize(want);
        QCoreApplication::processEvents();
        img = view_->grabWindow();
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
