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

#include <QImage>
#include <QStringList>
#include <QVariantList>

#include "panelview.h"

namespace {
int failures = 0;

// Rend un état APRÈS son animation d'apparition (140 ms) : le premier frame
// est presque transparent, il ne montrerait rien.
QImage settle(PanelView &v) {
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
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
    v.update(emojis(14), 9, {}, true, true, {}, false, false, {},
             QStringLiteral("coeur"));
    shot(v, dir, "picker-recherche", 260, 110);

    // 3) recherche sans résultat : le picker reste ouvert et le dit
    v.update({}, -1, {}, true, true, {}, false, false, {},
             QStringLiteral("zzqx"));
    shot(v, dir, "picker-vide", 260, 90);

    // 4) non-régression : la barre de mots ne bouge pas
    v.update({"bonjour", "bonsoir", "bon"}, 1, {}, true, false);
    shot(v, dir, "barre-mots", 120, 25);

    printf(failures ? "\n%d rendu(s) en échec\n" : "\ntous les rendus OK\n",
           failures);
    return failures ? 1 : 0;
}
