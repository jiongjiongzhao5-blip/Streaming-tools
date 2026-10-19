#include "core/encode/AudioEncoder.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

namespace {

std::string avErrorText(int err)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

}

struct AudioEncoder::Impl {
    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVCodecParameters* params = nullptr;

    int sampleRate = 0;
    int channels = 0;
    int frameSize = 0;

    std::vector<float> pending;

    int64_t nextPts = 0;

    int swrInRate = 0;
    int swrInChannels = 0;

    std::string lastError;
    LogCallback log;

    bool drain(std::vector<AVPacketPtr>& out);
    bool encodeOneFrame(std::vector<AVPacketPtr>& out);
    bool appendResampled(const AudioChunk& chunk);

    void release()
    {
        if (params) {
            avcodec_parameters_free(&params);
            params = nullptr;
        }
        if (frame) {
            av_frame_free(&frame);
            frame = nullptr;
        }
        if (swr) {
            swr_free(&swr);
            swr = nullptr;
        }
        if (ctx) {
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        pending.clear();
        nextPts = 0;
        swrInRate = 0;
        swrInChannels = 0;
        sampleRate = 0;
        channels = 0;
        frameSize = 0;
    }
};

bool AudioEncoder::Impl::drain(std::vector<AVPacketPtr>& out)
{
    while (true) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            lastError = "av_packet_alloc 失败";
            return false;
        }

        const int ret = avcodec_receive_packet(ctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&packet);
            break;
        }
        if (ret < 0) {
            av_packet_free(&packet);
            lastError = "avcodec_receive_packet 失败: " + avErrorText(ret);
            return false;
        }

        out.emplace_back(packet);
    }
    return true;
}

bool AudioEncoder::Impl::encodeOneFrame(std::vector<AVPacketPtr>& out)
{
    if (av_frame_make_writable(frame) < 0) {
        lastError = "音频帧不可写";
        return false;
    }

    for (int ch = 0; ch < channels; ++ch) {
        auto* dst = reinterpret_cast<float*>(frame->data[ch]);
        for (int i = 0; i < frameSize; ++i)
            dst[i] = pending[static_cast<size_t>(i) * channels + ch];
    }

    frame->nb_samples = frameSize;
    frame->pts = nextPts;

    nextPts += frameSize;

    const int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        lastError = "avcodec_send_frame 失败: " + avErrorText(ret);
        return false;
    }

    return drain(out);
}

bool AudioEncoder::Impl::appendResampled(const AudioChunk& chunk)
{
    const int inSamples = static_cast<int>(chunk.samples.size() / chunk.channels);
    if (inSamples <= 0)
        return true;

    if (!swr || swrInRate != chunk.sampleRate || swrInChannels != chunk.channels) {
        if (swr) {
            swr_free(&swr);
            swr = nullptr;
        }

        AVChannelLayout inLayout;
        AVChannelLayout outLayout;
        av_channel_layout_default(&inLayout, chunk.channels);
        av_channel_layout_default(&outLayout, channels);

        int ret = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, sampleRate,
                                      &inLayout, AV_SAMPLE_FMT_FLT, chunk.sampleRate,
                                      0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        if (ret < 0 || !swr) {
            lastError = "创建重采样上下文失败: " + avErrorText(ret);
            return false;
        }

        if (swr_init(swr) < 0) {
            lastError = "初始化重采样上下文失败";
            swr_free(&swr);
            return false;
        }

        swrInRate = chunk.sampleRate;
        swrInChannels = chunk.channels;
    }

    const int outCapacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr, chunk.sampleRate) + inSamples, sampleRate,
        chunk.sampleRate, AV_ROUND_UP));

    std::vector<float> buffer(static_cast<size_t>(outCapacity) * channels + 64);

    const uint8_t* inData[1] = {reinterpret_cast<const uint8_t*>(chunk.samples.data())};
    uint8_t* outData[1] = {reinterpret_cast<uint8_t*>(buffer.data())};

    const int converted =
        swr_convert(swr, outData, outCapacity, inData, inSamples);

    if (converted < 0) {
        lastError = "重采样失败: " + avErrorText(converted);
        return false;
    }

    if (converted > 0) {
        pending.insert(pending.end(), buffer.begin(),
                       buffer.begin() + static_cast<size_t>(converted) * channels);
    }
    return true;
}

AudioEncoder::AudioEncoder() : impl_(std::make_unique<Impl>())
{
}

AudioEncoder::~AudioEncoder()
{
    close();
}

