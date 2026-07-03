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

// QML : barre de chips horizontales COMPACTE (dense, façon Gboard) —
// surface arrondie + liseré, PILL accent qui GLISSE entre les candidats
// (hlPos flottant animé côté C++), apparition fade + slide-up (appear 0→1).
// Les emojis (picker ':') sont détectés et rendus plus grands avec la fonte
// couleur. Couleurs via la context property `colors` (matugen, cf qmlui.cpp).
static const char *kPanelQml = R"QML(
import QtQuick

Item {
    id: root
    implicitWidth: bar.width + 10
    implicitHeight: bar.height + 10

    Rectangle {
        id: bar
        x: 5
        y: 5 + (1 - appear) * 6        // slide-up à l'apparition
        opacity: appear
        width: row.width + 18
        height: row.height + 8         // 1 ligne = 34 ; grille emoji = 3 lignes
        radius: 11
        color: colors.surface
        border.width: 1
        border.color: colors.outline

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
            visible: hlPos >= 0 && rep.count > 0
            property real p: Math.max(0, Math.min(hlPos, rep.count - 1))
            property int i0: Math.floor(p)
            property real f: p - i0
            property Item a: rep.count > 0 ? rep.itemAt(i0) : null
            property Item b: rep.count > 0
                ? rep.itemAt(Math.min(i0 + 1, rep.count - 1)) : null
            x: a ? row.x + a.x + (b ? (b.x - a.x) * f : 0) : 0
            y: a ? row.y + a.y + (b ? (b.y - a.y) * f : 0) : 0
            width: a ? a.width + (b ? (b.width - a.width) * f : 0) : 0
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
            // grille emoji : 8 colonnes ; sinon une seule ligne
            columns: gridMode ? 8 : Math.max(1, rep.count)
            Repeater {
                id: rep
                model: candidates
                delegate: Item {
                    required property int index
                    required property string modelData
                    // décision C++ (cf looksEmoji) : l'heuristique « 1er point
                    // de code > 0x2100 » ratait les keycaps (1️⃣), ©️, ‼️…
                    readonly property bool emoji:
                        index < emojiMark.length && emojiMark[index] === true
                    readonly property bool isAuto:
                        index < autoMark.length && autoMark[index] === true
                    width: gridMode ? 30 : label.width + 16
                    height: 26
                    // liseré accent = « l'Espace appliquera CE candidat »
                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: colors.accent
                        opacity: 0.9
                        visible: isAuto && hlPos < 0
                    }
                    // indice 1-6 discret pendant la NAVIGATION : ces chiffres
                    // sélectionnent directement le candidat (découvrabilité)
                    Text {
                        visible: hlPos >= 0 && !gridMode && index < 6 && !emoji
                        text: index + 1
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.rightMargin: 3
                        color: index === Math.round(hlPos)
                               ? colors.onAccent : colors.onSurface
                        opacity: 0.45
                        font.pixelSize: 8
                        font.family: "Maple Mono NF"
                    }
                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: modelData
                        width: Math.min(implicitWidth, 190)
                        elide: Text.ElideRight
                        color: hlPos >= 0 && index === Math.round(hlPos)
                               ? colors.onAccent : colors.onSurface
                        font.pixelSize: emoji ? 17 : 14
                        // NB: font.families (plural, QStringList) is NOT a
                        // QML-assignable property of the font value type — it
                        // exists only on the C++ QFont. Qt 6.11's QML engine
                        // rejects it ("Cannot assign to non-existent property
                        // families"), so the whole panel component fails to load
                        // and keystrokes get swallowed. Use font.family (string);
                        // glyph fallback is handled by fontconfig anyway.
                        font.family: emoji ? "Noto Color Emoji" : "Maple Mono NF"
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
        if (!surface.isEmpty()) c["surface"] = surface;
        if (!onSurface.isEmpty()) c["onSurface"] = onSurface;
        if (!accent.isEmpty()) c["accent"] = accent;
        if (!onAccent.isEmpty()) c["onAccent"] = onAccent;
        if (!outline.isEmpty()) c["outline"] = outline;
    }
    // 2) override explicite {surface,onSurface,accent,onAccent,outline,mode}
    for (const QString &k :
         {"surface", "onSurface", "accent", "onAccent", "outline"})
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
    ctx->setContextProperty("hlPos", -1.0);
    ctx->setContextProperty("appear", 1.0);
    ctx->setContextProperty("contentFade", 1.0);
    ctx->setContextProperty("autoMark", QVariantList{});
    ctx->setContextProperty("emojiMark", QVariantList{});
    ctx->setContextProperty("composing", false);
    ctx->setContextProperty("gridMode", false);

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
                       bool grid) {
    auto now = Clock::now();
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
    if (candidates != cands_ || highlight < 0 || hl_.to < 0) {
        // nouveau contenu (frappe) ou pas de surlignage de départ : pas de
        // morph — le pill saute (ou disparaît), seul Tab→Tab anime.
        hl_ = {double(highlight), double(highlight), now, 0};
    } else if (highlight != highlight_) {
        hl_ = {hl_.at(now), double(highlight), now, animMs(110)};
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
    hl_ = {};
    hl_.to = -1.0;
    fade_ = {1.0, 1.0, {}, 0};
}

bool PanelView::animating() const {
    auto now = Clock::now();
    if (hiding_)
        return !hide_.done(now);
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
    ctx->setContextProperty("hlPos", hl_.at(now));
    ctx->setContextProperty("appear",
                            hiding_ ? hide_.at(now) : appear_.at(now));
    ctx->setContextProperty("contentFade", fade_.at(now));
    ctx->setContextProperty("autoMark", autoMark_);
    ctx->setContextProperty("emojiMark", emojiMark_);
    ctx->setContextProperty("composing", composing_);
    ctx->setContextProperty("gridMode", grid_);
    QCoreApplication::processEvents(); // laisse bindings + resize se résoudre
    QImage img = view_->grabWindow();
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
