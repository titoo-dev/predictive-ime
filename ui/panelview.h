// Moteur de rendu Qt Quick pour la barre de candidats.
//
// Rendu QML offscreen + SOFTWARE (plateforme offscreen, backend Software) vers
// une QImage via QQuickView::grabWindow(), sans event-loop Qt : piloté à la
// demande depuis le thread principal de fcitx (cohérent avec le module wayland).
// La QImage est ensuite blittée dans un buffer wl_shm (voir qmlui.cpp).
//
// ANIMATIONS : pas de boucle d'animation Qt ici — l'état (apparition de la
// barre, glissement du pill de surlignage) est avancé en C++ sur une horloge
// steady_clock, exposé au QML par context properties, et qmlui.cpp re-rend des
// frames tant que `animating()` est vrai (cadencé par les frame callbacks
// Wayland du compositeur). Déterministe et testable headless.
#pragma once

#include <chrono>

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class QQuickView;
class QQmlComponent;
class QQuickItem;

class PanelView {
public:
    using Clock = std::chrono::steady_clock;

    PanelView();
    ~PanelView();

    bool ok() const { return root_ != nullptr; }
    void setColors(const QVariantMap &colors);

    // Nouvel état de la barre (à chaque update fcitx). Déclenche les
    // animations : apparition si la barre était cachée, morph du pill si seul
    // le surlignage a bougé (mêmes candidats). `autoMark[i]` dit si le
    // candidat i sera appliqué par l'Espace (liseré accent) ; `composing` :
    // un mot est en cours (indicateur de mode).
    void update(const QStringList &candidates, int highlight,
                const QVariantList &autoMark = {}, bool composing = false);
    // Démarre le FONDU de fermeture ; false si la barre n'était pas visible.
    // Pendant le fondu, animating() reste vrai ; à la fin hideDone() == true
    // et l'appelant démappe la surface.
    bool startHide();
    bool hideDone() const;
    // La barre vient d'être cachée (preedit vide / focus perdu).
    void hidden();
    // Une animation est-elle en cours ? (→ re-rendre à la prochaine frame)
    bool animating() const;

    // Rend l'état courant (animations évaluées à `now`) ; isNull() si échec.
    QImage render();

private:
    struct Anim {
        double from = 0.0, to = 0.0;
        Clock::time_point start{};
        int durMs = 0;
        double at(Clock::time_point now) const;
        bool done(Clock::time_point now) const;
    };

    QQuickView *view_ = nullptr;
    QQmlComponent *component_ = nullptr;
    QQuickItem *root_ = nullptr;
    qint64 colorsStamp_ = -1; // mtime combiné des fichiers de couleurs (live-reload)

    Anim appear_;             // 0→1 à l'apparition de la barre
    Anim hide_;               // →0 au fondu de fermeture
    Anim hl_;                 // position (flottante) du pill de surlignage
    bool shown_ = false;
    bool hiding_ = false;
    int highlight_ = -1;
    QStringList cands_;
    QVariantList autoMark_;   // candidat appliqué par l'Espace (par index)
    bool composing_ = false;  // mot en cours (indicateur de mode)
};
