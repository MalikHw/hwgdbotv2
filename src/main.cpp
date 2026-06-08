#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <ctime>
#include <regex>

using namespace geode::prelude;

// --- Constants & Types ---

static const std::string BOOMLINGS_API = "https://www.boomlings.com/database/getGJLevels21.php";
static const std::string TWITCH_DEVICE_FLOW_URL = "https://id.twitch.tv/oauth2/device";
static const std::string TWITCH_TOKEN_URL = "https://id.twitch.tv/oauth2/token";
static const std::string TWITCH_IRC_WS = "wss://irc-ws.chat.twitch.tv:443";
static const std::string CLIENT_ID = "kycfao9ivv1m42kk6lqc2zrm3mvu88";

struct QueueEntry {
    std::string levelId;
    std::string requester;
    time_t timestamp;
    std::string levelName;
    std::string difficulty;
    matjson::Value toJson() const {
        return matjson::makeObject({
            {"levelId", levelId},
            {"requester", requester},
            {"timestamp", static_cast<long long>(timestamp)},
            {"levelName", levelName},
            {"difficulty", difficulty}
        });
    }
    static QueueEntry fromJson(matjson::Value const& json) {
        return {
            json["levelId"].asString().unwrapOr(""),
            json["requester"].asString().unwrapOr(""),
            static_cast<time_t>(json["timestamp"].asInt().unwrapOr(0)),
            json["levelName"].asString().unwrapOr(""),
            json["difficulty"].asString().unwrapOr("")
        };
    }
};
// globals ig
static std::vector<QueueEntry> g_queue;
static std::unordered_set<std::string> g_bannedUsers;
static std::unordered_map<std::string, time_t> g_userCooldowns;
static bool g_blackScreenActive = false;
static bool g_isConnecting = false;
static web::WebSocket* g_twitchWS = nullptr;

// persistence
void saveQueue() {
    matjson::Value data = matjson::makeArray();
    for (const auto& entry : g_queue) { data.push_back(entry.toJson()); }
    Mod::get()->setSavedValue("request-queue", data);
    matjson::Value bans = matjson::makeArray();
    for (const auto& user : g_bannedUsers) { bans.push_back(user); }
    Mod::get()->setSavedValue("banned-users", bans);
}

void loadQueue() {
    auto data = Mod::get()->getSavedValue<matjson::Value>("request-queue");
    if (data.isArray()) {
        g_queue.clear();
        for (const auto& item : data) {
            g_queue.push_back(QueueEntry::fromJson(item));
        }
    }

    auto bans = Mod::get()->getSavedValue<matjson::Value>("banned-users");
    if (bans.isArray()) {
        g_bannedUsers.clear();
        for (const auto& item : bans) {
            g_bannedUsers.insert(item.asString().unwrapOr(""));
        }
    }
}

// twitch
void sendTwitchMessage(const std::string& msg) {
    if (!g_twitchWS || !Mod::get()->getSettingValue<bool>("bot-speak")) return;
    std::string channel = Mod::get()->getSettingValue<std::string>("twitch-username");
    if (channel.empty()) return;
    std::transform(channel.begin(), channel.end(), channel.begin(), ::tolower);
    g_twitchWS->send(fmt::format("PRIVMSG #{} :{}", channel, msg));
}

