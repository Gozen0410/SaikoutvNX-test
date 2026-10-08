#pragma once

#include <borealis.hpp>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <string>
#include <vector>

struct SaikouMpvPlayerSubtitle
{
    std::string url;
    std::string label;
    std::string language;
};

class SaikouMpvVideoView : public brls::View
{
public:
    explicit SaikouMpvVideoView(std::string url, std::vector<std::string> headers = {},
                                std::vector<SaikouMpvPlayerSubtitle> subtitles = {});
    ~SaikouMpvVideoView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    void togglePause();

private:
    void loadSubtitles();
    void handleEvents();
    bool ensureTarget(NVGcontext* vg, int width, int height);
    void releaseTarget(NVGcontext* vg);

    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_render = nullptr;
    std::vector<std::string> m_headers;
    std::vector<SaikouMpvPlayerSubtitle> m_subtitles;
    NVGcontext* m_vg = nullptr;
    unsigned int m_fbo = 0;
    unsigned int m_texture = 0;
    int m_nvgImage = -1;
    int m_targetWidth = 0;
    int m_targetHeight = 0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    bool m_framePending = true;
    std::string m_status = "Connecting to stream...";
};

class SaikouMpvPlayerActivity : public brls::Activity
{
public:
    SaikouMpvPlayerActivity(std::string animeTitle, std::string episodeTitle,
                            std::string streamLabel, std::string url, std::vector<std::string> headers = {},
                            std::vector<SaikouMpvPlayerSubtitle> subtitles = {});
    brls::View* createContentView() override;

private:
    std::string m_animeTitle;
    std::string m_episodeTitle;
    std::string m_streamLabel;
    std::string m_url;
    std::vector<std::string> m_headers;
    std::vector<SaikouMpvPlayerSubtitle> m_subtitles;
    SaikouMpvVideoView* m_video = nullptr;
};
