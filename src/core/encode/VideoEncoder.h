#pragma once

#include "core/common/Common.h"
#include "core/encode/EncoderCommon.h"

#include <memory>
#include <string>
#include <vector>

struct VideoEncoderConfig {
    int width = 1920;
    int height = 1080;
    int fps = 30;

    int bitrateKbps = 4500;

    int gopSize = 0;

    int maxBFrames = 0;

    std::string preset = "veryfast";
    std::string tune = "zerolatency";
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool open(const VideoEncoderConfig& config, LogCallback log);
    void close();

    std::vector<AVPacketPtr> encode(const VideoFrame& frame);

    std::vector<AVPacketPtr> flush();

    bool isOpen() const;

    AVRational timeBase() const;

    const AVCodecParameters* codecParameters() const;

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
