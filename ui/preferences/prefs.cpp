// ime-preferences — popover de réglages de l'IME prédictif.
//
// Lit/écrit $XDG_CONFIG_HOME/ime-predictord/config.json — le daemon ET
// l'engine rechargent ce fichier À CHAUD (mtime) : chaque bascule est
// appliquée immédiatement, pas de bouton OK. Les clés inconnues du fichier
// sont préservées (on ne réécrit que ce qu'on change).
//
// La LANGUE des suggestions se choisit ici : fr / en (déterministe, aucune
// détection), auto (vote du contexte — l'ancien comportement), off (aucun
// boost). Couleurs DMS/matugen comme la barre de candidats.
//
// CLI : `ime-preferences --set lang=fr [--set ghostText=false ...]` applique
// sans ouvrir l'UI (scriptable) ; `--smoke` ouvre et quitte (test headless).
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSaveFile>
#include <QTimer>
#include <QVariant>

static const char *kPrefsQml = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: win
    visible: true
    title: "IME — Préférences"
    color: colors.surface
    width: 400
    height: col.implicitHeight + 40
    minimumWidth: 400
    minimumHeight: col.implicitHeight + 40
    maximumWidth: 400
    maximumHeight: col.implicitHeight + 40

    property string lang: cfg.get("lang", "auto")

    Column {
        id: col
        x: 20
        y: 20
        width: parent.width - 40
        spacing: 12

        Text {
            text: "Prédiction — préférences"
            color: colors.onSurface
            font.bold: true
            font.pixelSize: 15
            font.family: "Maple Mono NF"
        }

        // ---- Langue des suggestions (remplace l'auto-détection) ----
        Column {
            width: parent.width
            spacing: 7
            Text {
                text: "Langue des suggestions"
                color: colors.onSurface
                font.pixelSize: 13
                font.family: "Maple Mono NF"
            }
            Text {
                text: "Choisie ici — fini la détection : déterministe quel que soit le contexte"
                color: colors.onSurface
                opacity: 0.55
                font.pixelSize: 11
                font.family: "Maple Mono NF"
                width: parent.width
                wrapMode: Text.Wrap
            }
            Row {
                spacing: 6
                Repeater {
                    model: [
                        { key: "fr",   label: "Français" },
                        { key: "en",   label: "English" },
                        { key: "auto", label: "Auto" },
                        { key: "off",  label: "Aucune" }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool sel: win.lang === modelData.key
                        width: chipLabel.implicitWidth + 22
                        height: 28
                        radius: 9
                        color: sel ? colors.accent : "transparent"
                        border.width: 1
                        border.color: sel ? colors.accent : colors.outline
                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: sel ? colors.onAccent : colors.onSurface
                            font.pixelSize: 12
                            font.family: "Maple Mono NF"
                        }
                        TapHandler {
                            onTapped: {
                                win.lang = modelData.key
                                cfg.set("lang", modelData.key)
                            }
                        }
                    }
                }
            }
        }

        Rectangle { width: parent.width; height: 1; color: colors.outline; opacity: 0.5 }

        // ---- Interrupteurs (appliqués à la volée) ----
        Repeater {
            model: [
                { key: "autoApply",            def: true,  label: "Espace complète / corrige",
                  desc: "le candidat au liseré accent remplace le mot tapé" },
                { key: "autoApplyNeedsRevert", def: true,  label: "… seulement si annulable",
                  desc: "exige le surrounding-text (revert Backspace possible)" },
                { key: "nextWordBar",          def: true,  label: "Barre mot-suivant",
                  desc: "suggestions spéculatives après Espace — off = mode calme" },
                { key: "ghostText",            def: true,  label: "Texte fantôme",
                  desc: "le reste du mot s'affiche dans le préedit (bonjou‸r)" },
                { key: "multiWord",            def: true,  label: "Expressions multi-mots",
                  desc: "« sais pas » proposé en fin de barre" },
                { key: "escapeForward",        def: true,  label: "Échap traverse",
                  desc: "atteint l'application après avoir fermé la barre (vim)" },
                { key: "frenchSpacing",        def: false, label: "Espace fine avant ; : ! ?",
                  desc: "typographie française (U+202F insécable)" },
                { key: "autoCapitalize",       def: false, label: "Majuscule de phrase",
                  desc: "capitalise automatiquement en début de phrase" }
            ]
            delegate: Item {
                required property var modelData
                width: col.width
                height: Math.max(38, optText.implicitHeight + 8)
                Column {
                    id: optText
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 56
                    spacing: 1
                    Text {
                        text: modelData.label
                        color: colors.onSurface
                        font.pixelSize: 13
                        font.family: "Maple Mono NF"
                    }
                    Text {
                        text: modelData.desc
                        color: colors.onSurface
                        opacity: 0.55
                        font.pixelSize: 11
                        font.family: "Maple Mono NF"
                        width: parent.width
                        wrapMode: Text.Wrap
                    }
                }
                Rectangle {
                    id: tog
                    property bool on: cfg.get(modelData.key, modelData.def)
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 40
                    height: 22
                    radius: 11
                    color: on ? colors.accent : "transparent"
                    border.width: 1
                    border.color: on ? colors.accent : colors.outline
                    Rectangle {
                        x: tog.on ? parent.width - width - 3 : 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 8
                        color: tog.on ? colors.onAccent : colors.outline
                        Behavior on x {
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }
                    }
                    TapHandler {
                        onTapped: {
                            tog.on = !tog.on
                            cfg.set(modelData.key, tog.on)
                        }
                    }
                }
            }
        }

        Text {
            text: "Appliqué à la volée — daemon et engine rechargent à chaud."
            color: colors.onSurface
            opacity: 0.45
            font.pixelSize: 10
            font.family: "Maple Mono NF"
            width: parent.width
            wrapMode: Text.Wrap
        }
    }
}
)QML";

