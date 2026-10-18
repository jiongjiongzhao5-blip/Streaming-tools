#pragma once

#include "core/common/Common.h"

#include <memory>
#include <string>
#include <vector>

class AudioCapturer {
public:
    AudioCapturer();
    ~AudioCapturer();

    AudioCapturer(const AudioCapturer&) = delete;
    AudioCapturer& operator=(const AudioCapturer&) = delete;

    static std::vector<std::string> listDevices(AudioSourceType type);

    bool open(AudioSourceType type, int deviceIndex, LogCallback log);

    void close();

    AudioChunkPtr nextChunk(int timeoutMs);

    int sampleRate() const;
    int channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
