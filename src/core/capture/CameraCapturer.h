#pragma once

#include "core/common/Common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class CameraPixelFormat {
    Unknown = 0,
    RGB24,
    RGB32,
    YUY2,
    UYVY,
    MJPG,
};

const char* cameraPixelFormatName(CameraPixelFormat format);

struct CameraRawFrame {
    int width = 0;
    int height = 0;

    int stride = 0;

    CameraPixelFormat format = CameraPixelFormat::Unknown;

    bool flipVertical = false;

    int64_t pts = 0;

    std::vector<uint8_t> bytes;
};

using CameraRawFramePtr = std::shared_ptr<CameraRawFrame>;

class CameraCapturer {
public:
    CameraCapturer();
    ~CameraCapturer();

    CameraCapturer(const CameraCapturer&) = delete;
    CameraCapturer& operator=(const CameraCapturer&) = delete;

    static std::vector<std::string> listDevices();

    bool open(int deviceIndex, LogCallback log,
              CameraPixelFormat requestFormat = CameraPixelFormat::RGB24);

    void close();

    CameraRawFramePtr nextFrame(int timeoutMs);

    int width() const;
    int height() const;
    CameraPixelFormat pixelFormat() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
