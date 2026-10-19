#pragma once

#include "core/common/Common.h"
#include "core/encode/EncoderCommon.h"

#include <memory>
#include <string>
#include <vector>

struct AudioEncoderConfig {
    int sampleRate = 48000;
    int channels = 2;
    int bitrateKbps = 128;
};

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    bool open(const AudioEncoderConfig& config, LogCallback log);
    void close();

    std::vector<AVPacketPtr> encode(const AudioChunk& chunk);

    std::vector<AVPacketPtr> flush();

    bool isOpen() const;

    AVRational timeBase() const;

    const AVCodecParameters* codecParameters() const;

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
