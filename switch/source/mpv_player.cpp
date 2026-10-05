#include "mpv_player.hpp"

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <atomic>
#include <cctype>
#include <utility>
#include <vector>

static void* saikou_mpv_get_proc_address(void*, const char* name)
{
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

extern "C"
{
    int nvglCreateImageFromHandleGL3(NVGcontext* ctx, unsigned int texture, int w, int h, int flags);
    int nvglCreateImageFromHandleGLES3(NVGcontext* ctx, unsigned int texture, int w, int h, int flags);
}

#ifdef USE_GLES
#define nvglCreateImageFromHandle nvglCreateImageFromHandleGLES3
#else
#define nvglCreateImageFromHandle nvglCreateImageFromHandleGL3
#endif

extern void saikou_debug_log(const char* stage);

namespace
{
constexpr int kNvgImageNoDelete = 1 << 16;
std::atomic<bool> gFramePending{true};

std::string redact_mpv_urls(std::string text)
{
    for (const char* scheme : { "https://", "http://" }) {
        size_t pos = 0;
        while ((pos = text.find(scheme, pos)) != std::string::npos) {
            size_t end = pos;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
                   text[end] != '"' && text[end] != 39)
                ++end;
            text.replace(pos, end - pos, "[url]");
            pos += 5;
        }
    }
    if (text.size() > 220) text.resize(220);
    return text;
}

const char* mpv_log_level_name(mpv_log_level level)
{
    switch (level)
    {
        case MPV_LOG_LEVEL_WARN: return "warn";
        case MPV_LOG_LEVEL_ERROR: return "error";
        case MPV_LOG_LEVEL_FATAL: return "fatal";
        default: return nullptr;
    }
}

void onMpvRenderUpdate(void*)
{
    gFramePending.store(true, std::memory_order_release);
}
}


SaikouMpvVideoView::SaikouMpvVideoView(std::string url, std::vector<std::string> headers)
    : m_headers(std::move(headers))
{
    setFocusable(false);
    m_mpv = mpv_create();
    if (!m_mpv)
    {
        m_status = "Could not create the video player.";
        brls::Logger::error("mpv_create failed");
        return;
    }

    // The resolved scraper URL is passed straight to libmpv. libmpv/FFmpeg
    // handles HLS playlists, MP4 streams, redirects, and media probing.
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "ytdl", "no");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "network-timeout", "30");
    mpv_set_option_string(m_mpv, "tls-verify", "no");
    mpv_set_option_string(m_mpv, "hwdec", "auto");
    // Some anime CDNs use image-like file extensions for HLS segments.
    mpv_set_option_string(m_mpv, "demuxer-lavf-o", "extension_picky=0");
    // KAA CDNs validate the page that produced the HLS URL. These headers must
    // follow libmpv into both the master/variant playlists and media segments.
    mpv_set_option_string(m_mpv, "http-user-agent",
        "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/129.0.0.0 Mobile Safari/537.36");
    if (!m_headers.empty())
    {
        std::string fields;
        for (size_t i = 0; i < m_headers.size(); ++i)
        {
            if (i) fields += ",";
            fields += m_headers[i];
        }
        mpv_set_option_string(m_mpv, "http-header-fields", fields.c_str());
        brls::Logger::info("mpv stream headers configured count={}", m_headers.size());
    }
#ifdef __SWITCH__
    mpv_set_option_string(m_mpv, "vd-lavc-dr", "no");
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "4");
    mpv_set_option_string(m_mpv, "opengl-glfinish", "yes");
