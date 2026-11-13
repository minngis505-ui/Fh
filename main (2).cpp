// Dinamico Frame Hesperia (FH) - buffer de clic de salto
//
// Al pulsar salto, el mod mira si el jugador puede saltar en ese instante
// (en suelo, en techo o tocando un orbe). Si puede, el clic pasa normal.
// Si no, lo retiene y revisa en cada tick de fisica si ya puede; en cuanto
// puede, lo dispara. Si se acaba la ventana (N frames), lo dispara igual
// para no perder el clic. En nave, UFO, wave y swing no actua. No corrige
// clics tardios, solo los adelantados.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <type_traits>

using namespace geode::prelude;

namespace fh {

    struct Slot {
        bool active = false;       // hay un clic retenido
        bool releaseQueued = false; // el dedo ya se solto mientras estaba retenido
        bool releaseNext = false;  // enviar la suelta en el siguiente tick
        int ticks = 0;             // ticks que lleva retenido
    };

    struct State {
        Slot slot[2];
        bool injecting = false;    // true cuando el mod mismo envia el clic

        void reset() {
            slot[0] = Slot{};
            slot[1] = Slot{};
            injecting = false;
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

    // Dice si una coleccion (CCArray*, vector, etc.) tiene elementos
    template <class R>
    bool anyItems(R const& r) {
        if constexpr (std::is_pointer_v<R>) {
            if (!r) return false;
            if constexpr (requires { r->count(); }) return r->count() > 0;
            else if constexpr (requires { r->empty(); }) return !r->empty();
            else return false;
        } else {
            if constexpr (requires { r.empty(); }) return !r.empty();
            else if constexpr (requires { r.count(); }) return r.count() > 0;
            else return false;
        }
    }

    // Nave, UFO, wave y swing: el mod no actua
    template <class P>
    bool unsupportedMode(P* p) {
        bool bad = p->m_isShip || p->m_isBird || p->m_isDart;
        if constexpr (requires { p->m_isSwing; }) {
            bad = bad || p->m_isSwing;
        }
        return bad;
    }

    // En suelo, en techo o tocando un orbe
    template <class P>
    bool canJump(P* p) {
        if (p->m_isOnGround) return true;
        if constexpr (requires { p->m_isOnCeiling; }) {
            if (p->m_isOnCeiling) return true;
        }
        if constexpr (requires { p->m_touchingRings; }) {
            if (anyItems(p->m_touchingRings)) return true;
        }
        return false;
    }
}

// ---------------------------------------------------------------------------
// Retener el clic y dispararlo en el tick correcto
// ---------------------------------------------------------------------------
struct $modify(FHBase, GJBaseGameLayer) {

    bool fhActive() {
        auto pl = PlayLayer::get();
        return pl
            && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)
            && fh::enabled();
    }

    void fhSend(bool down, bool p1) {
        auto& s = fh::S();
        s.injecting = true;
        this->handleButton(down, 1, p1);
        s.injecting = false;
    }

    void handleButton(bool down, int button, bool p1) {
        auto& s = fh::S();

        // Solo el boton de salto (1), y no los clics que envia el propio mod
        if (s.injecting || button != 1 || !this->fhActive()) {
            return GJBaseGameLayer::handleButton(down, button, p1);
        }

        int pi = p1 ? 0 : 1;
        auto& slot = s.slot[pi];
        PlayerObject* pl = p1 ? m_player1 : m_player2;

        if (down) {
            // Sin jugador, modo no soportado, o ya puede saltar: pasa normal
            if (!pl || fh::unsupportedMode(pl) || fh::canJump(pl)) {
                return GJBaseGameLayer::handleButton(down, button, p1);
            }
            // Todavia no puede: retener el clic
            slot.active = true;
            slot.releaseQueued = false;
            slot.releaseNext = false;
            slot.ticks = 0;
            return;
        }

        // Suelta del dedo
        if (slot.active) {
            // El clic sigue retenido: guardar la suelta para despues del disparo
            slot.releaseQueued = true;
            return;
        }
        GJBaseGameLayer::handleButton(down, button, p1);
    }

    void fhTick() {
        auto& s = fh::S();

        for (int pi = 0; pi < 2; pi++) {
            auto& slot = s.slot[pi];
            bool p1 = (pi == 0);

            if (slot.releaseNext) {
                slot.releaseNext = false;
                this->fhSend(false, p1);
                continue;
            }
            if (!slot.active) continue;

            PlayerObject* pl = p1 ? m_player1 : m_player2;
            slot.ticks++;

            bool fire = !pl
                || fh::unsupportedMode(pl)
                || fh::canJump(pl)
                || slot.ticks > fh::frames(); // se acabo la ventana: disparar igual

            if (fire) {
                slot.active = false;
                this->fhSend(true, p1);
                if (slot.releaseQueued) {
                    slot.releaseQueued = false;
                    slot.releaseNext = true;
                }
            }
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (this->fhActive()) {
            this->fhTick();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------------------
// Limpiar el estado al reiniciar o salir del nivel
// ---------------------------------------------------------------------------
struct $modify(FHPlay, PlayLayer) {
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