void processIRCMessage(const std::string& raw) {
    // basic PRIVMSG parsing
    // :user!user@user.tmi.twitch.tv PRIVMSG #channel :message
    std::regex msgRegex(R"(:([^!]+)![^ ]+ PRIVMSG #[^ ]+ :(.+))");
    std::smatch match;
    if (std::regex_search(raw, match, msgRegex)) {
        std::string user = match[1].str();
        std::string text = match[2].str();
        if (g_bannedUsers.count(user)) return;
        // check for !r command or just ID
        bool isRequest = (text.find("!r ") == 0);
        std::string content = isRequest ? text.substr(3) : text;

        // Check for 6-9 digit IDs (nice)
        std::regex idRegex(R"(\b(\d{6,9})\b)");
        std::smatch idMatch;
        if (std::regex_search(content, idMatch, idRegex)) {
            std::string levelId = idMatch[1].str();
            // duplicate
            auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueueEntry& e) {
                return e.levelId == levelId;
            });
            if (it != g_queue.end()) return;
            // cooldown
            if (g_userCooldowns.count(user)) {
                if (time(nullptr) - g_userCooldowns[user] < 2) {
                    return;
                }
            }

            // Validate level
            std::string body = fmt::format("str={}&type=0&secret=Wmfd2893gb7", levelId);
            geode::async::spawn(
                [body]() -> web::WebFuture {
                    return web::WebRequest()
                        .header("Content-Type", "application/x-www-form-urlencoded")
                        .body(std::vector<uint8_t>(body.begin(), body.end()))
                        .post(BOOMLINGS_API);
                },
                [levelId, user](web::WebResponse res) {
                    if (!res.ok()) return;
                    std::string text = res.string().unwrapOr("");
                    if (text == "-1" || text.empty()) {
                        sendTwitchMessage(fmt::format("HwGDBot: {} does not exist as a level", levelId));
                        return;
                    }

                    // level name extraction
                    std::string levelName = "Unknown";
                    std::regex nameRegex(R"(^[^:]+:([^:]+):)");
                    std::smatch nameMatch;
                    if (std::regex_search(text, nameMatch, nameRegex)) { levelName = nameMatch[1].str(); }

                    QueueEntry entry;
                    entry.levelId = levelId;
                    entry.requester = user;
                    entry.timestamp = time(nullptr);
                    entry.levelName = levelName;
                    g_queue.push_back(entry);
                    g_userCooldowns[user] = time(nullptr);
                    saveQueue();
                    sendTwitchMessage(fmt::format("HwGDBot: added {} by {}", levelName, user));
                    Notification::create(fmt::format("New request: {} by {}", levelName, user), NotificationIcon::Info)->show();
                }
            );
        }
    } else if (raw.find("PING") == 0) {
        g_twitchWS->send("PONG :tmi.twitch.tv");
    }
}

void connectToTwitch() {
    if (g_isConnecting || g_twitchWS) return;
    std::string token = Mod::get()->getSettingValue<std::string>("twitch-oauth-token");
    std::string channel = Mod::get()->getSettingValue<std::string>("twitch-username");
    if (token.empty() || channel.empty()) return;

    g_isConnecting = true;
    log::info("Connecting to Twitch IRC...");

    geode::async::spawn(
        []() -> web::WebFuture {
            return web::WebSocket::connect(TWITCH_IRC_WS);
        },
        [token, channel](Result<web::WebSocket*, std::string> res) {
            g_isConnecting = false;
            if (!res) {
                log::error("WebSocket connection failed: {}", res.error());
                return;
            }
            g_twitchWS = res.unwrap();
            
            g_twitchWS->send(fmt::format("PASS oauth:{}", token));
            g_twitchWS->send("NICK geode_bot");
            std::string lowerChannel = channel;
            std::transform(lowerChannel.begin(), lowerChannel.end(), lowerChannel.begin(), ::tolower);
            g_twitchWS->send(fmt::format("JOIN #{}", lowerChannel));
            sendTwitchMessage("HwGDBot: Now capturing id's");
            g_twitchWS->onMessage([](web::WebSocketMessage msg) {
                if (msg.isText()) {
                    processIRCMessage(msg.asText());
                }
            });
            g_twitchWS->onClose([](std::optional<web::WebSocketClose> close) {
                g_twitchWS = nullptr;
                log::warn("Twitch WebSocket closed.");
            });
        }
    );
}

// oauth bs
class TwitchLoginPopup : public geode::Popup<> {
    std::string m_deviceCode;
    std::string m_userCode;
    std::string m_verificationUrl;
    int m_interval;
    bool m_polling = false;

protected:
    bool setup() override {
        this->setTitle("Twitch Login");
        auto sz = m_mainLayer->getContentSize();
        auto lbl = CCLabelBMFont::create("loading...", "bigFont.fnt");
        lbl->setScale(0.5f);
        lbl->setPosition(sz / 2);
        lbl->setTag(100);
        m_mainLayer->addChild(lbl);
        startDeviceFlow();
        return true;
    }

