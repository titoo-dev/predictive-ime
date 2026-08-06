// Rendu HORS LIGNE de la barre : instancie le VRAI PanelView (donc le vrai
// QML) et écrit quelques états en PNG, sans compositeur ni session graphique.
//
// Deux rôles :
//  • test de non-régression — une erreur QML rend le composant nul, et un
//    panneau nul avale les frappes (cf le commentaire font.families dans
//    panelview.cpp). Ici ça sort en échec au lieu de casser la saisie.
//  • relecture visuelle du design (picker emoji, barre de mots) : les PNG se
//    regardent, ce qu'un test de comportement ne donne pas.
//
// Usage : panel_preview [dossier de sortie]
#include <chrono>
#include <cstdio>
#include <thread>

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QStringList>
#include <QVariantList>

#include "panelview.h"

namespace {
int failures = 0;

// Animations ralenties ×20 (même levier que test-ui.sh) : c'est ce qui rend
// une frame INTERMÉDIAIRE capturable de façon fiable, malgré le coût du grab.
constexpr int kAnimScale = 20;

// Rend un état APRÈS son animation d'apparition (140 ms × échelle) : le
// premier frame est presque transparent, il ne montrerait rien.
QImage settle(PanelView &v) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(140 * kAnimScale + 300));
    return v.render();
}

void shot(PanelView &v, const QString &dir, const char *name, int minW,
          int minH) {
    QImage img = settle(v);
    QString path = dir + "/" + name + ".png";
    bool ok = !img.isNull() && img.width() >= minW && img.height() >= minH &&
              img.save(path);
    printf("%s %-22s %dx%d\n", ok ? "  ok  " : " FAIL ", name,
           img.width(), img.height());
    if (!ok)
        ++failures;
}

// Le panneau est du VERRE (fond translucide) : dans un PNG à fond transparent,
// son alpha se lit en damier ou en noir et le style acrylique n'est pas
// jugeable. On le compose donc sur un faux fond d'application (dégradé + lignes
// de texte) — c'est ce que le compositeur montrera, en plus flouté s'il a
// `decoration:blur:input_methods`.
QImage onBackdrop(const QImage &panel) {
    QImage bg(panel.width() + 140, panel.height() + 110,
              QImage::Format_ARGB32_Premultiplied);
    QPainter p(&bg);
    QLinearGradient g(0, 0, bg.width(), bg.height());
    g.setColorAt(0.0, QColor("#20364a"));
    g.setColorAt(0.5, QColor("#4c3a5c"));
    g.setColorAt(1.0, QColor("#7d5a3c"));
    p.fillRect(bg.rect(), g);
    p.setPen(QColor(255, 255, 255, 170));
    p.setFont(QFont(QStringLiteral("DejaVu Sans"), 10));
    for (int i = 0; i * 21 < bg.height(); i++)
        p.drawText(10, 20 + i * 21,
                   QStringLiteral("le texte de l'application passe DERRIÈRE le "
                                  "panneau — lisibilité du verre"));
    p.drawImage(70, 55, panel);
    p.end();
    return bg;
}

void shotOnBackdrop(PanelView &v, const QString &dir, const char *name) {
    QImage img = onBackdrop(settle(v));
    QString path = dir + "/" + name + ".png";
    bool ok = !img.isNull() && img.save(path);
    printf("%s %-22s %dx%d\n", ok ? "  ok  " : " FAIL ", name, img.width(),
           img.height());
    if (!ok)
        ++failures;
}

