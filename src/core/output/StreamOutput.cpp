#include "core/output/StreamOutput.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <cstring>

namespace {

std::string avErrorText(int err)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

constexpr AVRational kFlvTimeBase = {1, 1000};

constexpr const char* kIoTimeout = "5000000";

}

struct StreamOutput::Impl {
    struct Target {
        std::string name;
        std::string url;

        AVFormatContext* ctx = nullptr;
        AVStream* videoStream = nullptr;
        AVStream* audioStream = nullptr;

        bool connected = false;
        std::string lastError;

        int64_t packetsSent = 0;
        int64_t bytesSent = 0;
    };

    std::vector<Target> targets;

    AVCodecParameters* videoParams = nullptr;
    AVCodecParameters* audioParams = nullptr;

    bool audioOffsetReady = false;
    int64_t audioOffset = 0;

    LogCallback log;
    std::string lastError;

    bool openTarget(Target& target);
    void closeTarget(Target& target, bool writeTrailer);
    bool writePacket(const AVPacket& packet, AVRational timeBase, bool isVideo);

    void release()
    {
        for (Target& target : targets)
            closeTarget(target, true);
        targets.clear();

        if (videoParams) {
            avcodec_parameters_free(&videoParams);
            videoParams = nullptr;
        }
        if (audioParams) {
            avcodec_parameters_free(&audioParams);
            audioParams = nullptr;
        }

        audioOffsetReady = false;
        audioOffset = 0;
    }
};

