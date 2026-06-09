// Moteur de rendu Qt Quick pour la barre de candidats.
//
// Rendu QML offscreen + SOFTWARE (plateforme offscreen, backend Software) vers
// une QImage via QQuickView::grabWindow(), sans event-loop Qt : piloté à la
// demande depuis le thread principal de fcitx (cohérent avec le module wayland).
// La QImage est ensuite blittée dans un buffer wl_shm (voir qmlui.cpp).
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QQuickView;
class QQmlComponent;
class QQuickItem;

class PanelView {
public:
    PanelView();
    ~PanelView();

    bool ok() const { return root_ != nullptr; }
    void setColors(const QVariantMap &colors);

    // Met à jour les données et rend ; renvoie l'image (isNull() si échec).
    QImage render(const QStringList &candidates, int highlight,
                  const QString &preedit);

private:
    QQuickView *view_ = nullptr;
    QQmlComponent *component_ = nullptr;
    QQuickItem *root_ = nullptr;
    qint64 colorsStamp_ = -1; // mtime combiné des fichiers de couleurs (live-reload)
};
