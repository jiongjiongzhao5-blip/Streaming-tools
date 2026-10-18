#pragma once

#include "core/capture/CameraCapturer.h"
#include "core/common/Common.h"

#include <memory>
#include <string>

class CameraFrameConverter {
public:
    CameraFrameConverter();
    ~CameraFrameConverter();

    CameraFrameConverter(const CameraFrameConverter&) = delete;
    CameraFrameConverter& operator=(const CameraFrameConverter&) = delete;

    VideoFramePtr convert(const CameraRawFrame& raw);

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
