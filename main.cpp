// Predictive Input Buffer v3.0.0
// - Always predictive: a click is projected a few ticks ahead with a read-only math model; if it
//   would hit a hazard the click is held and registered on a later, safer tick (up to your limit).
// - In-game settings GUI opened from a big button on the pause menu.
#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <deque>
#include <map>
#include <algorithm>
#include <vector>
#include <cmath>

using namespace geode::prelude;

namespace {
    struct Pending { bool down; int button; bool p1; int age; int max; };

    std::deque<Pending> g_queue;
    bool  g_passthrough = false;
    int   g_tick = 0;
    int   g_lastPress[3] = {-100000, -100000, -100000};
    bool  g_bufferOn = true;
    bool  g_freeze = false;
    bool  g_step = false;
    float g_lastScale = 1.f;
    Ref<CCLabelBMFont> g_overlay;
    Ref<CCMenu> g_menu;

    // ---- prediction state (read-only: observed from the real player, never modifies it) ----
    float g_dx = 0.f;          // observed horizontal movement per tick
    float g_slope = 1.f;       // observed wave slope (|dy|/dx)
    bool  g_havePrev = false;
    float g_prevX = 0.f, g_prevY = 0.f;

    template <class T> T S(char const* k) { return Mod::get()->getSettingValue<T>(k); }


    bool isGroundMode(PlayerObject* p) {
        return !(p->m_isDart || p->m_isShip || p->m_isBird || p->m_isSwing);
    }

    int maxTicks(PlayerObject* p, bool down, bool predictive) {
        if (down) {
            if (isGroundMode(p)) return (int)S<int64_t>("ground-ticks");
            return predictive ? (int)S<int64_t>("flight-ticks") : 0;
        }
        if (p->m_isDart) return (int)S<int64_t>("wave-release-ticks");
        return (int)S<int64_t>("release-ticks");
    }

    bool isSpam() {
        if (!S<bool>("spam-guard")) return false;
        int iv = (int)S<int64_t>("spam-interval");
        return (g_lastPress[2] - g_lastPress[0]) < iv * 2 && (g_tick - g_lastPress[0]) < iv * 3;
    }

    void showOverlay(std::string const& text) {
        if (!g_overlay || !S<bool>("overlay")) return;
        g_overlay->setString(text.c_str());
        g_overlay->setScale((float)S<double>("overlay-scale"));
        g_overlay->stopAllActions();
        g_overlay->setOpacity(220);
        g_overlay->runAction(CCSequence::create(CCDelayTime::create(0.6f), CCFadeTo::create(0.4f, 0), nullptr));
    }

    // ---------- prediction ----------
    struct Hz { CCRect r; CCPoint c; float rad; };

    std::vector<Hz> collectHazards(PlayLayer* pl, float px, float reach) {
        std::vector<Hz> out;
        auto arr = pl->m_objects;
        if (!arr) return out;
        for (unsigned i = 0, n = arr->count(); i < n; ++i) {
            auto g = static_cast<GameObject*>(arr->objectAtIndex(i));
            if (!g) continue;
            if (g->m_objectType != GameObjectType::Hazard && g->m_objectType != GameObjectType::AnimatedHazard) continue;
            if (g->m_isGroupDisabled) continue;
            auto pos = g->getPosition();
            if (pos.x < px - 60.f || pos.x > px + reach) continue;
            Hz h;
            h.r = g->getObjectRect();
            h.c = pos;
            h.rad = g->m_objectRadius;
            out.push_back(h);
        }
        return out;
    }

    bool hitsHazard(CCPoint c, std::vector<Hz> const& hz) {
        constexpr float hs = 8.f; // forgiving half-size of the player hitbox
        CCRect pr(c.x - hs, c.y - hs, hs * 2, hs * 2);
        for (auto const& h : hz) {
            if (h.rad > 0.f) {
                float dx = c.x - h.c.x, dy = c.y - h.c.y;
                if (std::sqrt(dx * dx + dy * dy) < h.rad + hs) return true;
            } else if (h.r.intersectsRect(pr)) return true;
        }
        return false;
    }

