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
#include <QLocalSocket>
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
                { key: "accentRestore",        def: true,  label: "Restauration d'accents",
                  desc: "francais→français, oeuvre→œuvre sur Espace — n'ajoute que les signes, jamais un autre mot (marche même sans auto-correction)" },
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

// Saisie/validation de la clé Groq (dialogue --groq-key, lancé par l'engine
// depuis le panneau « Clé API requise »). La clé va dans le DATA dir
// (~/.local/share/ime-predictord/groq.key, 0600 — jamais dans le dépôt stow),
// puis le DAEMON la valide (op reformCheck = appel Groq minimal — il relit la
// clé à chaque appel, donc effet immédiat, aucun restart).
class KeyTool : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool save(const QString &key) {
        QString base = qEnvironmentVariable("XDG_DATA_HOME");
        if (base.isEmpty())
            base = QDir::homePath() + "/.local/share";
        const QString dir = base + "/ime-predictord";
        QDir().mkpath(dir);
        const QString p = dir + "/groq.key";
        QSaveFile f(p);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(key.trimmed().toUtf8());
        f.write("\n");
        if (!f.commit())
            return false;
        QFile::setPermissions(p, QFileDevice::ReadOwner |
                                     QFileDevice::WriteOwner);
        return true;
    }

    // "valid" | "invalid" (clé refusée) | "offline" (réseau) | "daemon"
    // (daemon injoignable). BLOQUANT (quelques secondes au pire) — le QML
    // l'appelle via un Timer pour que « Validation… » s'affiche d'abord.
    Q_INVOKABLE QString validate() {
        QString sock = qEnvironmentVariable("IME_PREDICTORD_SOCK");
        if (sock.isEmpty())
            sock = "/tmp/ime-predictord.sock";
        QLocalSocket s;
        s.connectToServer(sock);
        if (!s.waitForConnected(1500))
            return "daemon";
        s.write("{\"reformCheck\":true}\n");
        s.flush();
        QByteArray buf;
        while (!buf.contains('\n')) {
            if (!s.waitForReadyRead(10000))
                return "daemon";
            buf += s.readAll();
        }
        const QJsonObject r =
            QJsonDocument::fromJson(buf.left(buf.indexOf('\n'))).object();
        if (r.value("keyValid").toBool())
            return "valid";
        return r.value("error").toString() == "network" ? "offline"
                                                        : "invalid";
    }
};

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

// Dialogue de SAISIE DE CLÉ (--groq-key) : un champ où COLLER fonctionne
// (impossible dans le panneau de l'IME — Ctrl+V va à l'appli), validation
// AUTOMATIQUE à l'enregistrement (via le daemon), fermeture auto si valide.
static const char *kGroqKeyQml = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: win
    visible: true
    title: "IME — Clé API Groq"
    color: colors.surface
    width: 480
    height: col.implicitHeight + 40
    minimumWidth: 480
    minimumHeight: col.implicitHeight + 40

    function go() {
        var k = field.text.trim()
        if (!k.length) { status.text = "La clé est vide."; return }
        if (!keytool.save(k)) { status.text = "✗ Écriture impossible."; return }
        status.text = "Validation en cours…"
        checkTimer.start()
    }

    Column {
        id: col
        x: 20; y: 20
        width: parent.width - 40
        spacing: 12

        Text {
            text: "Clé API Groq (reformulation)"
            color: colors.onSurface
            font.bold: true; font.pixelSize: 15; font.family: "Maple Mono NF"
        }
        Text {
            text: "Colle ta clé (console.groq.com → API Keys) puis Entrée. " +
                  "Elle est stockée en local, hors de tout dépôt " +
                  "(~/.local/share/ime-predictord/groq.key)."
            color: colors.onSurface; opacity: 0.55
            font.pixelSize: 11; font.family: "Maple Mono NF"
            width: parent.width; wrapMode: Text.Wrap
        }
        Rectangle {
            width: parent.width; height: 36; radius: 8
            color: "transparent"
            border.color: field.activeFocus ? colors.accent : colors.outline
            border.width: 1
            TextInput {
                id: field
                anchors.fill: parent; anchors.margins: 9
                color: colors.onSurface
                font.pixelSize: 12; font.family: "Maple Mono NF"
                selectByMouse: true
                clip: true
                focus: true
                onAccepted: win.go()
            }
        }
        Row {
            spacing: 10
            Rectangle {
                width: btnLabel.implicitWidth + 24; height: 32; radius: 8
                color: colors.accent
                Text {
                    id: btnLabel
                    anchors.centerIn: parent
                    text: "Valider et enregistrer"
                    color: colors.surface
                    font.pixelSize: 12; font.family: "Maple Mono NF"
                }
                MouseArea { anchors.fill: parent; onClicked: win.go() }
            }
            Text {
                id: status
                anchors.verticalCenter: parent.verticalCenter
                text: ""
                color: colors.onSurface
                font.pixelSize: 12; font.family: "Maple Mono NF"
            }
        }
    }

    Timer { // laisse « Validation en cours… » se PEINDRE avant l'appel bloquant
        id: checkTimer
        interval: 60
        onTriggered: {
            var r = keytool.validate()
            if (r === "valid") {
                status.text = "✓ Clé valide — la reformulation est prête !"
                closeTimer.start()
            } else if (r === "invalid") {
                status.text = "✗ Clé refusée par l'API — vérifie-la."
            } else if (r === "offline") {
                status.text = "⚠ Réseau injoignable — clé enregistrée, elle sera utilisée dès le retour du réseau."
            } else {
                status.text = "⚠ Daemon injoignable — clé enregistrée."
            }
        }
    }
    Timer { id: closeTimer; interval: 1400; onTriggered: Qt.quit() }
}
)QML";

int main(int argc, char **argv) {
    QGuiApplication::setDesktopFileName("ime-preferences");
    QGuiApplication app(argc, argv);

    Cfg cfg(configPath());

    // mode CLI : --set k=v (répétable) applique et sort, sans UI.
    bool smoke = false, didSet = false, groqKey = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "--smoke") {
            smoke = true;
        } else if (args[i] == "--groq-key") {
            groqKey = true;
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

    if (groqKey) { // dialogue de clé, sans le popover complet
        QQmlApplicationEngine engine;
        KeyTool keytool;
        engine.rootContext()->setContextProperty("keytool", &keytool);
        engine.rootContext()->setContextProperty("colors", loadColors());
        engine.loadData(kGroqKeyQml);
        if (engine.rootObjects().isEmpty()) {
            fprintf(stderr, "ime-preferences: QML clé sans objet racine\n");
            return 1;
        }
        if (smoke)
            QTimer::singleShot(400, &app, &QGuiApplication::quit);
        return app.exec();
    }

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
