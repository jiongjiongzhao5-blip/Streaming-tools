#pragma once

#include "core/common/Common.h"

#include <cstdint>
#include <memory>
#include <string>

struct PipelineStats {
    int64_t videoFramesCaptured = 0;
    int64_t videoPacketsEncoded = 0;
    int64_t videoFramesDropped = 0;
    int64_t audioChunksCaptured = 0;
    int64_t audioPacketsEncoded = 0;

    int connectedTargets = 0;
    int totalTargets = 0;

    double elapsedSeconds = 0.0;
};

class StreamPipeline {
public:
    StreamPipeline();
    ~StreamPipeline();

    StreamPipeline(const StreamPipeline&) = delete;
    StreamPipeline& operator=(const StreamPipeline&) = delete;

    bool start(const StreamConfig& config, LogCallback log);

    void stop();

    bool isRunning() const;

    PipelineStats stats() const;

    int reconnectDisconnected();

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
