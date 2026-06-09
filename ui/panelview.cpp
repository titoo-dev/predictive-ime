#include "panelview.h"

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

// QML par défaut : chips horizontales (façon Gboard), pill accent sur le
// candidat surligné. Couleurs via le context property `colors` (matugen, cf
// qmlui.cpp). Police = Maple Mono NF (système).
static const char *kPanelQml = R"QML(
import QtQuick

Rectangle {
    id: root
    color: colors.surface
    radius: 16
    implicitWidth: Math.max(row.width + 14, 40)
    implicitHeight: 46

    Row {
        id: row
        x: 7
        spacing: 4
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            model: candidates
            delegate: Rectangle {
                required property int index
                required property string modelData
                radius: 11
                color: index === highlight ? colors.accent : "transparent"
                implicitWidth: label.implicitWidth + 22
                height: 34
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData
                    color: index === highlight ? colors.onAccent : colors.onSurface
                    font.pixelSize: 17
                    font.family: "Maple Mono NF"
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
QVariantMap loadColors() {
    QVariantMap c;
    c["surface"] = "#1e1e2e";
    c["onSurface"] = "#cdd6f4";
    c["accent"] = "#89b4fa";
    c["onAccent"] = "#11111b";

    // 1) couleurs DMS/matugen (régénérées à chaque changement de thème)
    QJsonObject d =
        readJson(dmsColorsPath()).value("colors").toObject().value("dark").toObject();
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
        if (!surface.isEmpty()) c["surface"] = surface;
        if (!onSurface.isEmpty()) c["onSurface"] = onSurface;
        if (!accent.isEmpty()) c["accent"] = accent;
        if (!onAccent.isEmpty()) c["onAccent"] = onAccent;
    }
    // 2) override explicite {surface,onSurface,accent,onAccent}
    QJsonObject o = readJson(overrideColorsPath());
    for (const QString &k : {"surface", "onSurface", "accent", "onAccent"})
        if (o.contains(k))
            c[k] = o.value(k).toString();
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
} // namespace

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
    ctx->setContextProperty("preeditText", QString());

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

QImage PanelView::render(const QStringList &candidates, int highlight,
                         const QString &preedit) {
    if (!root_) {
        return {};
    }
    auto *ctx = view_->engine()->rootContext();
    // relit les couleurs si le thème (matugen/DMS) a changé
    qint64 s = colorsStamp();
    if (s != colorsStamp_) {
        colorsStamp_ = s;
        ctx->setContextProperty("colors", loadColors());
    }
    ctx->setContextProperty("candidates", candidates);
    ctx->setContextProperty("highlight", highlight);
    ctx->setContextProperty("preeditText", preedit);
    QCoreApplication::processEvents(); // laisse bindings + resize se résoudre
    QImage img = view_->grabWindow();
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
