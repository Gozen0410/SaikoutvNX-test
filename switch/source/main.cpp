#include <borealis.hpp>
#include <borealis/core/font.hpp>
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char* kAppDir = "sdmc:/switch/SaikouTV";
static constexpr const char* kAppLogPath = "sdmc:/switch/SaikouTV/saikou_debug.log";
static FILE* g_log = nullptr;
static brls::View* g_homeView = nullptr;

static void log_stage(const char* stage)
{
    if (!g_log)
        return;

    std::fprintf(g_log, "[Saikou] %s\n", stage);
    std::fflush(g_log);
}

static void open_debug_logs()
{
    fsdevMountSdmc();
    mkdir("sdmc:/switch", 0777);
    mkdir(kAppDir, 0777);

    g_log = std::fopen(kAppLogPath, "w");
    log_stage("entered main");
    log_stage(g_log ? "app-folder log opened" : "app-folder log could not be opened");
}

static void close_debug_logs()
{
    if (g_log)
    {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

static void log_controller_edges()
{
    struct WatchedButton
    {
        brls::ControllerButton button;
        const char* name;
    };

    static constexpr WatchedButton watchedButtons[] = {
        {brls::BUTTON_START, "Plus / START"},
        {brls::BUTTON_NAV_UP, "D-pad NAV UP"},
        {brls::BUTTON_NAV_RIGHT, "D-pad NAV RIGHT"},
        {brls::BUTTON_NAV_DOWN, "D-pad NAV DOWN"},
        {brls::BUTTON_NAV_LEFT, "D-pad NAV LEFT"},
        {brls::BUTTON_A, "A"},
        {brls::BUTTON_B, "B"},
    };
    static bool previous[brls::_BUTTON_MAX] = {};

    const brls::ControllerState& state = brls::Application::getControllerState();
    for (const WatchedButton& watched : watchedButtons)
    {
        const bool pressed = state.buttons[watched.button];
        if (pressed && !previous[watched.button])
        {
            char marker[128];
            std::snprintf(marker, sizeof(marker), "Borealis received controller button: %s", watched.name);
            log_stage(marker);
        }
        previous[watched.button] = pressed;
    }
}

static brls::View* create_xml_failure_view()
{
    log_stage("creating visible XML-failure fallback");
    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidthPercentage(100.0f);
    root->setHeightPercentage(100.0f);
    root->setPadding(48.0f);
    root->setBackgroundColor(nvgRGB(16, 20, 29));

    brls::Label* title = new brls::Label();
    title->setText("SAIKOU TV");
    title->setFontSize(36.0f);
    title->setTextColor(nvgRGB(244, 246, 250));
    root->addView(title);

    brls::Label* message = new brls::Label();
    message->setText("The Home XML could not be loaded. Check saikou_debug.log on the SD card.");
    message->setFontSize(18.0f);
    message->setTextColor(nvgRGB(174, 184, 200));
    message->setMarginTop(14.0f);
    root->addView(message);

    return root;
}

#include "live_anime_ui.hpp"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    open_debug_logs();
    initialize_source_settings();

    log_stage("before romfsInit");
    Result romfsResult = romfsInit();
    if (R_FAILED(romfsResult))
    {
        char marker[96];
        std::snprintf(marker, sizeof(marker), "romfsInit FAILED rc=0x%08X", static_cast<unsigned int>(romfsResult));
        log_stage(marker);
        close_debug_logs();
        return EXIT_FAILURE;
    }
    log_stage("romfsInit OK");

    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    brls::FontLoader::USER_FONT_PATH = "romfs:/font/switch_font.ttf";
    log_stage("Borealis bundled Switch font path set to romfs:/font/switch_font.ttf");
    log_stage("before Borealis Application::init");
    if (!brls::Application::init())
    {
        log_stage("Borealis Application::init FAILED");
        romfsExit();
        close_debug_logs();
        return EXIT_FAILURE;
    }
    log_stage("Borealis Application::init OK");

    log_stage("before createWindow");
    brls::Application::createWindow("SaikouTV NX");
    log_stage("createWindow returned");

    const int regularFont = brls::Application::getFont(brls::FONT_REGULAR);
    const int defaultFont = brls::Application::getDefaultFont();
    const int fallbackBaseFont = regularFont >= 0 ? regularFont : defaultFont;
#ifdef __SWITCH__
    NVGcontext* vg = brls::Application::getNVGContext();
    int fallbackCount = 0;
    const std::string fallbackFonts[] = {
        brls::FONT_CHINESE_SIMPLIFIED,
        brls::FONT_CHINESE_SIMPLIFIED_EXT,
        brls::FONT_CHINESE_TRADITIONAL,
        brls::FONT_KOREAN_REGULAR,
        brls::FONT_SWITCH_ICONS,
        brls::FONT_MATERIAL_ICONS,
    };
    for (const std::string& name : fallbackFonts)
    {
        const int fallbackFont = brls::Application::getFont(name);
        if (vg && fallbackBaseFont >= 0 && fallbackFont >= 0 && fallbackFont != fallbackBaseFont)
        {
            nvgAddFallbackFontId(vg, fallbackBaseFont, fallbackFont);
            ++fallbackCount;
        }
    }
#endif

    char fontMarker[192];
    std::snprintf(fontMarker, sizeof(fontMarker),
        "After createWindow: bundled font file=%d; regular=%d zh-Hans=%d default=%d fallback-base=%d fallbacks=%d",
        access("romfs:/font/switch_font.ttf", F_OK) == 0 ? 1 : 0,
        regularFont,
        brls::Application::getFont(brls::FONT_CHINESE_SIMPLIFIED),
        defaultFont,
        fallbackBaseFont,
#ifdef __SWITCH__
        fallbackCount
#else
        0
#endif
    );
    log_stage(fontMarker);
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    log_stage("Borealis dark theme applied");
    brls::Application::setGlobalQuit(true);
    log_stage("Borealis built-in + exit action enabled");
    log_stage("before pushActivity");
    HomeActivity* homeActivity = new HomeActivity();
    brls::Application::pushActivity(homeActivity, brls::TransitionAnimation::NONE);
    log_stage("pushActivity returned");

    if (brls::View* focus = brls::Application::getCurrentFocus())
    {
        const std::string description = focus->describe();
        char marker[256];
        std::snprintf(marker, sizeof(marker), "Borealis initial focus after push: %.220s", description.c_str());
        log_stage(marker);
    }
    else
    {
        log_stage("Borealis initial focus after push: NULL");
    }
    if (g_homeView)
    {
        const auto stack = brls::Application::getActivitiesStack();
        char marker[192];
        std::snprintf(marker, sizeof(marker),
            "Home after push: size=%.0fx%.0f alpha=%.2f hidden=%d activities=%zu",
            g_homeView->getWidth(), g_homeView->getHeight(), g_homeView->getAlpha(),
            g_homeView->isHidden() ? 1 : 0, stack.size());
        log_stage(marker);
        g_homeView->setAlpha(1.0f);
        log_stage("Home root alpha forced to 1");
    }

    log_stage("before first mainLoop frame");
    int frameCount = 0;
    bool focusLoggedAfterFirstFrame = false;
    while (true)
    {
        const bool running = brls::Application::mainLoop();
        log_controller_edges();
        homeActivity->tick();
        tick_live_ui_activities();

        if (!focusLoggedAfterFirstFrame)
        {
            focusLoggedAfterFirstFrame = true;
            if (brls::View* focus = brls::Application::getCurrentFocus())
            {
                const std::string description = focus->describe();
                char marker[256];
                std::snprintf(marker, sizeof(marker), "Borealis focus after first frame: %.220s", description.c_str());
                log_stage(marker);
            }
            else
            {
                log_stage("Borealis focus after first frame: NULL");
            }
        }

        if (!running)
            break;

        ++frameCount;
        if (frameCount <= 10)
        {
            char marker[96];
            std::snprintf(marker, sizeof(marker), "mainLoop returned true #%d", frameCount);
            log_stage(marker);
        }
    }

    log_stage("mainLoop returned false");
    romfsExit();
    log_stage("romfsExit returned");
    close_debug_logs();
    return EXIT_SUCCESS;
}
