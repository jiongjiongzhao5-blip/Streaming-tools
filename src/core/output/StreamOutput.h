#pragma once

#include "core/common/Common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include <memory>
#include <string>
#include <vector>

struct OutputTargetInfo {
    std::string name;
    std::string url;
    bool connected = false;
    int64_t packetsSent = 0;
    int64_t bytesSent = 0;
    std::string lastError;

    int retryCount = 0;

    int nextRetryInMs = 0;
};

class StreamOutput {
public:
    StreamOutput();
    ~StreamOutput();

    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;

    bool open(const std::vector<StreamTarget>& targets,
              const AVCodecParameters* videoParams,
              const AVCodecParameters* audioParams, LogCallback log);

    void close();

    bool writeVideo(const AVPacket& packet, AVRational encoderTimeBase);
    bool writeAudio(const AVPacket& packet, AVRational encoderTimeBase);

    void flush();

    std::vector<OutputTargetInfo> targets() const;

    int connectedCount() const;

    int pollReconnect();

    int reconnectDisconnected();

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
