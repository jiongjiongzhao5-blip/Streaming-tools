#include "core/pipeline/StreamPipeline.h"

#include "core/capture/AudioCapturer.h"
#include "core/capture/CameraCapturer.h"
#include "core/capture/CameraFrameConverter.h"
#include "core/capture/ScreenCapturer.h"
#include "core/common/BoundedQueue.h"
#include "core/encode/AudioEncoder.h"
#include "core/encode/VideoEncoder.h"
#include "core/output/StreamOutput.h"

#include <windows.h>

#include <mmsystem.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int64_t msSince(Clock::time_point from, Clock::time_point now)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - from).count();
}

}

struct StreamPipeline::Impl {
    StreamConfig config;
    LogCallback log;
    std::string lastError;

    std::mutex logMutex;

    std::mutex previewMutex;
    PreviewCallback previewCallback;

    std::atomic<bool> running{false};

    ScreenCapturer screen;
    CameraCapturer camera;
    CameraFrameConverter cameraConverter;
    AudioCapturer audio;

    VideoEncoder videoEncoder;
    AudioEncoder audioEncoder;

    StreamOutput output;

    std::thread videoThread;
    std::thread audioThread;
    std::thread writeThread;

    BoundedQueue<AVPacketPtr> videoQueue{24};
    BoundedQueue<AVPacketPtr> audioQueue{256};

    SwsContext* scaleContext = nullptr;
    int scaleSrcWidth = 0;
    int scaleSrcHeight = 0;

    std::atomic<int64_t> videoFrames{0};
    std::atomic<int64_t> videoPackets{0};
    std::atomic<int64_t> videoDropped{0};
    std::atomic<int64_t> audioChunks{0};
    std::atomic<int64_t> audioPackets{0};

    Clock::time_point clockStart;

    bool timerRaised = false;

    void videoLoop();
    void audioLoop();
    void writeLoop();

    void emit(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (log)
            log(message);
    }

    LogCallback guardedLog()
    {
        return [this](const std::string& message) { emit(message); };
    }

    VideoFramePtr scaleToTarget(const VideoFramePtr& frame);
    void releaseScale()
    {
        if (scaleContext) {
            sws_freeContext(scaleContext);
            scaleContext = nullptr;
        }
        scaleSrcWidth = 0;
        scaleSrcHeight = 0;
    }
};

VideoFramePtr StreamPipeline::Impl::scaleToTarget(const VideoFramePtr& frame)
{
    if (!frame)
        return nullptr;

    if (frame->width == config.width && frame->height == config.height)
        return frame;

    if (!scaleContext || scaleSrcWidth != frame->width
        || scaleSrcHeight != frame->height) {
        releaseScale();
        scaleContext = sws_getContext(frame->width, frame->height, AV_PIX_FMT_BGRA,
                                      config.width, config.height, AV_PIX_FMT_BGRA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaleContext) {
            emit("创建缩放上下文失败");
            return nullptr;
        }
        scaleSrcWidth = frame->width;
        scaleSrcHeight = frame->height;
    }

    auto out = std::make_shared<VideoFrame>();
    out->width = config.width;
    out->height = config.height;
    out->pts = frame->pts;
    out->bgra.resize(static_cast<size_t>(config.width) * config.height * 4);

    const uint8_t* srcData[4] = {frame->bgra.data(), nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {frame->width * 4, 0, 0, 0};
    uint8_t* dstData[4] = {out->bgra.data(), nullptr, nullptr, nullptr};
    const int dstLinesize[4] = {config.width * 4, 0, 0, 0};

    sws_scale(scaleContext, srcData, srcLinesize, 0, frame->height, dstData,
              dstLinesize);

    return out;
}

void StreamPipeline::Impl::videoLoop()
{
    const bool isScreen = (config.sourceType == SourceType::Screen);
    const int fps = std::max(1, config.fps);
    const auto frameInterval = std::chrono::microseconds(1000000 / fps);

    auto nextFrameAt = Clock::now();

    const auto waitNextFrame = [&]() {
        if (!isScreen)
            return;
        nextFrameAt += frameInterval;
        const auto after = Clock::now();
        if (nextFrameAt < after)
            nextFrameAt = after;
        std::this_thread::sleep_until(nextFrameAt);
    };

    bool firstFrame = true;
    Clock::time_point firstFrameAt;
    int64_t lastPts = -1;

    while (running) {
        VideoFramePtr frame;

        if (isScreen) {
            frame = screen.capture();
        } else {
            CameraRawFramePtr raw = camera.nextFrame(1000);
            if (!raw) {
                continue;
            }
            frame = cameraConverter.convert(*raw);
            if (!frame) {
                if (log && !cameraConverter.lastError().empty())
                    log("摄像头帧转换失败: " + cameraConverter.lastError());
                continue;
            }
        }

        if (!frame)
            continue;

        const auto now = Clock::now();
        if (firstFrame) {
            firstFrameAt = now;
            firstFrame = false;
        }

        const int64_t elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - firstFrameAt)
                .count();
        const int64_t pts = (elapsedUs * fps + 500000) / 1000000;

        if (pts <= lastPts) {
            ++videoDropped;
            waitNextFrame();
            continue;
        }
        lastPts = pts;

        frame->pts = pts;

        frame = scaleToTarget(frame);
        if (!frame) {
            waitNextFrame();
            continue;
        }

        {
            PreviewCallback callback;
            {
                std::lock_guard<std::mutex> lock(previewMutex);
                callback = previewCallback;
            }
            if (callback)
                callback(frame);
        }

        for (auto& packet : videoEncoder.encode(*frame)) {
            videoQueue.push(std::move(packet));
            ++videoPackets;
        }
        ++videoFrames;

        waitNextFrame();
    }

    for (auto& packet : videoEncoder.flush())
        videoQueue.push(std::move(packet));
}