bool StreamOutput::Impl::openTarget(Target& target)
{
    closeTarget(target, false);

    if (!videoParams) {
        target.lastError = "没有视频编码参数，无法建流";
        return false;
    }

    AVFormatContext* ctx = nullptr;
    int ret = avformat_alloc_output_context2(&ctx, nullptr, "flv", target.url.c_str());
    if (ret < 0 || !ctx) {
        target.lastError = "创建输出上下文失败: " + avErrorText(ret);
        return false;
    }

    AVStream* videoStream = avformat_new_stream(ctx, nullptr);
    if (!videoStream
        || avcodec_parameters_copy(videoStream->codecpar, videoParams) < 0) {
        avformat_free_context(ctx);
        target.lastError = "创建视频流失败";
        return false;
    }
    videoStream->codecpar->codec_tag = 0;
    videoStream->time_base = kFlvTimeBase;

    AVStream* audioStream = nullptr;
    if (audioParams) {
        audioStream = avformat_new_stream(ctx, nullptr);
        if (!audioStream
            || avcodec_parameters_copy(audioStream->codecpar, audioParams) < 0) {
            avformat_free_context(ctx);
            target.lastError = "创建音频流失败";
            return false;
        }
        audioStream->codecpar->codec_tag = 0;
        audioStream->time_base = kFlvTimeBase;
    }

    if (!(ctx->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rw_timeout", kIoTimeout, 0);
        ret = avio_open2(&ctx->pb, target.url.c_str(), AVIO_FLAG_WRITE, nullptr,
                         &options);
        av_dict_free(&options);

        if (ret < 0) {
            avformat_free_context(ctx);
            target.lastError = "连接失败: " + avErrorText(ret);
            return false;
        }
    }

    ret = avformat_write_header(ctx, nullptr);
    if (ret < 0) {
        if (ctx->pb)
            avio_closep(&ctx->pb);
        avformat_free_context(ctx);
        target.lastError = "写文件头失败: " + avErrorText(ret);
        return false;
    }

    target.ctx = ctx;
    target.videoStream = videoStream;
    target.audioStream = audioStream;
    target.connected = true;
    target.lastError.clear();
    return true;
}

void StreamOutput::Impl::closeTarget(Target& target, bool writeTrailer)
{
    if (target.ctx) {
        if (writeTrailer && target.connected)
            av_write_trailer(target.ctx);

        if (target.ctx->pb)
            avio_closep(&target.ctx->pb);

        avformat_free_context(target.ctx);
        target.ctx = nullptr;
    }

    target.videoStream = nullptr;
    target.audioStream = nullptr;
    target.connected = false;
}

bool StreamOutput::Impl::writePacket(const AVPacket& packet, AVRational timeBase,
                                     bool isVideo)
{
    int64_t ptsOffset = 0;
    if (!isVideo) {
        if (!audioOffsetReady) {
            audioOffset = packet.pts == AV_NOPTS_VALUE ? 0 : packet.pts;
            audioOffsetReady = true;
            if (audioOffset != 0 && log) {
                log("音频起点偏移 " + std::to_string(audioOffset)
                    + "（AAC 编码延迟），已归零");
            }
        }
        ptsOffset = audioOffset;
    }

    bool anyOk = false;

    for (Target& target : targets) {
        if (!target.connected || !target.ctx)
            continue;

        AVStream* stream = isVideo ? target.videoStream : target.audioStream;
        if (!stream)
            continue;

        AVPacket* copy = av_packet_clone(&packet);
        if (!copy) {
            target.lastError = "av_packet_clone 失败（内存不足？）";
            continue;
        }

        if (ptsOffset != 0) {
            if (copy->pts != AV_NOPTS_VALUE)
                copy->pts -= ptsOffset;
            if (copy->dts != AV_NOPTS_VALUE)
                copy->dts -= ptsOffset;
        }

        copy->stream_index = stream->index;

        av_packet_rescale_ts(copy, timeBase, stream->time_base);

        const int ret = av_interleaved_write_frame(target.ctx, copy);
        av_packet_free(&copy);

        if (ret < 0) {
            target.connected = false;
            target.lastError = "写入失败: " + avErrorText(ret);
            if (log)
                log("[" + target.name + "] 已断开: " + target.lastError);

            if (target.ctx) {
                if (target.ctx->pb)
                    avio_closep(&target.ctx->pb);
                avformat_free_context(target.ctx);
                target.ctx = nullptr;
            }
            target.videoStream = nullptr;
            target.audioStream = nullptr;
            continue;
        }

        target.packetsSent += 1;
        target.bytesSent += packet.size;
        anyOk = true;
    }

    return anyOk;
}

StreamOutput::StreamOutput() : impl_(std::make_unique<Impl>())
{
}

StreamOutput::~StreamOutput()
{
    close();
}

bool StreamOutput::open(const std::vector<StreamTarget>& targets,
                        const AVCodecParameters* videoParams,
                        const AVCodecParameters* audioParams, LogCallback log)
{
    close();

    impl_->log = std::move(log);
    impl_->lastError.clear();

    const auto say = [this](const std::string& message) {
        if (impl_->log)
            impl_->log(message);
    };

    if (targets.empty()) {
        impl_->lastError = "没有配置任何推流目标";
        return false;
    }

    if (!videoParams) {
        impl_->lastError = "没有视频编码参数";
        return false;
    }

    impl_->videoParams = avcodec_parameters_alloc();
    if (!impl_->videoParams
        || avcodec_parameters_copy(impl_->videoParams, videoParams) < 0) {
        impl_->lastError = "保存视频参数失败";
        close();
        return false;
    }

    if (audioParams) {
        impl_->audioParams = avcodec_parameters_alloc();
        if (!impl_->audioParams
            || avcodec_parameters_copy(impl_->audioParams, audioParams) < 0) {
            impl_->lastError = "保存音频参数失败";
            close();
            return false;
        }
    }

    impl_->targets.resize(targets.size());
    int connected = 0;

    for (size_t i = 0; i < targets.size(); ++i) {
        Impl::Target& target = impl_->targets[i];
        target.name = targets[i].platform.empty()
                          ? ("target" + std::to_string(i))
                          : targets[i].platform;
        target.url = targets[i].url;

        if (impl_->openTarget(target)) {
            ++connected;
            say("[" + target.name + "] 已连接 " + target.url);
        } else {
            say("[" + target.name + "] " + target.lastError);
        }
    }

    if (connected == 0) {
        impl_->lastError = "所有推流目标都没连上";
        return false;
    }

    say("推流输出就绪: " + std::to_string(connected) + "/"
        + std::to_string(targets.size()) + " 路可用");

    return true;
}

void StreamOutput::close()
{
    impl_->release();
}

bool StreamOutput::writeVideo(const AVPacket& packet, AVRational encoderTimeBase)
{
    return impl_->writePacket(packet, encoderTimeBase, true);
}

bool StreamOutput::writeAudio(const AVPacket& packet, AVRational encoderTimeBase)
{
    return impl_->writePacket(packet, encoderTimeBase, false);
}

void StreamOutput::flush()
{
    for (Impl::Target& target : impl_->targets) {
        if (target.connected && target.ctx)
            av_interleaved_write_frame(target.ctx, nullptr);
    }
}

std::vector<OutputTargetInfo> StreamOutput::targets() const
{
    std::vector<OutputTargetInfo> result;
    result.reserve(impl_->targets.size());

    for (const Impl::Target& target : impl_->targets) {
        OutputTargetInfo info;
        info.name = target.name;
        info.url = target.url;
        info.connected = target.connected;
        info.packetsSent = target.packetsSent;
        info.bytesSent = target.bytesSent;
        info.lastError = target.lastError;
        result.push_back(std::move(info));
    }

    return result;
}

int StreamOutput::connectedCount() const
{
    int count = 0;
    for (const Impl::Target& target : impl_->targets) {
        if (target.connected)
            ++count;
    }
    return count;
}

int StreamOutput::reconnectDisconnected()
{
    int reconnected = 0;

    for (Impl::Target& target : impl_->targets) {
        if (target.connected)
            continue;

        if (impl_->openTarget(target)) {
            ++reconnected;
            if (impl_->log)
                impl_->log("[" + target.name + "] 重连成功");
        }
    }

    return reconnected;
}

const std::string& StreamOutput::lastError() const
{
    return impl_->lastError;
}