    // Pure math model on copies of numbers. Never calls game physics, so it cannot corrupt state.
    // Would registering this press/release RIGHT NOW lead into a hazard within ~24 ticks?
    // Modelled: wave (press/release) and cube jumps. Other modes are not modelled (never held back).
    bool wouldDie(PlayLayer* pl, PlayerObject* p, bool pressNow) {
        constexpr int look = 24;
        if (!pl || !p || p->m_isDead || g_dx < 0.01f) return false;
        auto start = p->getPosition();
        auto hz = collectHazards(pl, start.x, g_dx * look + 80.f);
        if (hz.empty()) return false;
        float flip = p->m_isUpsideDown ? -1.f : 1.f;
        float x = start.x, y = start.y;

        if (p->m_isDart) {
            float vy = (pressNow ? 1.f : -1.f) * flip * g_slope * g_dx;
            for (int t = 0; t < look; ++t) {
                x += g_dx; y += vy;
                if (hitsHazard({x, y}, hz)) return true;
            }
            return false;
        }
        if (!pressNow || !isGroundMode(p) || p->m_isBall || p->m_isRobot || p->m_isSpider) return false;

        // cube jump: 60 Hz-frame constants, scaled by frames-per-tick derived from observed speed
        float ft = g_dx / (5.193f * std::max(p->m_playerSpeed, 0.1f));
        float vy = 11.18f * flip, g = 0.958f * flip, groundY = start.y;
        for (int t = 0; t < look; ++t) {
            x += g_dx; y += vy * ft; vy -= g * ft;
            if ((flip > 0 && y <= groundY) || (flip < 0 && y >= groundY)) { y = groundY; vy = 0.f; }
            if (hitsHazard({x, y}, hz)) return true;
        }
        return false;
    }

    bool canFireNow(PlayLayer* pl, PlayerObject* p, Pending const& pd, bool predictive) {
        if (pd.down) {
            if (isGroundMode(p) && !p->m_isOnGround) return false;
            return predictive ? !wouldDie(pl, p, true) : true;
        }
        return predictive ? !wouldDie(pl, p, false) : false;
    }

}

