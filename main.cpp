// Dinamico Frame Hesperia (FH)
// Corrige clics dados hasta N frames (pasos de fisica) antes de lo correcto.
//
// Idea: en cada clic se guarda una "foto" del estado del juego (checkpoint interno).
// Si el jugador va a morir poco despues, en vez de morir se vuelve a esa foto y se
// repite el tramo con el clic retrasado 1, 2, 3... frames (hasta N) hasta que sobreviva.
// No se dibuja ni se muestra nada: el jugador solo nota que no muere.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <vector>

using namespace geode::prelude;

namespace fh {

    // Maximo de pasos de fisica entre el clic y la muerte para intentar corregir.
    // (240 pasos = 1 segundo con el TPS por defecto). 120 = medio segundo.
    constexpr int LOOKBACK = 120;

    struct Ev {
        int off;     // paso relativo al clic donde ocurrio el evento
        bool down;
        int button;  // 1 = salto, 2/3 = izquierda/derecha (plataformer)
        bool p1;
    };

    struct State {
        int step = 0;                     // contador propio de pasos de fisica
        CheckpointObject* snap = nullptr; // foto del estado justo antes del ultimo clic
        int snapStep = 0;
        std::vector<Ev> log;              // entradas reales desde esa foto
        int attempts = 0;                 // retrasos ya probados para este clic
        bool pending = false;             // hay una muerte por evitar (se hace al final del paso)
        int pendingSpan = 0;
        bool replaying = false;
        bool injecting = false;
        int endStep = 0;
        size_t idx = 0;
        std::vector<Ev> plan;
        bool phys[2][4] = {};             // estado real del dedo
        bool game[2][4] = {};             // estado que el juego recibio
        bool gameAtSnap[2][4] = {};

        void clearSnap() {
            if (snap) { snap->release(); snap = nullptr; }
            log.clear();
            plan.clear();
            idx = 0;
            attempts = 0;
            pending = false;
            replaying = false;
        }

        void reset() {
            clearSnap();
            step = 0;
            for (int a = 0; a < 2; a++) for (int b = 0; b < 4; b++) {
                phys[a][b] = false;
                game[a][b] = false;
                gameAtSnap[a][b] = false;
            }
        }
    };

    inline State& S() {
        static State s;
        return s;
    }

    inline bool enabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    inline int frames() {
        return static_cast<int>(Mod::get()->getSettingValue<int64_t>("frames"));
    }
}

// ---------------------------------------------------------------------------
// Entradas y avance de fisica
// ---------------------------------------------------------------------------
struct $modify(FHBase, GJBaseGameLayer) {

    bool fhActive() {
        auto pl = PlayLayer::get();
        return pl
            && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)
            && fh::enabled();
    }

    void handleButton(bool down, int button, bool p1) {
        if (!this->fhActive() || button < 1 || button > 3) {
            return GJBaseGameLayer::handleButton(down, button, p1);
        }

        auto& s = fh::S();
        int pi = p1 ? 0 : 1;

        if (!s.injecting) {
            // Entrada real del jugador
            s.phys[pi][button] = down;

            // Mientras se repite un tramo, las entradas reales se ignoran
            // (se resincroniza el estado del dedo al terminar la repeticion).
            if (s.replaying) return;

            if (down && button == 1) {
                // Clic nuevo: tomar foto del estado ANTES de aplicarlo
                if (auto pl = PlayLayer::get()) {
                    if (auto cp = pl->createCheckpoint()) {
                        s.clearSnap();
                        cp->retain();
                        s.snap = cp;
                        s.snapStep = s.step;
                        for (int a = 0; a < 2; a++) for (int b = 0; b < 4; b++) {
                            s.gameAtSnap[a][b] = s.game[a][b];
                        }
                    }
                }
            }

            if (s.snap) {
                s.log.push_back({ s.step - s.snapStep, down, button, p1 });
            }
        }

        s.game[pi][button] = down;
        GJBaseGameLayer::handleButton(down, button, p1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!this->fhActive()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        auto& s = fh::S();

        // Inyectar las entradas del plan (con el clic retrasado)
        if (s.replaying) {
            while (s.idx < s.plan.size() && s.step >= s.snapStep + s.plan[s.idx].off) {
                auto e = s.plan[s.idx++];
                s.injecting = true;
                this->handleButton(e.down, e.button, e.p1);
                s.injecting = false;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        s.step++;

        // Rebobinar si en este paso se evito una muerte
        if (s.pending) {
            s.pending = false;
            auto pl = PlayLayer::get();
            if (pl && s.snap) {
                int k = s.attempts; // retraso a probar (1..N)

                s.plan.clear();
                for (size_t i = 0; i < s.log.size(); i++) {
                    auto e = s.log[i];
                    if (i == 0) e.off = k;                 // el clic se retrasa k pasos
                    else if (e.off <= k) e.off = k + 1;    // lo siguiente nunca antes del clic
                    s.plan.push_back(e);
                }

                for (int a = 0; a < 2; a++) for (int b = 0; b < 4; b++) {
                    s.game[a][b] = s.gameAtSnap[a][b];
                }

                pl->loadFromCheckpoint(s.snap);
                s.step = s.snapStep;
                s.idx = 0;
                s.endStep = s.snapStep + s.pendingSpan;
                s.replaying = true;
            }
        }

        // Fin de la repeticion: devolver el control al dedo real
        if (s.replaying && s.step >= s.endStep) {
            s.replaying = false;
            for (int a = 0; a < 2; a++) for (int b = 1; b <= 3; b++) {
                if (s.game[a][b] != s.phys[a][b]) {
                    s.injecting = true;
                    this->handleButton(s.phys[a][b], b, a == 0);
                    s.injecting = false;
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Muerte: en vez de morir, pedir un rebobinado con el clic retrasado
// ---------------------------------------------------------------------------
struct $modify(FHPlay, PlayLayer) {

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& s = fh::S();

        if (!fh::enabled() || !s.snap || obj == m_anticheatSpike) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        int span = s.step - s.snapStep;
        int next = s.attempts + 1;

        // Sin foto util, muy lejos del clic, o ya probados todos los retrasos -> morir normal
        if (span > fh::LOOKBACK || next > fh::frames() || next >= span) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        if (!s.pending) {
            s.attempts = next;
            s.pending = true;
            s.pendingSpan = span;
        }
        // No se llama al original: el jugador no muere. El rebobinado se hace
        // al final del paso de fisica (processCommands) para no romper la iteracion.
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        fh::S().reset();
    }

    void onQuit() {
        fh::S().reset();
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Boton con icono (menu principal y menu de pausa) que abre la interfaz
// ---------------------------------------------------------------------------
struct $modify(FHMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        if (auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("bottom-menu"))) {
            auto spr = CCSprite::create("fh-icon.png"_spr);
            if (spr) {
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FHMenu::onFH));
                btn->setID("fh-button"_spr);
                menu->addChild(btn);
                menu->updateLayout();
            }
        }
        return true;
    }

    void onFH(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

struct $modify(FHPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        if (auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"))) {
            auto spr = CCSprite::create("fh-icon.png"_spr);
            if (spr) {
                spr->setScale(0.8f);
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FHPause::onFH));
                btn->setID("fh-button"_spr);
                menu->addChild(btn);
                menu->updateLayout();
            }
        }
    }

    void onFH(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};
