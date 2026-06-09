// Phase 1 — addon UI fcitx5 minimal.
//
// But : prouver le POSITIONNEMENT. On crée la popup-surface input-method
// (zwp_input_popup_surface_v2, que LE COMPOSITEUR place au caret) et on y
// attache un buffer wl_shm de couleur unie. Pas encore de Qt Quick (Phase 2).
//
// Repose sur fcitx5 patché : `WaylandIMModule::getInputMethodV2Raw` (voir
// ui/waylandim-public.patch) donne le pointeur brut zwp_input_method_v2, dont on
// tire la popup avec nos propres bindings (générés depuis input-method-v2.xml).
//
// Threading : le module wayland de fcitx lit les events sur un thread dédié mais
// re-planifie dispatch()/flush sur le THREAD PRINCIPAL ; update() et les
// callbacks registry tournent donc tous sur le thread principal → pas de race.
#define _GNU_SOURCE 1
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/userinterface.h>

#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "panelview.h"
#include "wayland_public.h"   // IWaylandModule::addConnectionCreatedCallback
#include "waylandim_public.h" // IWaylandIMModule::getInputMethodV2Raw

namespace {
FCITX_DEFINE_LOG_CATEGORY(qmlpanel_log, "qmlpanel");
#define QP_DEBUG() FCITX_LOGC(qmlpanel_log, Debug)

// Buffer wl_shm auto-destructible (sur l'event wl_buffer.release) à partir d'une
// QImage ARGB32 prémultipliée (= WL_SHM_FORMAT_ARGB8888, alpha prémultiplié).
void bufferRelease(void *, wl_buffer *buf) { wl_buffer_destroy(buf); }
const wl_buffer_listener kBufferListener = {bufferRelease};

wl_buffer *makeImageBuffer(wl_shm *shm, const QImage &img) {
    const int w = img.width(), h = img.height();
    const int stride = w * 4;
    const int size = stride * h;
    int fd = memfd_create("qmlpanel-shm", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return nullptr;
    }
    auto *data = static_cast<uint8_t *>(
        mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    for (int y = 0; y < h; y++)
        std::memcpy(data + y * stride, img.scanLine(y), stride);
    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                               WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(data, size);
    close(fd);
    wl_buffer_add_listener(buf, &kBufferListener, nullptr);
    return buf;
}
} // namespace

using namespace fcitx;

class QmlPanel : public UserInterface {
public:
    QmlPanel(Instance *instance); // défini hors-ligne (loaders auto)

    void update(UserInterfaceComponent comp, InputContext *ic) override {
        if (comp != UserInterfaceComponent::InputPanel)
            return;
        if (!display_ || !compositor_ || !shm_)
            return;
        bool show = ic && ic->hasFocus();
        if (show) {
            auto list = ic->inputPanel().candidateList();
            show = list && list->size() > 0;
        }
        if (show)
            showPanel(ic);
        else
            unmapPanel();
    }
    bool available() override { return true; }
    void suspend() override { destroyPanel(); }
    void resume() override {}

    // appelés par les callbacks registry (thread principal)
    void bindCompositor(wl_compositor *c) { compositor_ = c; }
    void bindShm(wl_shm *s) { shm_ = s; }

private:
    FCITX_ADDON_DEPENDENCY_LOADER(wayland, instance_->addonManager());
    FCITX_ADDON_DEPENDENCY_LOADER(waylandim, instance_->addonManager());

