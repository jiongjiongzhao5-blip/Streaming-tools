#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using LogCallback = std::function<void(const std::string&)>;

enum class SourceType {
    Screen,
    Camera,
};

enum class AudioSourceType {
    SystemAudio,
    Microphone,
};

struct StreamTarget {
    std::string platform;
    std::string url;
};

struct StreamConfig {
    SourceType sourceType = SourceType::Screen;
    AudioSourceType audioType = AudioSourceType::SystemAudio;

    int cameraIndex = 0;

    int audioDeviceIndex = -1;

    int width = 1920;
    int height = 1080;
    int fps = 30;
    int videoKbps = 4500;
    int audioKbps = 128;

    int sampleRate = 48000;
    int channels = 2;

    std::vector<StreamTarget> targets;
};

struct VideoFrame {
    int width = 0;
    int height = 0;
    int64_t pts = 0;

    std::vector<uint8_t> bgra;
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

struct AudioChunk {
    int sampleRate = 0;
    int channels = 0;
    int64_t pts = 0;
    std::vector<float> samples;
};

using AudioChunkPtr = std::shared_ptr<AudioChunk>;