void StreamPipeline::Impl::audioLoop()
{
    while (running) {
        AudioChunkPtr chunk = audio.nextChunk(100);
        if (!chunk)
            continue;

        ++audioChunks;
        for (auto& packet : audioEncoder.encode(*chunk)) {
            audioQueue.push(std::move(packet));
            ++audioPackets;
        }
    }

    for (auto& packet : audioEncoder.flush())
        audioQueue.push(std::move(packet));
}

void StreamPipeline::Impl::writeLoop()
{
    while (running || !videoQueue.empty() || !audioQueue.empty()) {
        bool didWork = false;

        if (auto packet = audioQueue.tryPop(1)) {
            output.writeAudio(**packet, audioEncoder.timeBase());
            didWork = true;
        }
        if (auto packet = videoQueue.tryPop(1)) {
            output.writeVideo(**packet, videoEncoder.timeBase());
            didWork = true;
        }

        if (!didWork) {
            if (!running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

StreamPipeline::StreamPipeline() : impl_(std::make_unique<Impl>())
{
}

StreamPipeline::~StreamPipeline()
{
    stop();
}

void StreamPipeline::setPreviewCallback(PreviewCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->previewMutex);
    impl_->previewCallback = std::move(callback);
}

bool StreamPipeline::start(const StreamConfig& config, LogCallback log)
{
    stop();

    impl_->config = config;
    impl_->log = std::move(log);
    impl_->lastError.clear();

    const LogCallback say = impl_->guardedLog();
    const auto fail = [this, &say](const std::string& reason) {
        say(reason);
        stop();
        impl_->lastError = reason;
        return false;
    };

    if (config.width <= 0 || config.height <= 0 || config.fps <= 0)
        return fail("推流参数无效：宽高和帧率都要大于 0");

    if (config.targets.empty())
        return fail("没有配置推流目标");

    impl_->config.width = config.width & ~1;
    impl_->config.height = config.height & ~1;
    if (impl_->config.width != config.width || impl_->config.height != config.height) {
        say("宽高已对齐到偶数: " + std::to_string(impl_->config.width) + "x"
            + std::to_string(impl_->config.height));
    }

    VideoEncoderConfig videoConfig;
    videoConfig.width = impl_->config.width;
    videoConfig.height = impl_->config.height;
    videoConfig.fps = impl_->config.fps;
    videoConfig.bitrateKbps = impl_->config.videoKbps;

    if (!impl_->videoEncoder.open(videoConfig, say))
        return fail("视频编码器打开失败: " + impl_->videoEncoder.lastError());

    AudioEncoderConfig audioConfig;
    audioConfig.sampleRate = config.sampleRate;
    audioConfig.channels = config.channels;
    audioConfig.bitrateKbps = config.audioKbps;

    if (!impl_->audioEncoder.open(audioConfig, say))
        return fail("音频编码器打开失败: " + impl_->audioEncoder.lastError());

    if (!impl_->output.open(config.targets, impl_->videoEncoder.codecParameters(),
                            impl_->audioEncoder.codecParameters(), say)) {
        return fail("推流输出打开失败: " + impl_->output.lastError());
    }

    if (config.sourceType == SourceType::Screen) {
        if (!impl_->screen.init(say))
            return fail("屏幕采集初始化失败");
    } else {
        if (!impl_->camera.open(config.cameraIndex, say))
            return fail("摄像头打开失败: 索引 " + std::to_string(config.cameraIndex));
    }

    if (!impl_->audio.open(config.audioType, config.audioDeviceIndex, say))
        return fail("音频采集打开失败");

    timeBeginPeriod(1);
    impl_->timerRaised = true;

    impl_->running = true;
    impl_->clockStart = Clock::now();

    impl_->videoThread = std::thread([this] { impl_->videoLoop(); });
    impl_->audioThread = std::thread([this] { impl_->audioLoop(); });
    impl_->writeThread = std::thread([this] { impl_->writeLoop(); });

    say("推流已启动: " + std::to_string(impl_->config.width) + "x"
        + std::to_string(impl_->config.height) + " @"
        + std::to_string(impl_->config.fps) + "fps, "
        + std::to_string(impl_->output.connectedCount()) + "/"
        + std::to_string(config.targets.size()) + " 路");

    return true;
}

void StreamPipeline::stop()
{
    const auto lowerTimer = [this]() {
        if (impl_->timerRaised) {
            timeEndPeriod(1);
            impl_->timerRaised = false;
        }
    };

    if (!impl_->running && !impl_->videoThread.joinable()
        && !impl_->audioThread.joinable() && !impl_->writeThread.joinable()) {
        impl_->screen.close();
        impl_->camera.close();
        impl_->audio.close();
        impl_->videoEncoder.close();
        impl_->audioEncoder.close();
        impl_->output.close();
        impl_->videoQueue.clear();
        impl_->audioQueue.clear();
        impl_->releaseScale();
        lowerTimer();
        return;
    }

    impl_->running = false;

    if (impl_->videoThread.joinable())
        impl_->videoThread.join();
    if (impl_->audioThread.joinable())
        impl_->audioThread.join();
    if (impl_->writeThread.joinable())
        impl_->writeThread.join();

    impl_->output.flush();

    impl_->screen.close();
    impl_->camera.close();
    impl_->audio.close();

    impl_->videoEncoder.close();
    impl_->audioEncoder.close();

    impl_->output.close();

    impl_->videoQueue.clear();
    impl_->audioQueue.clear();
    impl_->releaseScale();

    lowerTimer();
    impl_->emit("推流已停止");
}

bool StreamPipeline::isRunning() const
{
    return impl_->running;
}

PipelineStats StreamPipeline::stats() const
{
    PipelineStats result;
    result.videoFramesCaptured = impl_->videoFrames.load();
    result.videoPacketsEncoded = impl_->videoPackets.load();
    result.videoFramesDropped = impl_->videoDropped.load();
    result.audioChunksCaptured = impl_->audioChunks.load();
    result.audioPacketsEncoded = impl_->audioPackets.load();

    const std::vector<OutputTargetInfo> targets = impl_->output.targets();
    result.totalTargets = static_cast<int>(targets.size());
    result.connectedTargets = impl_->output.connectedCount();

    if (impl_->running) {
        result.elapsedSeconds =
            static_cast<double>(msSince(impl_->clockStart, Clock::now())) / 1000.0;
    }

    return result;
}

int StreamPipeline::reconnectDisconnected()
{
    return impl_->output.reconnectDisconnected();
}

const std::string& StreamPipeline::lastError() const
{
    return impl_->lastError;
}
