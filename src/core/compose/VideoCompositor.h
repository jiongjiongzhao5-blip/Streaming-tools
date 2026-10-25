#pragma once

#include "core/common/Common.h"

#include <memory>
#include <string>

struct CompositorConfig {
    int canvasWidth = 1280;
    int canvasHeight = 720;

    bool pipEnabled = false;
    PipCorner pipCorner = PipCorner::BottomRight;
    int pipWidth = 0;

    std::string watermarkText;
    int watermarkFontSize = 28;
};

class VideoCompositor {
public:
    VideoCompositor();
    ~VideoCompositor();

    VideoCompositor(const VideoCompositor&) = delete;
    VideoCompositor& operator=(const VideoCompositor&) = delete;

    bool configure(const CompositorConfig& config, LogCallback log);
    void close();
    bool isConfigured() const;

    VideoFramePtr compose(const VideoFrame& base, const VideoFrame* overlay);

    int pipX() const;
    int pipY() const;
    int pipWidth() const;
    int pipHeight() const;

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
