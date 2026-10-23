#include "core/encode/VideoEncoder.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstring>

namespace {

std::string avErrorText(int err)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

}

struct VideoEncoder::Impl {
    AVCodecContext* ctx = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* yuvFrame = nullptr;
    AVCodecParameters* params = nullptr;

    int width = 0;
    int height = 0;

    std::atomic<bool> forceKeyFrame{false};

    std::string lastError;
    LogCallback log;

    bool drain(std::vector<AVPacketPtr>& out);

    void release()
    {
        if (params) {
            avcodec_parameters_free(&params);
            params = nullptr;
        }
        if (yuvFrame) {
            av_frame_free(&yuvFrame);
            yuvFrame = nullptr;
        }
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (ctx) {
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        width = 0;
        height = 0;
    }
};

bool VideoEncoder::Impl::drain(std::vector<AVPacketPtr>& out)
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

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>())
{
}

VideoEncoder::~VideoEncoder()
{
    close();
}

bool VideoEncoder::open(const VideoEncoderConfig& config, LogCallback log)
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

    if (config.width <= 0 || config.height <= 0 || config.fps <= 0)
        return fail("编码参数无效：宽高和帧率都必须大于 0");

    if ((config.width % 2) != 0 || (config.height % 2) != 0) {
        return fail("宽高必须是偶数，当前 " + std::to_string(config.width) + "x"
                    + std::to_string(config.height));
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        say("没有找到 libx264，退回 FFmpeg 内置的 H.264 编码器（质量会差一些）");
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec)
        return fail("这个 FFmpeg 没有任何 H.264 编码器");

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx)
        return fail("avcodec_alloc_context3 失败");

    impl_->width = config.width;
    impl_->height = config.height;

    impl_->ctx->width = config.width;
    impl_->ctx->height = config.height;

    impl_->ctx->time_base = AVRational{1, config.fps};
    impl_->ctx->framerate = AVRational{config.fps, 1};

    impl_->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    impl_->ctx->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
    impl_->ctx->gop_size =
        config.gopSize > 0 ? config.gopSize : config.fps * 2;
    impl_->ctx->max_b_frames = config.maxBFrames < 0 ? 0 : config.maxBFrames;

    impl_->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    impl_->ctx->rc_max_rate = impl_->ctx->bit_rate;
    impl_->ctx->rc_buffer_size = static_cast<int>(impl_->ctx->bit_rate);

    if (!config.preset.empty())
        av_opt_set(impl_->ctx->priv_data, "preset", config.preset.c_str(), 0);
    if (!config.tune.empty())
        av_opt_set(impl_->ctx->priv_data, "tune", config.tune.c_str(), 0);

    if (std::strcmp(codec->name, "libx264") == 0)
        av_opt_set(impl_->ctx->priv_data, "forced-idr", "1", 0);

    const int openRet = avcodec_open2(impl_->ctx, codec, nullptr);
    if (openRet < 0) {
        return fail("打开 H.264 编码器失败: " + avErrorText(openRet)
                    + "（检查 preset/tune 名字是否正确）");
    }

    impl_->sws = sws_getContext(config.width, config.height, AV_PIX_FMT_BGRA,
                                config.width, config.height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws)
        return fail("创建 BGRA->YUV420P 转换上下文失败");

    impl_->yuvFrame = av_frame_alloc();
    if (!impl_->yuvFrame)
        return fail("av_frame_alloc 失败");

    impl_->yuvFrame->format = AV_PIX_FMT_YUV420P;
    impl_->yuvFrame->width = config.width;
    impl_->yuvFrame->height = config.height;
    if (av_frame_get_buffer(impl_->yuvFrame, 0) < 0)
        return fail("为 YUV 帧分配缓冲失败");

    impl_->params = avcodec_parameters_alloc();
    if (!impl_->params)
        return fail("avcodec_parameters_alloc 失败");
    if (avcodec_parameters_from_context(impl_->params, impl_->ctx) < 0)
        return fail("从编码器导出 codec parameters 失败");

    say("H.264 编码器已就绪: " + std::to_string(config.width) + "x"
        + std::to_string(config.height) + " @" + std::to_string(config.fps) + "fps, "
        + std::to_string(config.bitrateKbps) + " kbps, GOP "
        + std::to_string(impl_->ctx->gop_size) + ", extradata "
        + std::to_string(impl_->params->extradata_size) + " 字节");

    return true;
}

void VideoEncoder::close()
{
    impl_->release();
}

bool VideoEncoder::isOpen() const
{
    return impl_->ctx != nullptr;
}

AVRational VideoEncoder::timeBase() const
{
    if (!impl_->ctx)
        return AVRational{0, 1};
    return impl_->ctx->time_base;
}

const AVCodecParameters* VideoEncoder::codecParameters() const
{
    return impl_->params;
}

const std::string& VideoEncoder::lastError() const
{
    return impl_->lastError;
}

std::vector<AVPacketPtr> VideoEncoder::encode(const VideoFrame& frame)
{
    std::vector<AVPacketPtr> out;
    impl_->lastError.clear();

    if (!impl_->ctx) {
        impl_->lastError = "编码器还没打开";
        return out;
    }

    if (frame.width != impl_->width || frame.height != impl_->height) {
        impl_->lastError = "帧尺寸与编码器配置不一致: 期望 "
                           + std::to_string(impl_->width) + "x"
                           + std::to_string(impl_->height) + "，收到 "
                           + std::to_string(frame.width) + "x"
                           + std::to_string(frame.height);
        return out;
    }

    const size_t needed =
        static_cast<size_t>(frame.width) * frame.height * 4;
    if (frame.bgra.size() < needed) {
        impl_->lastError = "BGRA 数据不足，期望 " + std::to_string(needed)
                           + " 字节，实际 " + std::to_string(frame.bgra.size());
        return out;
    }

    if (av_frame_make_writable(impl_->yuvFrame) < 0) {
        impl_->lastError = "YUV 帧不可写";
        return out;
    }

    const uint8_t* srcData[4] = {frame.bgra.data(), nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {frame.width * 4, 0, 0, 0};

    sws_scale(impl_->sws, srcData, srcLinesize, 0, frame.height,
              impl_->yuvFrame->data, impl_->yuvFrame->linesize);

    impl_->yuvFrame->pts = frame.pts;

    if (impl_->forceKeyFrame.exchange(false))
        impl_->yuvFrame->pict_type = AV_PICTURE_TYPE_I;
    else
        impl_->yuvFrame->pict_type = AV_PICTURE_TYPE_NONE;

    const int sendRet = avcodec_send_frame(impl_->ctx, impl_->yuvFrame);
    if (sendRet < 0) {
        impl_->lastError = "avcodec_send_frame 失败: " + avErrorText(sendRet);
        return out;
    }

    impl_->drain(out);
    return out;
}

void VideoEncoder::requestKeyFrame()
{
    impl_->forceKeyFrame.store(true);
}

std::vector<AVPacketPtr> VideoEncoder::flush()
{
    std::vector<AVPacketPtr> out;
    if (!impl_->ctx)
        return out;

    avcodec_send_frame(impl_->ctx, nullptr);
    impl_->drain(out);
    return out;
}