bool AudioEncoder::open(const AudioEncoderConfig& config, LogCallback log)
{
    close();
    impl_->log = std::move(log);
    impl_->lastError.clear();

    const auto say = [this](const std::string& message) {
        if (impl_->log)
            impl_->log(message);
    };
    const auto fail = [this, &say](const std::string& reason) {
        say(reason);
        close();
        impl_->lastError = reason;
        return false;
    };

    if (config.sampleRate <= 0 || config.channels <= 0)
        return fail("音频编码参数无效：采样率和声道数都必须大于 0");

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
        return fail("这个 FFmpeg 没有 AAC 编码器");

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx)
        return fail("avcodec_alloc_context3 失败");

    impl_->sampleRate = config.sampleRate;
    impl_->channels = config.channels;

    impl_->ctx->sample_rate = config.sampleRate;
    impl_->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    impl_->ctx->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
    impl_->ctx->time_base = AVRational{1, config.sampleRate};
    impl_->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_channel_layout_default(&impl_->ctx->ch_layout, config.channels);

    const int openRet = avcodec_open2(impl_->ctx, codec, nullptr);
    if (openRet < 0) {
        return fail("打开 AAC 编码器失败: " + avErrorText(openRet));
    }

    impl_->frameSize = impl_->ctx->frame_size > 0 ? impl_->ctx->frame_size : 1024;

    impl_->frame = av_frame_alloc();
    if (!impl_->frame)
        return fail("av_frame_alloc 失败");

    impl_->frame->format = AV_SAMPLE_FMT_FLTP;
    impl_->frame->nb_samples = impl_->frameSize;
    impl_->frame->sample_rate = config.sampleRate;
    av_channel_layout_default(&impl_->frame->ch_layout, config.channels);

    if (av_frame_get_buffer(impl_->frame, 0) < 0)
        return fail("为音频帧分配缓冲失败");

    impl_->params = avcodec_parameters_alloc();
    if (!impl_->params)
        return fail("avcodec_parameters_alloc 失败");
    if (avcodec_parameters_from_context(impl_->params, impl_->ctx) < 0)
        return fail("从编码器导出 codec parameters 失败");

    say("AAC 编码器已就绪: " + std::to_string(config.sampleRate) + " Hz, "
        + std::to_string(config.channels) + " 声道, "
        + std::to_string(config.bitrateKbps) + " kbps, 每帧 "
        + std::to_string(impl_->frameSize) + " 样本, extradata "
        + std::to_string(impl_->params->extradata_size) + " 字节");

    if (impl_->ctx->initial_padding > 0) {
        say("注意: AAC 有 " + std::to_string(impl_->ctx->initial_padding)
            + " 个样本的编码延迟，第一包的 pts 会是负的");
    }

    return true;
}

void AudioEncoder::close()
{
    impl_->release();
}

bool AudioEncoder::isOpen() const
{
    return impl_->ctx != nullptr;
}

AVRational AudioEncoder::timeBase() const
{
    if (!impl_->ctx)
        return AVRational{0, 1};
    return impl_->ctx->time_base;
}

const AVCodecParameters* AudioEncoder::codecParameters() const
{
    return impl_->params;
}

const std::string& AudioEncoder::lastError() const
{
    return impl_->lastError;
}

std::vector<AVPacketPtr> AudioEncoder::encode(const AudioChunk& chunk)
{
    std::vector<AVPacketPtr> out;
    impl_->lastError.clear();

    if (!impl_->ctx) {
        impl_->lastError = "编码器还没打开";
        return out;
    }

    if (chunk.channels <= 0 || chunk.sampleRate <= 0) {
        impl_->lastError = "音频块的格式信息无效";
        return out;
    }

    if (chunk.samples.empty())
        return out;

    if (chunk.sampleRate == impl_->sampleRate && chunk.channels == impl_->channels) {
        impl_->pending.insert(impl_->pending.end(), chunk.samples.begin(),
                              chunk.samples.end());
    } else {
        if (!impl_->appendResampled(chunk))
            return out;
    }

    const size_t need =
        static_cast<size_t>(impl_->frameSize) * impl_->channels;

    while (impl_->pending.size() >= need) {
        if (!impl_->encodeOneFrame(out))
            break;
        impl_->pending.erase(impl_->pending.begin(),
                             impl_->pending.begin() + need);
    }

    return out;
}

std::vector<AVPacketPtr> AudioEncoder::flush()
{
    std::vector<AVPacketPtr> out;
    if (!impl_->ctx)
        return out;

    const size_t need =
        static_cast<size_t>(impl_->frameSize) * impl_->channels;

    if (!impl_->pending.empty() && impl_->pending.size() < need) {
        impl_->pending.resize(need, 0.0f);
        impl_->encodeOneFrame(out);
        impl_->pending.clear();
    }

    avcodec_send_frame(impl_->ctx, nullptr);
    impl_->drain(out);
    return out;
}