    void startDeviceFlow() {
        std::string body = fmt::format("client_id={}&scope=chat:read+chat:edit", CLIENT_ID);
        geode::async::spawn(
            [body]() -> web::WebFuture {
                return web::WebRequest()
                    .header("Content-Type", "application/x-www-form-urlencoded")
                    .body(std::vector<uint8_t>(body.begin(), body.end()))
                    .post(TWITCH_DEVICE_FLOW_URL);
            },
            [this](web::WebResponse res) {
                if (!res.ok()) {
                    onClose(nullptr);
                    FLAlertLayer::create("Error", "Failed to start device flow", "OK")->show();
                    return;
                }
                auto json = res.json();
                if (!json) return;
                m_deviceCode = (*json)["device_code"].asString().unwrapOr("");
                m_userCode = (*json)["user_code"].asString().unwrapOr("");
                m_verificationUrl = (*json)["verification_uri"].asString().unwrapOr("twitch.tv/activate");
                m_interval = (*json)["interval"].asInt().unwrapOr(5);
                updateUI();
                startPolling();
            }
        );
    }

    void updateUI() {
        if (auto lbl = m_mainLayer->getChildByTag(100)) lbl->removeFromParent();
        auto sz = m_mainLayer->getContentSize();
        auto text = fmt::format("Go to <cy>{}</c>\nand enter code: <cg>{}</c>", m_verificationUrl, m_userCode);
        auto lbl = TextArea::create(text, "chatFont.fnt", 1.0f, 200.f, {0.5f, 0.5f}, 20.f, false);
        lbl->setPosition(sz / 2);
        m_mainLayer->addChild(lbl);
        auto menu = CCMenu::create();
        auto visitBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Visit", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(TwitchLoginPopup::onVisit)
        );
        visitBtn->setPosition({0, -50});
        menu->addChild(visitBtn);
        m_mainLayer->addChild(menu);
    }

    void onVisit(CCObject*) {
        geode::utils::web::openLinkInBrowser("https://" + m_verificationUrl);
    }

    void startPolling() {
        m_polling = true;
        pollToken(0);
    }

    void pollToken(float) {
        if (!m_polling) return;
        std::string body = fmt::format("client_id={}&device_code={}&grant_type=urn:ietf:params:oauth:grant-type:device_code", CLIENT_ID, m_deviceCode);
        geode::async::spawn(
            [body]() -> web::WebFuture {
                return web::WebRequest()
                    .header("Content-Type", "application/x-www-form-urlencoded")
                    .body(std::vector<uint8_t>(body.begin(), body.end()))
                    .post(TWITCH_TOKEN_URL);
            },
            [this](web::WebResponse res) {
                if (!res.ok()) {
                    this->scheduleOnce(schedule_selector(TwitchLoginPopup::pollToken), m_interval);
                    return;
                }
                auto json = res.json();
                if (json && (*json).contains("access_token")) {
                    std::string token = (*json)["access_token"].asString().unwrapOr("");
                    Mod::get()->setSettingValue("twitch-oauth-token", token);
                    FLAlertLayer::create("Success", "Logged in to Twitch!", "OK")->show();
                    connectToTwitch();
                    onClose(nullptr);
                } else {
                    this->scheduleOnce(schedule_selector(TwitchLoginPopup::pollToken), m_interval);
                }
            }
        );
    }

    void onClose(CCObject* sender) override {
        m_polling = false;
        Popup::onClose(sender);
    }

