// Predictive Input Buffer v2.1.0 — mobile-friendly rewrite.
// NOTE: written without access to a compiler/Geode SDK; see README for build + fixes.
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <deque>
#include <algorithm>

using namespace geode::prelude;

namespace {
    struct Pending { bool down; int button; bool p1; int age; int max; };

    std::deque<Pending> g_queue;
    bool   g_passthrough = false;
    int    g_tick = 0;
    int    g_lastPress[3] = {-100000, -100000, -100000};
    bool   g_bufferOn = true;      // runtime toggle (on top of the "enabled" setting)
    bool   g_freeze = false;
    bool   g_step = false;
    Ref<CCLabelBMFont> g_overlay;
    float  g_lastScale = 1.f;

    template <class T> T S(char const* k) { return Mod::get()->getSettingValue<T>(k); }

    // ---- per-level profile: "normal,wave" stored as a string ----
    std::string profileKey(PlayLayer* pl) {
        return fmt::format("profile-{}", (int)pl->m_level->m_levelID.value());
    }
    bool loadProfile(PlayLayer* pl, int& normalT, int& waveT) {
        auto key = profileKey(pl);
        if (!Mod::get()->hasSavedValue(key)) return false;
        auto v = Mod::get()->getSavedValue<std::string>(key);
        if (v.empty()) return false;
        return sscanf(v.c_str(), "%d,%d", &normalT, &waveT) == 2;
    }

    int pressTicksFor(PlayerObject* p, PlayLayer* pl) {
        int normal = (int)S<int64_t>("normal-ticks");
        int wave = (int)S<int64_t>("wave-ticks");
        int pn, pw;
        if (pl && loadProfile(pl, pn, pw)) { normal = pn; wave = pw; }
        if (p->m_isDart) return wave;
        char const* k = "cube-ticks";
        if (p->m_isShip) k = "ship-ticks";
        else if (p->m_isBird) k = "ufo-ticks";
        else if (p->m_isBall) k = "ball-ticks";
        else if (p->m_isRobot) k = "robot-ticks";
        else if (p->m_isSpider) k = "spider-ticks";
        else if (p->m_isSwing) k = "swing-ticks";
        int v = (int)S<int64_t>(k);
        return v < 0 ? normal : v;
    }

    int releaseTicksFor(PlayerObject* p, PlayLayer* pl) {
        if (p->m_isDart) {
            int wave = (int)S<int64_t>("wave-release-ticks");
            return wave;
        }
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
        g_overlay->setScale(S<double>("overlay-scale"));
        g_overlay->stopAllActions();
        g_overlay->setOpacity(220);
        g_overlay->runAction(CCSequence::create(CCDelayTime::create(0.6f), CCFadeTo::create(0.4f, 0), nullptr));
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

        if (down) {
            g_lastPress[0] = g_lastPress[1]; g_lastPress[1] = g_lastPress[2]; g_lastPress[2] = g_tick;
            int maxT = pressTicksFor(player, pl);
            // already able to act, buffer disabled for this mode, or spamming -> straight through
            if (maxT <= 0 || player->m_isOnGround || isSpam()) {
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                return;
            }
            g_queue.push_back({true, button, isPlayer1, 0, maxT});
            return;
        }

        // release: if its press is still queued, fire the press first so the tap isn't lost
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
        int rT = releaseTicksFor(player, pl);
        if (rT <= 0 || isSpam()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }
        g_queue.push_back({false, button, isPlayer1, 0, rT});
    }

    void processCommands(float dt) {
        ++g_tick;
        auto pl = PlayLayer::get();
        if (pl && !g_queue.empty()) {
            for (auto it = g_queue.begin(); it != g_queue.end();) {
                auto* player = it->p1 ? m_player1 : m_player2;
                ++it->age;
                bool fire = it->age >= it->max;
                if (it->down && player && player->m_isOnGround) fire = true;
                if (fire) {
                    auto pr = *it; it = g_queue.erase(it);
                    g_passthrough = true;
                    GJBaseGameLayer::handleButton(pr.down, pr.button, pr.p1);
                    g_passthrough = false;
                    showOverlay(fmt::format("P{} {} +{}t", pr.p1 ? 1 : 2, pr.down ? "press" : "release", pr.age));
                } else ++it;
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(BufPlay, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_queue.clear(); g_freeze = false; g_step = false;

        auto win = CCDirector::get()->getWinSize();
        auto lbl = CCLabelBMFont::create("", "bigFont.fnt");
        lbl->setPosition({win.width / 2, win.height - 40});
        lbl->setOpacity(0);
        lbl->setZOrder(100);
        this->addChild(lbl);
        g_overlay = lbl;

        if (S<bool>("practice-buttons")) {
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
            this->addChild(menu);
        }
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

    void update(float dt) {
        auto sched = CCDirector::get()->getScheduler();
        float want = (m_isPracticeMode) ? (float)S<double>("game-speed") : 1.f;
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
        PlayLayer::resetLevel();
    }

    void onQuit() {
        CCDirector::get()->getScheduler()->setTimeScale(1.f);
        g_lastScale = 1.f; g_freeze = false; g_step = false;
        g_queue.clear(); g_overlay = nullptr;
        PlayLayer::onQuit();
    }
};

// Pause menu: Buffer button -> per-level profile save/clear
class $modify(BufPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        std::string c = S<std::string>("button-corner");
        float x = (c.find("der.") != std::string::npos) ? win.width - 40 : 40;
        float y = (c.rfind("Abajo", 0) == 0) ? 40 : win.height - 40;
        auto spr = ButtonSprite::create("Buffer", 60, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .6f);
        spr->setScale((float)S<double>("button-scale"));
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufPause::onBuffer));
        auto menu = CCMenu::create();
        menu->setPosition({x, y});
        menu->addChild(btn);
        btn->setPosition({0, 0});
        this->addChild(menu);
    }

    void onBuffer(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        int n = (int)S<int64_t>("normal-ticks"), w = (int)S<int64_t>("wave-ticks");
        int pn, pw;
        bool has = loadProfile(pl, pn, pw);
        auto msg = fmt::format(
            "Global: normal <cy>{}</c>t, wave <cy>{}</c>t.\nThis level profile: {}.\n\n"
            "<cg>Save</c> stores the global values for this level. <cr>Clear</c> removes it.\n"
            "(Change global values in the mod settings, then Save.)",
            n, w, has ? fmt::format("normal {}t, wave {}t", pn, pw) : "none");
        createQuickPopup("Buffer", msg, "Clear", "Save",
            [pl](auto, bool save) {
                auto key = profileKey(pl);
                if (save) {
                    Mod::get()->setSavedValue<std::string>(key, fmt::format("{},{}",
                        (int)S<int64_t>("normal-ticks"), (int)S<int64_t>("wave-ticks")));
                } else {
                    Mod::get()->setSavedValue<std::string>(key, "");
                }
            });
    }
};
