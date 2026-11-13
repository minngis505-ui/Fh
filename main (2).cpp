// Dinamico Frame Hesperia (FH) - clic con prediccion de muerte
//
// Cada clic de salto (pulsar o soltar) se retiene un instante. El mod toma una
// "foto" del estado del juego y simula hacia adelante varios ticks de fisica
// retrasando ese clic 0, 1, 2... hasta N ticks (N = opcion Frames, 2 a 16).
// Elige el retraso mas pequeno con el que el jugador NO muere y vuelve a la
// foto. Despues el clic real se dispara en ese tick exacto.
// Funciona en cubo, bola, robot, spider y wave. En nave, UFO y swing no actua.
// No corrige clics tardios: solo retrasa, nunca adelanta.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <algorithm>
#include <vector>

using namespace geode::prelude;

namespace fh {

    // Cuantos ticks despues del clic se simulan para ver si el jugador muere.
    constexpr int HORIZON = 48;

    struct Ev {
        long long due; // tick (contador propio) en el que se envia al juego
        bool down;
        bool p1;
    };

    struct State {
        long long tick = 0;        // contador propio de ticks reales
        std::vector<Ev> pending;   // clics ya decididos, esperando su tick (ordenados)
        std::vector<Ev> incoming;  // clics nuevos esperando decision
        bool injecting = false;    // el mod mismo envia el clic
        bool simulating = false;   // estamos simulando (nada es real)
        bool dead = false;         // el jugador murio dentro de la simulacion

        void reset() {
            tick = 0;
            pending.clear();
            incoming.clear();
            injecting = false;
            simulating = false;
            dead = false;
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

    // Nave, UFO y swing: el mod no actua (el wave SI funciona)
    template <class P>
    bool unsupportedMode(P* p) {
        bool bad = p->m_isShip || p->m_isBird;
        if constexpr (requires { p->m_isSwing; }) {
            bad = bad || p->m_isSwing;
        }
        return bad;
    }
}

// ---------------------------------------------------------------------------
// Retener los clics, predecir la muerte y dispararlos en el tick correcto
// ---------------------------------------------------------------------------
struct $modify(FHBase, GJBaseGameLayer) {

    bool fhActive() {
        auto pl = PlayLayer::get();
        return pl
            && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)
            && fh::enabled();
    }

    // Envia un clic al juego (sin pasar por el filtro del mod)
    void fhSend(bool down, bool p1) {
        auto& s = fh::S();
        bool old = s.injecting;
        s.injecting = true;
        this->handleButton(down, 1, p1);
        s.injecting = old;
    }

    // Envia todos los clics pendientes que ya les toca
    void fhFlushDue() {
        auto& s = fh::S();
        while (!s.pending.empty() && s.pending.front().due <= s.tick) {
            auto e = s.pending.front();
            s.pending.erase(s.pending.begin());
            this->fhSend(e.down, e.p1);
        }
    }

    // Simula ticks desde la foto con los clics indicados.
    // Devuelve cuantos ticks sobrevive el jugador (HORIZON completo si no muere).
    int fhSimulate(PlayLayer* pl, CheckpointObject* cp, std::vector<fh::Ev> const& evs,
                   long long T, long long end, float dt, bool half, bool last) {
        auto& s = fh::S();

        pl->loadFromCheckpoint(cp);
        s.simulating = true;
        s.dead = false;

        int survived = static_cast<int>(end - T);
        size_t idx = 0;

        for (long long t = T; t < end; t++) {
            while (idx < evs.size() && evs[idx].due <= t) {
                this->fhSend(evs[idx].down, evs[idx].p1);
                idx++;
            }
            GJBaseGameLayer::processCommands(dt, half, last);
            if (s.dead) {
                survived = static_cast<int>(t - T);
                break;
            }
        }

        s.simulating = false;
        return survived;
    }

    // Decide en que tick se envia un clic nuevo
    void fhDecide(fh::Ev e, float dt, bool half, bool last) {
        auto& s = fh::S();
        auto pl = PlayLayer::get();
        PlayerObject* player = e.p1 ? m_player1 : m_player2;

        long long T = s.tick;
        long long base = s.pending.empty() ? T : std::max(T, s.pending.back().due);

        // Sin jugador o modo no soportado: el clic sale sin retraso
        if (!pl || !player || fh::unsupportedMode(player)) {
            s.pending.push_back({ base, e.down, e.p1 });
            return;
        }

        auto cp = pl->createCheckpoint();
        if (!cp) {
            s.pending.push_back({ base, e.down, e.p1 });
            return;
        }
        cp->retain();

        int N = fh::frames();
        long long bestDue = base;
        int bestSurv = -1;
        long long lastDue = -1;

        for (int k = 0; k <= N; k++) {
            long long due = std::max(T + k, base);
            if (due == lastDue) continue; // mismo tick que el intento anterior
            lastDue = due;

            std::vector<fh::Ev> evs = s.pending;
            evs.push_back({ due, e.down, e.p1 });

            long long end = due + fh::HORIZON;
            int surv = this->fhSimulate(pl, cp, evs, T, end, dt, half, last);
            bool full = surv >= static_cast<int>(end - T);

            // Solo se cambia de retraso si mejora claramente (o si ya sobrevive todo)
            if (bestSurv < 0 || full || surv > bestSurv + 1) {
                bestSurv = surv;
                bestDue = due;
            }
            if (full) break;
        }

        // Volver al estado real y soltar la foto
        pl->loadFromCheckpoint(cp);
        cp->release();

        s.pending.push_back({ bestDue, e.down, e.p1 });
    }

    void fhTick(float dt, bool half, bool last) {
        auto& s = fh::S();

        this->fhFlushDue();

        if (!s.incoming.empty()) {
            auto inc = s.incoming;
            s.incoming.clear();
            for (auto const& e : inc) {
                this->fhDecide(e, dt, half, last);
            }
            this->fhFlushDue();
        }
    }

    void handleButton(bool down, int button, bool p1) {
        auto& s = fh::S();

        // Solo el boton de salto (1); no los clics del propio mod ni de la simulacion
        if (s.injecting || s.simulating || button != 1 || !this->fhActive()) {
            return GJBaseGameLayer::handleButton(down, button, p1);
        }

        if (!(p1 ? m_player1 : m_player2)) {
            return GJBaseGameLayer::handleButton(down, button, p1);
        }

        // Retener: se decide al inicio del siguiente tick
        s.incoming.push_back({ 0, down, p1 });
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& s = fh::S();

        if (s.simulating) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        if (!this->fhActive()) {
            // Si el mod se apago con clics retenidos, soltarlos para no perderlos
            if (!fh::enabled() && (!s.pending.empty() || !s.incoming.empty())) {
                for (auto const& e : s.pending) this->fhSend(e.down, e.p1);
                for (auto const& e : s.incoming) this->fhSend(e.down, e.p1);
                s.pending.clear();
                s.incoming.clear();
            }
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        this->fhTick(dt, isHalfTick, isLastTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        s.tick++;
    }
};

// ---------------------------------------------------------------------------
// Muerte dentro de la simulacion: solo se anota, no se mata al jugador
// ---------------------------------------------------------------------------
struct $modify(FHPlay, PlayLayer) {

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& s = fh::S();
        if (s.simulating) {
            s.dead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void resetLevel() {
        fh::S().reset();
        PlayLayer::resetLevel();
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