public:
    static TwitchLoginPopup* create() {
        auto ret = new TwitchLoginPopup();
        if (ret->initAnchored(240.f, 160.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// queue UI
class QueuePopup : public geode::Popup<>, public FLAlertLayerProtocol {
    int m_page = 0;
    static constexpr int PER_PAGE = 5;

protected:
    bool setup() override {
        this->setTitle("Request Queue");
        buildPage();
        return true;
    }
    void buildPage() {
        m_mainLayer->removeAllChildrenWithCleanup(true);
        auto sz = m_mainLayer->getContentSize();
        if (g_queue.empty()) {
            auto lbl = CCLabelBMFont::create("Queue is empty :(", "bigFont.fnt");
            lbl->setScale(0.5f);
            lbl->setPosition(sz / 2);
            m_mainLayer->addChild(lbl);
            return;
        }
        int total = g_queue.size();
        int totalPages = (total + PER_PAGE - 1) / PER_PAGE;
        if (m_page >= totalPages) m_page = totalPages - 1;

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        float y = sz.height - 60.f;
        int start = m_page * PER_PAGE;
        int end = std::min(start + PER_PAGE, total);

        for (int i = start; i < end; i++) {
            auto& entry = g_queue[i];
            auto row = CCNode::create();
            row->setContentSize({sz.width - 40.f, 40.f});
            row->setPosition({sz.width / 2, y});

            auto posLbl = CCLabelBMFont::create(std::to_string(i + 1).c_str(), "goldFont.fnt");
            posLbl->setScale(0.4f);
            posLbl->setPosition({20, 20});
            row->addChild(posLbl);

            auto nameLbl = CCLabelBMFont::create(fmt::format("{} ({})", entry.levelName, entry.levelId).c_str(), "bigFont.fnt");
            nameLbl->setScale(0.4f);
            nameLbl->setAnchorPoint({0, 0.5f});
            nameLbl->setPosition({40, 25});
            row->addChild(nameLbl);

            auto reqLbl = CCLabelBMFont::create(fmt::format("Requested by: {}", entry.requester).c_str(), "chatFont.fnt");
            reqLbl->setScale(0.4f);
            reqLbl->setAnchorPoint({0, 0.5f});
            reqLbl->setPosition({40, 10});
            row->addChild(reqLbl);

            auto removeBtn = CCMenuItemSpriteExtra::create(
                CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"),
                this, menu_selector(QueuePopup::onRemove)
            );
            removeBtn->setTag(i);
            removeBtn->setScale(0.6f);
            removeBtn->setPosition({sz.width - 60.f, y});
            menu->addChild(removeBtn);

            m_mainLayer->addChild(row);
            y -= 45.f;
        }

        // footah
        auto clearBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Clear All", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(QueuePopup::onClearAll)
        );
        clearBtn->setPosition({sz.width / 2, 25});
        clearBtn->setScale(0.6f);
        menu->addChild(clearBtn);

        if (totalPages > 1) {
            if (m_page > 0) {
                auto prevBtn = CCMenuItemSpriteExtra::create(
                    CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
                    this, menu_selector(QueuePopup::onPrev)
                );
                prevBtn->setPosition({30, 25});
                prevBtn->setScale(0.6f);
                menu->addChild(prevBtn);
            }
            if (m_page < totalPages - 1) {
                auto nextBtn = CCMenuItemSpriteExtra::create(
                    CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
                    this, menu_selector(QueuePopup::onNext)
                );
                nextBtn->setFlipX(true);
                nextBtn->setPosition({sz.width - 30, 25});
                nextBtn->setScale(0.6f);
                menu->addChild(nextBtn);
            }
        }
    }

    void onRemove(CCObject* sender) {
        int idx = sender->getTag();
        g_queue.erase(g_queue.begin() + idx);
        saveQueue();
        buildPage();
    }

    void onClearAll(CCObject*) {
        geode::createQuickPopup("Clear All", "Clear the entire queue?", "Cancel", "Clear", [this](FLAlertLayer*, bool btn2) {
            if (btn2) {
                g_queue.clear();
                saveQueue();
                buildPage();
            }
        });
    }

    void onPrev(CCObject*) { m_page--; buildPage(); }
    void onNext(CCObject*) { m_page++; buildPage(); }

public:
    static QueuePopup* create() {
        auto ret = new QueuePopup();
        if (ret->initAnchored(350.f, 280.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// hooks
class $modify(HwGDMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto rightMenu = this->getChildByID("right-side-menu");
        if (rightMenu) {
            auto spr = CCSprite::create("logo.png"_spr);
            if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_chatBtn_001.png");
            spr->setScale(0.7f);

            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(HwGDMenuLayer::onOpenQueue));
            btn->setID("hwgdbot-btn");
            rightMenu->addChild(btn);
            rightMenu->updateLayout();

            this->schedule(schedule_selector(HwGDMenuLayer::updateBadge), 1.0f);
        }

        connectToTwitch();
        return true;
    }

    void updateBadge(float) {
        if (auto btn = this->getChildByIDRecursive("hwgdbot-btn")) {
            if (auto existing = btn->getChildByTag(999)) existing->removeFromParent();
            if (!g_queue.empty()) {
                auto badge = CCSprite::createWithSpriteFrameName("circle.png");
                badge->setColor({255, 50, 50});
                badge->setScale(0.5f);
                badge->setPosition({btn->getContentSize().width - 5, btn->getContentSize().height - 5});
                badge->setTag(999);

                auto lbl = CCLabelBMFont::create(std::to_string(g_queue.size()).c_str(), "bigFont.fnt");
                lbl->setScale(0.4f);
                lbl->setPosition(badge->getContentSize() / 2);
                badge->addChild(lbl);
                btn->addChild(badge);
            }
        }
    }

    void onOpenQueue(CCObject*) {
        QueuePopup::create()->show();
    }
};

class $modify(HwGDPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        std::string lvlId = std::to_string(level->m_levelID);
        auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueueEntry& e) {
            return e.levelId == lvlId;
        });

        if (it != g_queue.end()) {
            std::string requester = it->requester;
            g_queue.erase(it);
            saveQueue();
            if (Mod::get()->getSettingValue<bool>("show-toast")) {
                Notification::create(fmt::format("Now playing: {} requested by {}", level->m_levelName, requester), NotificationIcon::None)->show();
            }
            sendTwitchMessage(fmt::format("HwGDBot: Now playing {} requested by {}", level->m_levelName, requester));
        }

        // franklin overlay
        auto ws = CCDirector::get()->getWinSize();
        auto overlay = CCLayerColor::create({0, 0, 0, 255});
        overlay->setID("black-overlay");
        overlay->setZOrder(99999);
        overlay->setVisible(g_blackScreenActive);
        this->addChild(overlay);

        return true;
    }
};

class $modify(HwGDShaderLayer, ShaderLayer) {
    void visit() {
        if (g_blackScreenActive) return;
        ShaderLayer::visit();
    }
};

class $modify(HwGDLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        std::string lvlId = std::to_string(level->m_levelID);
        auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueueEntry& e) {
            return e.levelId == lvlId;
        });

        if (it != g_queue.end()) {
            auto menu = CCMenu::create();
            menu->setPosition({0, 0});
            menu->setID("hwgdbot-info-menu");

            auto banBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Ban Requester", "goldFont.fnt", "GJ_button_06.png", .8f),
                this, menu_selector(HwGDLevelInfoLayer::onBanRequester)
            );
            banBtn->setPosition({100, 100});
            banBtn->setScale(0.6f);
            banBtn->setID("ban-btn");
            menu->addChild(banBtn);
            this->addChild(menu);
        }

        return true;
    }

    void onBanRequester(CCObject*) {
        std::string lvlId = std::to_string(m_level->m_levelID);
        auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueueEntry& e) {
            return e.levelId == lvlId;
        });
        if (it != g_queue.end()) {
            g_bannedUsers.insert(it->requester);
            g_queue.erase(it);
            saveQueue();
            FLAlertLayer::create("Banned", "User has been banned from requests.", "OK")->show();
        }
    }
};