QStringList emojis(int n) {
    static const char *pool[] = {"😀", "😍", "🎉", "❤️", "🔥", "👍", "🙏",
                                 "😂", "🥰", "✨",  "💯", "🤔", "😭", "🚀",
                                 "☀️", "🌙", "🍕", "🐈", "🎧", "📚", "⚡",
                                 "🌈", "🍀", "🥲"};
    QStringList out;
    for (int i = 0; i < n; i++)
        out << pool[i % 24];
    return out;
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QMLPANEL_ANIM_SCALE", QByteArray::number(kAnimScale));
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : QStringLiteral(".");
    PanelView v;
    if (!v.ok()) {
        fprintf(stderr, " FAIL  QML non chargé (composant nul)\n");
        return 1;
    }

    // 1) picker nu : favoris + populaires, aucune requête, rien de surligné
    QVariantList autoMark;
    autoMark << true;
    for (int i = 1; i < 24; i++)
        autoMark << false;
    v.update(emojis(24), -1, autoMark, /*composing=*/true, /*grid=*/true, {},
             false, false, {}, QString{});
    shot(v, dir, "picker-nu", 260, 130);

    // 2) picker avec requête + case surlignée (2e ligne)
    v.update(emojis(14), 9, {}, true, true, {}, false, false,
             QStringLiteral("2/4"), QStringLiteral("coeur"));
    shot(v, dir, "picker-recherche", 260, 110);

    // 2bis) FRAME EN PLEIN MORPH : le surlignage passe de la case 0 à la case
    // 9 (ligne du dessous, colonne 2). Le pill doit être EN DIAGONALE à
    // mi-parcours ; s'il longe la 1re ligne, c'est qu'on interpole encore un
    // index flottant (le défaut corrigé : « ça part à droite quand je descends »).
    v.update(emojis(24), 0, {}, true, true, {}, false, false, {},
             QStringLiteral("coeur"));
    settle(v);
    v.update(emojis(24), 9, {}, true, true, {}, false, false, {},
             QStringLiteral("coeur"));
    // ~1/5 du trajet en temps brut → à peu près la moitié une fois l'ease-out
    // appliqué : le pill est visiblement ENTRE les deux cases.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(110 * kAnimScale / 5));
    {
        QImage mid = v.render();
        bool ok = !mid.isNull() && mid.save(dir + "/picker-morph-mid.png");
        printf("%s %-22s %dx%d\n", ok ? "  ok  " : " FAIL ",
               "picker-morph-mid", mid.width(), mid.height());
        if (!ok)
            ++failures;
    }

    // 3) recherche sans résultat : le picker reste ouvert et le dit
    v.update({}, -1, {}, true, true, {}, false, false, {},
             QStringLiteral("zzqx"));
    shot(v, dir, "picker-vide", 260, 90);

    // 4) non-régression : la barre de mots ne bouge pas
    v.update({"bonjour", "bonsoir", "bon"}, 1, {}, true, false);
    shot(v, dir, "barre-mots", 120, 25);

    // 5) VERRE : les deux états sur un fond d'application, pour juger le style
    //    acrylique (translucidité, reflet, arête, halo) et la lisibilité.
    v.update(emojis(24), 3, autoMark, true, true, {}, false, false,
             QStringLiteral("1/4"), QStringLiteral("coeur"));
    shotOnBackdrop(v, dir, "verre-picker");
    v.update({"bonjour", "bonsoir", "bon"}, 1, {}, true, false);
    shotOnBackdrop(v, dir, "verre-barre-mots");

    // 6) THÈME CLAIR : même verre, palette claire (QMLPANEL_MODE force le mode
    //    — sans ça il faudrait basculer le thème DMS pour relire ce rendu). Un
    //    second PanelView : les couleurs sont lues à la construction.
    qputenv("QMLPANEL_MODE", "light");
    PanelView light;
    if (!light.ok()) {
        fprintf(stderr, " FAIL  QML non chargé en thème clair\n");
        return 1;
    }
    light.update(emojis(24), 3, autoMark, true, true, {}, false, false,
                 QStringLiteral("1/4"), QStringLiteral("coeur"));
    shotOnBackdrop(light, dir, "verre-picker-clair");

    printf(failures ? "\n%d rendu(s) en échec\n" : "\ntous les rendus OK\n",
           failures);
    return failures ? 1 : 0;
}