namespace {

QString configPath() {
    QString home = qEnvironmentVariable("HOME");
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config");
    return base + "/ime-predictord/config.json";
}

// Palette : défauts Catppuccin → écrasés par DMS (matugen) → override explicite
// — même logique que la barre de candidats (panelview.cpp), version compacte.
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

QVariantMap loadColors() {
    QString home = qEnvironmentVariable("HOME");
    QJsonObject root =
        readJson(qEnvironmentVariable("XDG_CACHE_HOME", home + "/.cache") +
                 "/DankMaterialShell/dms-colors.json");
    QJsonObject ov =
        readJson(qEnvironmentVariable("XDG_CONFIG_HOME", home + "/.config") +
                 "/fcitx5/qmlpanel/colors.json");
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
    QVariantMap c;
    c["surface"] = dark ? "#1e1e2e" : "#eff1f5";
    c["onSurface"] = dark ? "#cdd6f4" : "#4c4f69";
    c["accent"] = dark ? "#89b4fa" : "#1e66f5";
    c["onAccent"] = dark ? "#11111b" : "#ffffff";
    c["outline"] = dark ? "#45455a" : "#bcc0cc";
    QJsonObject d = root.value("colors").toObject()
                        .value(dark ? "dark" : "light").toObject();
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
    for (const QString &k :
         {"surface", "onSurface", "accent", "onAccent", "outline"})
        if (ov.contains(k))
            c[k] = ov.value(k).toString();
    return c;
}

// "true"/"false" → bool, nombre → double, sinon chaîne (pour --set k=v).
QVariant parseValue(const QString &raw) {
    if (raw == "true")
        return true;
    if (raw == "false")
        return false;
    bool ok = false;
    double d = raw.toDouble(&ok);
    if (ok)
        return d;
    return raw;
}

} // namespace

class Cfg : public QObject {
    Q_OBJECT
public:
    explicit Cfg(QString path) : path_(std::move(path)) {
        // ÉCRIRE À TRAVERS un lien symbolique (config stowée depuis les
        // dotfiles) : QSaveFile fait un rename qui remplacerait le lien par
        // un fichier — on résout d'abord la cible réelle.
        QFileInfo fi(path_);
        if (fi.isSymLink())
            path_ = fi.canonicalFilePath().isEmpty() ? fi.symLinkTarget()
                                                     : fi.canonicalFilePath();
        QFile f(path_);
        if (f.open(QIODevice::ReadOnly))
            obj_ = QJsonDocument::fromJson(f.readAll()).object();
    }

    Q_INVOKABLE QVariant get(const QString &key, const QVariant &def) const {
        return obj_.contains(key) ? obj_.value(key).toVariant() : def;
    }

    Q_INVOKABLE void set(const QString &key, const QVariant &value) {
        obj_.insert(key, QJsonValue::fromVariant(value));
        save();
    }

private:
    void save() {
        QFileInfo fi(path_);
        QDir().mkpath(fi.path());
        QSaveFile f(path_); // atomique : le daemon/engine relisent sur mtime
        if (!f.open(QIODevice::WriteOnly))
            return;
        f.write(QJsonDocument(obj_).toJson(QJsonDocument::Indented));
        f.commit();
    }

    QString path_;
    QJsonObject obj_;
};

int main(int argc, char **argv) {
    QGuiApplication::setDesktopFileName("ime-preferences");
    QGuiApplication app(argc, argv);

    Cfg cfg(configPath());

    // mode CLI : --set k=v (répétable) applique et sort, sans UI.
    bool smoke = false, didSet = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "--smoke") {
            smoke = true;
        } else if (args[i] == "--set" && i + 1 < args.size()) {
            const QString kv = args[++i];
            int eq = kv.indexOf('=');
            if (eq > 0) {
                cfg.set(kv.left(eq), parseValue(kv.mid(eq + 1)));
                didSet = true;
            }
        }
    }
    if (didSet && !smoke)
        return 0;

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError> &warnings) {
            for (const QQmlError &w : warnings)
                fprintf(stderr, "ime-preferences: %s\n",
                        w.toString().toUtf8().constData());
        });
    engine.rootContext()->setContextProperty("cfg", &cfg);
    engine.rootContext()->setContextProperty("colors", loadColors());
    engine.loadData(kPrefsQml);
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "ime-preferences: QML sans objet racine (erreur de "
                        "chargement ci-dessus ?)\n");
        return 1;
    }
    if (smoke)
        QTimer::singleShot(400, &app, &QGuiApplication::quit);
    return app.exec();
}

#include "prefs.moc"