class $modify(BufGJBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (g_passthrough || !pl || !S<bool>("enabled") || !g_bufferOn || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        auto* player = isPlayer1 ? m_player1 : m_player2;
        if (!player) { GJBaseGameLayer::handleButton(down, button, isPlayer1); return; }
        bool predictive = true;

        if (down) {
            // a new press must not be cancelled by an older delayed release: flush those first
            for (auto it = g_queue.begin(); it != g_queue.end();) {
                if (!it->down && it->p1 == isPlayer1) {
                    auto pr = *it; it = g_queue.erase(it);
                    g_passthrough = true;
                    GJBaseGameLayer::handleButton(false, pr.button, pr.p1);
                    g_passthrough = false;
                } else ++it;
            }
            g_lastPress[0] = g_lastPress[1]; g_lastPress[1] = g_lastPress[2]; g_lastPress[2] = g_tick;
        } else {
            // release while its press is still queued: send the press first so the tap isn't lost
            for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
                if (it->down && it->p1 == isPlayer1) {
                    auto pr = *it; g_queue.erase(it);
                    g_passthrough = true;
                    GJBaseGameLayer::handleButton(true, pr.button, pr.p1);
                    g_passthrough = false;
                    showOverlay(fmt::format("P{} press +{}t", pr.p1 ? 1 : 2, pr.age));
                    break;
                }
            }
        }

        int maxT = maxTicks(player, down, predictive);
        Pending pd{down, button, isPlayer1, 0, maxT};
        if (maxT <= 0 || isSpam() || canFireNow(pl, player, pd, predictive)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        g_queue.push_back(pd);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        ++g_tick;
        auto pl = PlayLayer::get();
        if (pl && m_player1 && !m_player1->m_isDead) {
            auto pos = m_player1->getPosition();
            if (g_havePrev && pos.x > g_prevX) {
                float dx = pos.x - g_prevX;
                g_dx = g_dx <= 0.f ? dx : g_dx * 0.8f + dx * 0.2f;
                if (m_player1->m_isDart) {
                    float sl = std::fabs(pos.y - g_prevY) / dx;
                    if (sl > 0.3f && sl < 3.f) g_slope = sl;
                }
            }
            g_prevX = pos.x; g_prevY = pos.y; g_havePrev = true;
        }

        if (pl && !g_queue.empty()) {
            bool predictive = true;
            for (auto it = g_queue.begin(); it != g_queue.end();) {
                auto* player = it->p1 ? m_player1 : m_player2;
                ++it->age;
                bool go = !player || it->age >= it->max || canFireNow(pl, player, *it, predictive);
                if (go) {
                    auto pr = *it; it = g_queue.erase(it);
                    g_passthrough = true;
                    GJBaseGameLayer::handleButton(pr.down, pr.button, pr.p1);
                    g_passthrough = false;
                    if (pr.age > 0) showOverlay(fmt::format("P{} {} +{}t", pr.p1 ? 1 : 2, pr.down ? "press" : "release", pr.age));
                } else ++it;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ---------------------------------------------------------------- settings GUI
namespace {
    struct TRow { char const* key; char const* label; };
    struct SRow { char const* key; char const* label; double mn, mx, step; bool isFloat; };

    constexpr TRow kToggles[] = {
        {"enabled", "Buffer enabled"},
        {"spam-guard", "Spam guard"},
        {"overlay", "Input overlay"},
        {"practice-buttons", "Practice buttons"},
        {"noclip", "Noclip (practice)"},
    };
    constexpr SRow kSteppers[] = {
        {"ground-ticks", "Ground ticks", 0, 40, 1, false},
        {"flight-ticks", "Flight ticks", 0, 40, 1, false},
        {"wave-release-ticks", "Wave release", 0, 40, 1, false},
        {"release-ticks", "Release ticks", 0, 40, 1, false},
        {"game-speed", "Game speed", 0.1, 2.0, 0.1, true},
    };

    std::string fmtVal(SRow const& r) {
        if (r.isFloat) return fmt::format("{:.1f}x", S<double>(r.key));
        return fmt::format("{}", S<int64_t>(r.key));
    }
}

class BufferPopup : public geode::Popup {
protected:
    std::map<std::string, CCLabelBMFont*> m_vals;

    bool init() {
        if (!Popup::init(390.f, 255.f)) return false;
        this->setTitle("Predictive Buffer");
        float top = 190.f;

        for (int i = 0; i < (int)std::size(kToggles); ++i) {
            auto& r = kToggles[i];
            float y = top - i * 30.f;
            auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(BufferPopup::onToggle), .55f);
            t->setID(r.key);
            t->toggle(S<bool>(r.key));
            t->setPosition({28.f, y});
            m_buttonMenu->addChild(t);
            auto l = CCLabelBMFont::create(r.label, "bigFont.fnt");
            l->setScale(.42f);
            l->setAnchorPoint({0.f, .5f});
            l->setPosition({46.f, y});
            m_mainLayer->addChild(l);
        }

        for (int i = 0; i < (int)std::size(kSteppers); ++i) {
            auto& r = kSteppers[i];
            float y = top - i * 30.f;
            auto l = CCLabelBMFont::create(r.label, "bigFont.fnt");
            l->setScale(.36f);
            l->setAnchorPoint({0.f, .5f});
            l->setPosition({185.f, y});
            m_mainLayer->addChild(l);

            auto mk = [&](char const* txt, int dir, float x) {
                auto spr = ButtonSprite::create(txt, 20, true, "bigFont.fnt", "GJ_button_04.png", 20.f, .55f);
                auto b = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufferPopup::onStep));
                b->setID(r.key);
                b->setTag(dir);
                b->setPosition({x, y});
                m_buttonMenu->addChild(b);
            };
            mk("-", -1, 297.f);
            mk("+", 1, 366.f);

            auto v = CCLabelBMFont::create(fmtVal(r).c_str(), "goldFont.fnt");
            v->setScale(.5f);
            v->setPosition({331.f, y});
            m_mainLayer->addChild(v);
            m_vals[r.key] = v;
        }

        auto hint = CCLabelBMFont::create("Tick = 1/4 frame at 60 fps. Predictive is always on: a click is held only if it would hit a hazard.", "chatFont.fnt");
        hint->setScale(.5f);
        hint->setOpacity(160);
        hint->setPosition({195.f, 22.f});
        m_mainLayer->addChild(hint);
        return true;
    }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        Mod::get()->setSettingValue<bool>(t->getID(), !t->isToggled());
    }

    void onStep(CCObject* s) {
        auto n = static_cast<CCNode*>(s);
        std::string key = n->getID();
        int dir = n->getTag();
        for (auto& r : kSteppers) {
            if (key != r.key) continue;
            if (r.isFloat) {
                double v = std::round((S<double>(r.key) + dir * r.step) * 10.0) / 10.0;
                v = std::clamp(v, r.mn, r.mx);
                Mod::get()->setSettingValue<double>(r.key, v);
            } else {
                int64_t v = S<int64_t>(r.key) + (int64_t)(dir * r.step);
                v = std::clamp<int64_t>(v, (int64_t)r.mn, (int64_t)r.mx);
                Mod::get()->setSettingValue<int64_t>(r.key, v);
            }
            m_vals[key]->setString(fmtVal(r).c_str());
        }
    }

public:
    static BufferPopup* create() {
        auto ret = new BufferPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------- play layer
class $modify(BufPlay, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_queue.clear(); g_freeze = false; g_step = false; g_havePrev = false;

        auto win = CCDirector::get()->getWinSize();
        auto lbl = CCLabelBMFont::create("", "bigFont.fnt");
        lbl->setPosition({win.width / 2, win.height - 40});
        lbl->setOpacity(0);
        lbl->setZOrder(100);
        this->addChild(lbl);
        g_overlay = lbl;

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->setZOrder(100);
        auto mk = [&](char const* txt, SEL_MenuHandler cb, CCPoint pos) {
            auto spr = ButtonSprite::create(txt, 30, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .6f);
            spr->setOpacity(150);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition(pos);
            menu->addChild(btn);
        };
        mk("Buf", menu_selector(BufPlay::onToggleBuf), {30, win.height / 2 + 60});
        mk("||",  menu_selector(BufPlay::onFreeze),   {30, win.height / 2 + 20});
        mk(">|",  menu_selector(BufPlay::onStep),     {30, win.height / 2 - 20});
        mk("CP",  menu_selector(BufPlay::onCheckpoint), {30, win.height / 2 - 60});
        menu->setVisible(false);
        this->addChild(menu);
        g_menu = menu;
    }

    void onToggleBuf(CCObject*) {
        g_bufferOn = !g_bufferOn;
        g_queue.clear();
        showOverlay(g_bufferOn ? "Buffer ON" : "Buffer OFF");
    }
    void onFreeze(CCObject*) {
        if (!m_isPracticeMode) return;
        g_freeze = !g_freeze;
        showOverlay(g_freeze ? "Frozen" : "Running");
    }
    void onStep(CCObject*) {
        if (!m_isPracticeMode || !g_freeze) return;
        g_step = true;
    }
    void onCheckpoint(CCObject*) {
        if (!m_isPracticeMode) return;
        this->markCheckpoint();
        showOverlay("Checkpoint");
    }

    void pauseGame(bool unfocused) {
        if (g_menu) g_menu->setVisible(false);
        PlayLayer::pauseGame(unfocused);
    }

    void update(float dt) {
        // practice buttons only exist on screen while playing in practice mode (never while paused)
        if (g_menu) g_menu->setVisible(m_isPracticeMode && S<bool>("practice-buttons"));

        auto sched = CCDirector::get()->getScheduler();
        float want = m_isPracticeMode ? (float)S<double>("game-speed") : 1.f;
        if (want != g_lastScale) { sched->setTimeScale(want); g_lastScale = want; }

        if (m_isPracticeMode && g_freeze) {
            if (!g_step) return;
            g_step = false;
            PlayLayer::update(1.f / 60.f);
            return;
        }
        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (m_isPracticeMode && S<bool>("noclip")) return;
        PlayLayer::destroyPlayer(p, obj);
    }

    void resetLevel() {
        g_queue.clear();
        g_havePrev = false;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        g_lastScale = 1.f; g_freeze = false; g_step = false;
        g_queue.clear(); g_overlay = nullptr; g_menu = nullptr;
        g_havePrev = false; g_dx = 0.f; g_slope = 1.f;
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------- pause menu button
class $modify(BufPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Buffer", 90, true, "goldFont.fnt", "GJ_button_01.png", 45.f, .9f);
        spr->setScale((float)S<double>("button-scale"));
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufPause::onBuffer));
        auto menu = CCMenu::create();
        menu->setPosition({win.width - 70.f, win.height / 2.f});
        menu->setZOrder(100);
        btn->setPosition({0, 0});
        menu->addChild(btn);
        this->addChild(menu);
    }

    void onBuffer(CCObject*) {
        if (auto p = BufferPopup::create()) p->show();
    }
};