#endif
    const int initResult = mpv_initialize(m_mpv);
    if (initResult < 0)
    {
        m_status = std::string("Player initialization failed: ") + mpv_error_string(initResult);
        brls::Logger::error("mpv_initialize failed: {}", mpv_error_string(initResult));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    mpv_opengl_init_params glInit{saikou_mpv_get_proc_address, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int renderResult = mpv_render_context_create(&m_render, m_mpv, params);
    if (renderResult < 0)
    {
        m_status = std::string("Video renderer initialization failed: ") + mpv_error_string(renderResult);
        brls::Logger::error("mpv_render_context_create failed: {}", mpv_error_string(renderResult));
        return;
    }

    mpv_render_context_set_update_callback(m_render, onMpvRenderUpdate, nullptr);
    mpv_request_log_messages(m_mpv, "warn");
    mpv_observe_property(m_mpv, 0, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "dheight", MPV_FORMAT_INT64);

    std::vector<const char*> command = {"loadfile", url.c_str(), "replace", nullptr};
    const int loadResult = mpv_command_async(m_mpv, 0, command.data());
    if (loadResult < 0)
    {
        m_status = std::string("Could not open stream: ") + mpv_error_string(loadResult);
        brls::Logger::error("mpv loadfile failed: {}", mpv_error_string(loadResult));
    }
    brls::Logger::info("Starting native playback");
}

SaikouMpvVideoView::~SaikouMpvVideoView()
{
    if (m_vg)
        releaseTarget(m_vg);
    else
    {
        if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
        if (m_texture) glDeleteTextures(1, &m_texture);
    }
    if (m_render)
        mpv_render_context_free(m_render);
    if (m_mpv)
        mpv_terminate_destroy(m_mpv);
}

void SaikouMpvVideoView::togglePause()
{
    if (!m_mpv)
        return;
    const char* command[] = {"cycle", "pause", nullptr};
    const int result = mpv_command_async(m_mpv, 0, command);
    if (result < 0)
        brls::Logger::error("mpv pause toggle failed: {}", mpv_error_string(result));
    m_status = "Playback controls: A pause/resume  |  B back";
}

void SaikouMpvVideoView::handleEvents()
{
    if (!m_mpv)
        return;
    while (true)
    {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id)
        {
            case MPV_EVENT_LOG_MESSAGE:
            {
                auto* message = static_cast<mpv_event_log_message*>(event->data);
                const char* level = message ? mpv_log_level_name(message->log_level) : nullptr;
                if (message && level && message->text)
                {
                    char marker[320];
                    const std::string safeText = redact_mpv_urls(message->text);
                    std::snprintf(marker, sizeof(marker), "MPV %s %s: %s",
                        level, message->prefix ? message->prefix : "player",
                        safeText.c_str());
                    saikou_debug_log(marker);
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
            {
                int64_t value = 0;
                if (mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &value) >= 0 && value > 0)
                    m_videoWidth = static_cast<int>(value);
                if (mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &value) >= 0 && value > 0)
                    m_videoHeight = static_cast<int>(value);
                m_framePending = true;
                m_status = "Playing  |  A pause/resume  |  B back";
                brls::Logger::info("mpv stream loaded video={}x{}", m_videoWidth, m_videoHeight);
                saikou_debug_log("MPV FILE LOADED");
                break;
            }
            case MPV_EVENT_END_FILE:
            {
                auto* end = static_cast<mpv_event_end_file*>(event->data);
                if (end && end->reason == MPV_END_FILE_REASON_ERROR)
                {
                    m_status = std::string("Playback error: ") + mpv_error_string(end->error);
                    brls::Logger::error("mpv playback ended with error: {}", m_status);
                    const std::string safeStatus = redact_mpv_urls(m_status);
                    saikou_debug_log(("MPV PLAYBACK ERROR " + safeStatus).c_str());
                }
                else
                {
                    m_status = "Stream ended  |  B back";
                    saikou_debug_log("MPV STREAM ENDED");
                }
                break;
            }
            default:
                break;
        }
    }
}

bool SaikouMpvVideoView::ensureTarget(NVGcontext* vg, int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_fbo != 0 && width == m_targetWidth && height == m_targetHeight)
        return true;

    releaseTarget(vg);

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint previousFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        brls::Logger::error("mpv video FBO incomplete: 0x{:x}", status);
        releaseTarget(vg);
        return false;
    }

    m_nvgImage = nvglCreateImageFromHandle(vg, m_texture, width, height, kNvgImageNoDelete);
    if (m_nvgImage < 0)
    {
        brls::Logger::error("failed to adopt mpv video texture into NanoVG");
        releaseTarget(vg);
        return false;
    }

    m_targetWidth = width;
    m_targetHeight = height;
    brls::Logger::info("mpv video render target ready {}x{}", width, height);
    return true;
}