// settings...
class TwitchLoginSettingNode : public SettingNodeV3 {
protected:
    bool init(std::shared_ptr<SettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;
        auto menu = this->getButtonMenu();
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Login", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(TwitchLoginSettingNode::onLogin)
        );
        btn->setScale(0.5f);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }
    void onLogin(CCObject*) { TwitchLoginPopup::create()->show(); }
    void onCommit() override {}
    void onResetToDefault() override {}
public:
    static TwitchLoginSettingNode* create(std::shared_ptr<SettingV3> setting, float width) {
        auto ret = new TwitchLoginSettingNode();
        if (ret->init(setting, width)) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

class JSONExportSettingNode : public SettingNodeV3 {
protected:
    bool init(std::shared_ptr<SettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;
        auto menu = this->getButtonMenu();
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Export", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(JSONExportSettingNode::onExport)
        );
        btn->setScale(0.5f);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }
    void onExport(CCObject*) {
        matjson::Value data = matjson::makeObject({
            {"hwgdbot-version", Mod::get()->getVersion().toString()},
            {"queue", matjson::makeArray()},
            {"bans", matjson::makeArray()}
        });
        for (const auto& e : g_queue) data["queue"].push_back(e.toJson());
        for (const auto& b : g_bannedUsers) data["bans"].push_back(b);

        auto path = Mod::get()->getSaveDir() / "queue_export.json";
        std::ofstream file(path.string());
        file << data.dump(4);
        file.close();
        FLAlertLayer::create("Exported", fmt::format("Queue exported to {}", path.string()), "OK")->show();
    }
    void onCommit() override {}
    void onResetToDefault() override {}
public:
    static JSONExportSettingNode* create(std::shared_ptr<SettingV3> setting, float width) {
        auto ret = new JSONExportSettingNode();
        if (ret->init(setting, width)) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

class JSONImportSettingNode : public SettingNodeV3 {
protected:
    bool init(std::shared_ptr<SettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;
        auto menu = this->getButtonMenu();
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Import", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(JSONImportSettingNode::onImport)
        );
        btn->setScale(0.5f);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }
    void onImport(CCObject*) {
        file::FilePicker::create()->setFilter({"*.json"})->pickFile([](ghc::filesystem::path path) {
            std::ifstream file(path.string());
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto json = matjson::parse(content);
            if (!json || !(*json).contains("hwgdbot-version")) {
                FLAlertLayer::create("Error", "Invalid data/version", "OK")->show();
                return;
            }
            if ((*json)["hwgdbot-version"].asString().unwrapOr("") != Mod::get()->getVersion().toString()) {
                FLAlertLayer::create("Error", "Version mismatch", "OK")->show();
                return;
            }
            
            g_queue.clear();
            for (const auto& item : (*json)["queue"]) g_queue.push_back(QueueEntry::fromJson(item));
            g_bannedUsers.clear();
            for (const auto& item : (*json)["bans"]) g_bannedUsers.insert(item.asString().unwrapOr(""));
            saveQueue();
            FLAlertLayer::create("Imported", "Queue and bans imported successfully.", "OK")->show();
        });
    }
    void onCommit() override {}
    void onResetToDefault() override {}
public:
    static JSONImportSettingNode* create(std::shared_ptr<SettingV3> setting, float width) {
        auto ret = new JSONImportSettingNode();
        if (ret->init(setting, width)) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

class ReinstallSettingNode : public SettingNodeV3 {
protected:
    bool init(std::shared_ptr<SettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;
        auto menu = this->getButtonMenu();
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Reinstall", "goldFont.fnt", "GJ_button_01.png", .8f),
            this, menu_selector(ReinstallSettingNode::onReinstall)
        );
        btn->setScale(0.5f);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }
    void onCommit() override {}
    void onResetToDefault() override {}
public:
    static ReinstallSettingNode* create(std::shared_ptr<SettingV3> setting, float width) {
        auto ret = new ReinstallSettingNode();
        if (ret->init(setting, width)) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

// settings register
SettingNodeV3* TwitchLoginSettingNode::createNode(float width) { return TwitchLoginSettingNode::create(shared_from_this(), width); }
SettingNodeV3* JSONExportSettingNode::createNode(float width) { return JSONExportSettingNode::create(shared_from_this(), width); }
SettingNodeV3* JSONImportSettingNode::createNode(float width) { return JSONImportSettingNode::create(shared_from_this(), width); }
SettingNodeV3* ReinstallSettingNode::createNode(float width) { return ReinstallSettingNode::create(shared_from_this(), width); }

class ButtonSetting : public SettingV3 {
public:
    static Result<std::shared_ptr<SettingV3>> parse(std::string const& key, std::string const& modID, matjson::Value const& json) {
        auto res = std::make_shared<ButtonSetting>();
        res->init(key, modID, json);
        return Ok(std::static_pointer_cast<SettingV3>(res));
    }
    SettingNodeV3* createNode(float width) override {
        if (m_key == "login-button") return TwitchLoginSettingNode::create(shared_from_this(), width);
        if (m_key == "export-json") return JSONExportSettingNode::create(shared_from_this(), width);
        if (m_key == "import-json") return JSONImportSettingNode::create(shared_from_this(), width);
        if (m_key == "reinstall-mod") return ReinstallSettingNode::create(shared_from_this(), width);
        return nullptr;
    }
    bool load(matjson::Value const&) override { return true; }
    bool save(matjson::Value&) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}
};

class $modify(HwGDCCLayer, CCLayer) {
    void keyDown(enum enumKeyCodes key) {
        auto openKey = Mod::get()->getSettingValue<geode::prelude::KeybindSetting>("keybind-open-queue");
        auto blackKey = Mod::get()->getSettingValue<geode::prelude::KeybindSetting>("keybind-toggle-black");
        
        if (key == KEY_F2) {
            QueuePopup::create()->show();
        } else if (key == KEY_F3) {
            g_blackScreenActive = !g_blackScreenActive;
            if (auto pl = PlayLayer::get()) {
                if (auto overlay = pl->getChildByID("black-overlay")) {
                    overlay->setVisible(g_blackScreenActive);
                }
            }
        }
        CCLayer::keyDown(key);
    }
};

$on_mod(Loaded) {
    loadQueue();
    Mod::get()->addCustomSetting<ButtonSetting>("login-button");
    Mod::get()->addCustomSetting<ButtonSetting>("export-json");
    Mod::get()->addCustomSetting<ButtonSetting>("import-json");
    Mod::get()->addCustomSetting<ButtonSetting>("reinstall-mod");
}
