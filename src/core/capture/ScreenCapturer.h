#pragma once

#include "core/common/Common.h"

#include <memory>

class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    bool init(LogCallback log);

    void close();

    VideoFramePtr capture();

private:
    VideoFramePtr captureDxgi();
    VideoFramePtr captureGdi();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool dxgiReady_ = false;
};