    static void registryGlobal(void *data, wl_registry *reg, uint32_t name,
                               const char *iface, uint32_t ver) {
        auto *self = static_cast<QmlPanel *>(data);
        if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
            self->bindCompositor(static_cast<wl_compositor *>(wl_registry_bind(
                reg, name, &wl_compositor_interface, ver < 4 ? ver : 4)));
        } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
            self->bindShm(static_cast<wl_shm *>(
                wl_registry_bind(reg, name, &wl_shm_interface, 1)));
        }
    }
    static void registryRemove(void *, wl_registry *, uint32_t) {}

    void showPanel(InputContext *ic) {
        auto *im = waylandim()->call<IWaylandIMModule::getInputMethodV2Raw>(ic);
        if (!im) {
            QP_DEBUG() << "no input_method_v2 for ic";
            return;
        }
        if (!view_) {
            view_ = std::make_unique<PanelView>();
        }
        if (!view_->ok()) {
            QP_DEBUG() << "PanelView not ok (QML/Qt error)";
            return;
        }
        // La surface/popup PERSISTE entre les mots (masquée par un buffer NULL,
        // cf unmapPanel) : zéro churn create/destroy pendant la frappe. On ne
        // recrée que si le contexte d'entrée (ou son input-method) change.
        if (!surface_ || ic != currentIc_ || im != currentIm_) {
            destroyPanel(); // remet aussi l'anim d'apparition à zéro
            surface_ = wl_compositor_create_surface(compositor_);
            popup_ = zwp_input_method_v2_get_input_popup_surface(im, surface_);
            currentIc_ = ic;
            currentIm_ = im;
        }
        // candidats + surlignage + marquage « auto-appliqué » (flag Bold posé
        // par l'engine sur le candidat que l'Espace va appliquer)
        auto list = ic->inputPanel().candidateList();
        QStringList cands;
        QVariantList autoMark;
        int highlight = -1;
        if (list) {
            highlight = list->cursorIndex();
            for (int i = 0; i < list->size(); i++) {
                const fcitx::Text &t = list->candidate(i).text();
                cands << QString::fromStdString(t.toString());
                bool bold = false;
                for (size_t s = 0; s < t.size(); s++) {
                    if (t.formatAt(s).test(TextFormatFlag::Bold)) {
                        bold = true;
                        break;
                    }
                }
                autoMark << bold;
            }
        }
        view_->update(cands, highlight, autoMark);
        if (!renderFrame())
            return;
        mapped_ = true;
        QP_DEBUG() << "panel shown, " << cands.size() << " cands";
    }

    // Rend l'état courant et l'affiche ; si une animation est en cours,
    // demande une frame au compositeur pour continuer (boucle onFrame).
    bool renderFrame() {
        QImage img = view_->render();
        if (img.isNull()) {
            QP_DEBUG() << "render returned null image";
            return false;
        }
        wl_buffer *buf = makeImageBuffer(shm_, img);
        if (!buf)
            return false;
        wl_surface_attach(surface_, buf, 0, 0);
        wl_surface_damage(surface_, 0, 0, img.width(), img.height());
        if (view_->animating())
            scheduleFrame(); // la requête doit précéder le commit
        wl_surface_commit(surface_);
        wl_display_flush(display_);
        return true;
    }

    void scheduleFrame() {
        if (frameCb_ || !surface_)
            return;
        frameCb_ = wl_surface_frame(surface_);
        wl_callback_add_listener(frameCb_, &frameListener_, this);
    }

    static void frameDone(void *data, wl_callback *cb, uint32_t) {
        auto *self = static_cast<QmlPanel *>(data);
        if (self->frameCb_ == cb)
            self->frameCb_ = nullptr;
        wl_callback_destroy(cb);
        if (self->surface_ && self->view_ && self->view_->ok())
            self->renderFrame();
    }

    // Masque SANS détruire : buffer NULL → le compositeur démappe la popup,
    // mais surface/popup/rôle restent prêts pour le mot suivant.
    void unmapPanel() {
        if (frameCb_) {
            wl_callback_destroy(frameCb_);
            frameCb_ = nullptr;
        }
        if (surface_ && mapped_) {
            wl_surface_attach(surface_, nullptr, 0, 0);
            wl_surface_commit(surface_);
            mapped_ = false;
            if (display_)
                wl_display_flush(display_);
        }
        if (view_)
            view_->hidden();
    }

    // Destruction réelle (changement de contexte d'entrée, suspend).
    void destroyPanel() {
        unmapPanel();
        if (popup_) {
            zwp_input_popup_surface_v2_destroy(popup_);
            popup_ = nullptr;
        }
        if (surface_) {
            wl_surface_destroy(surface_);
            surface_ = nullptr;
        }
        currentIc_ = nullptr;
        currentIm_ = nullptr;
        if (display_)
            wl_display_flush(display_);
    }

    Instance *instance_;
    std::unique_ptr<HandlerTableEntry<WaylandConnectionCreated>> conn_;
    wl_display *display_ = nullptr;
    std::string name_;
    wl_compositor *compositor_ = nullptr;
    wl_shm *shm_ = nullptr;
    wl_surface *surface_ = nullptr;
    zwp_input_popup_surface_v2 *popup_ = nullptr;
    wl_callback *frameCb_ = nullptr;
    bool mapped_ = false;
    InputContext *currentIc_ = nullptr;
    zwp_input_method_v2 *currentIm_ = nullptr;
    std::unique_ptr<PanelView> view_;

    static constexpr wl_registry_listener registryListener_ = {
        &QmlPanel::registryGlobal, &QmlPanel::registryRemove};
    static constexpr wl_callback_listener frameListener_ = {
        &QmlPanel::frameDone};
};

// Hors-ligne : les loaders de dépendance (auto) sont alors entièrement déclarés.
QmlPanel::QmlPanel(Instance *instance) : instance_(instance) {
    // wl_display via le module wayland (callback sur le thread principal ;
    // rejoué pour les connexions déjà ouvertes).
    conn_ = wayland()->call<IWaylandModule::addConnectionCreatedCallback>(
        [this](const std::string &name, wl_display *display, FocusGroup *) {
            display_ = display;
            name_ = name;
            wl_registry *reg = wl_display_get_registry(display_);
            wl_registry_add_listener(reg, &registryListener_, this);
            wl_display_flush(display_);
            QP_DEBUG() << "wayland connection: " << name;
        });
}

class QmlPanelFactory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override {
        return new QmlPanel(manager->instance());
    }
};

FCITX_ADDON_FACTORY(QmlPanelFactory)