void SaikouMpvVideoView::releaseTarget(NVGcontext* vg)
{
    if (m_nvgImage >= 0 && vg)
    {
        nvgDeleteImage(vg, m_nvgImage);
        m_nvgImage = -1;
    }
    if (m_fbo)
    {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_texture)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    m_targetWidth = m_targetHeight = 0;
}

void SaikouMpvVideoView::draw(NVGcontext* vg, float x, float y, float width,
                              float height, brls::Style style, brls::FrameContext* ctx)
{
    handleEvents();
    m_vg = vg;

    if (m_render && m_videoWidth > 0 && m_videoHeight > 0 &&
        ensureTarget(vg, m_videoWidth, m_videoHeight))
    {
        const uint64_t updateFlags = mpv_render_context_update(m_render);
        if (updateFlags & MPV_RENDER_UPDATE_FRAME)
            m_framePending = true;

        if (m_framePending)
        {
            GLint previousFbo = 0;
            GLint previousViewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
            glGetIntegerv(GL_VIEWPORT, previousViewport);

            mpv_opengl_fbo target{static_cast<int>(m_fbo), m_videoWidth, m_videoHeight, 0};
            int flipY = 0;
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &target},
                {MPV_RENDER_PARAM_FLIP_Y, &flipY},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            const int result = mpv_render_context_render(m_render, params);
            if (result < 0)
                brls::Logger::error("mpv render failed: {}", mpv_error_string(result));
            else
                m_framePending = false;

            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
            glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
            mpv_render_context_report_swap(m_render);
        }

        NVGpaint video = nvgImagePattern(vg, x, y, width, height, 0.0f, m_nvgImage, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, video);
        nvgFill(vg);
    }

    if (!m_status.empty())
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 16.0f, y + height - 48.0f, width - 32.0f, 34.0f, 5.0f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 190));
        nvgFill(vg);
        nvgFontSize(vg, 16.0f);
        nvgFillColor(vg, nvgRGB(235, 239, 246));
        nvgText(vg, x + 28.0f, y + height - 26.0f, m_status.c_str(), nullptr);
    }
}

SaikouMpvPlayerActivity::SaikouMpvPlayerActivity(
    std::string animeTitle, std::string episodeTitle, std::string streamLabel, std::string url,
    std::vector<std::string> headers)
    : m_animeTitle(std::move(animeTitle)),
      m_episodeTitle(std::move(episodeTitle)),
      m_streamLabel(std::move(streamLabel)),
      m_url(std::move(url)),
      m_headers(std::move(headers))
{
}

brls::View* SaikouMpvPlayerActivity::createContentView()
{
    brls::Box* root = new brls::Box(brls::Axis::COLUMN);
    root->setWidthPercentage(100.0f);
    root->setHeightPercentage(100.0f);
    root->setPadding(24.0f);
    root->setBackgroundColor(nvgRGB(16, 20, 29));
    root->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        brls::sync([] {
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [] {}, true);
        });
        return true;
    });
    root->registerAction("Pause or resume playback", brls::BUTTON_A, [this](brls::View*) {
        if (m_video)
            m_video->togglePause();
        return true;
    });

    brls::Label* title = new brls::Label();
    title->setText(m_animeTitle);
    title->setFontSize(23.0f);
    title->setTextColor(nvgRGB(244, 246, 250));
    root->addView(title);

    brls::Label* episode = new brls::Label();
    episode->setText(m_episodeTitle + "  •  " + m_streamLabel);
    episode->setFontSize(16.0f);
    episode->setTextColor(nvgRGB(174, 184, 200));
    episode->setMargins(0, 4, 0, 0);
    root->addView(episode);

    m_video = new SaikouMpvVideoView(m_url, m_headers);
    m_video->setWidthPercentage(100.0f);
    m_video->setGrow(1.0f);
    m_video->setMargins(0, 12, 0, 0);
    root->addView(m_video);

    brls::Label* controls = new brls::Label();
    controls->setText("A  Pause/resume     B  Back");
    controls->setFontSize(14.0f);
    controls->setTextColor(nvgRGB(135, 147, 166));
    controls->setMargins(0, 9, 0, 0);
    controls->setFocusable(false);
    root->addView(controls);

    // Keep the activity itself focused so A/B actions work even though the
    // full-screen video surface and its explanatory labels are not focusable.
    root->setFocusable(true);
    brls::Application::giveFocus(root);
    return root;
}
