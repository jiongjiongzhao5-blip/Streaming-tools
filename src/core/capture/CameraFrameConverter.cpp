#include "core/capture/CameraFrameConverter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstring>

namespace {

inline uint8_t clampByte(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<uint8_t>(value);
}

int packedRowBytes(CameraPixelFormat format, int width)
{
    switch (format) {
    case CameraPixelFormat::RGB24:
        return width * 3;
    case CameraPixelFormat::RGB32:
        return width * 4;
    case CameraPixelFormat::YUY2:
    case CameraPixelFormat::UYVY:
        return ((width + 1) / 2) * 4;
    default:
        return 0;
    }
}

std::string avErrorText(int err)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

inline void yuvToBgra(int y, int u, int v, uint8_t* dst)
{
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;

    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;

    dst[0] = clampByte(b);
    dst[1] = clampByte(g);
    dst[2] = clampByte(r);
    dst[3] = 255;
}

void convertRgb24Row(const uint8_t* src, uint8_t* dst, int width)
{
    for (int x = 0; x < width; ++x) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        src += 3;
        dst += 4;
    }
}

void convertRgb32Row(const uint8_t* src, uint8_t* dst, int width)
{
    for (int x = 0; x < width; ++x) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        src += 4;
        dst += 4;
    }
}

void convertYuy2Row(const uint8_t* src, uint8_t* dst, int width)
{
    for (int x = 0; x < width; x += 2) {
        const int y0 = src[0];
        const int u = src[1];
        const int y1 = src[2];
        const int v = src[3];

        yuvToBgra(y0, u, v, dst);
        dst += 4;

        if (x + 1 < width) {
            yuvToBgra(y1, u, v, dst);
            dst += 4;
        }
        src += 4;
    }
}

void convertUyvyRow(const uint8_t* src, uint8_t* dst, int width)
{
    for (int x = 0; x < width; x += 2) {
        const int u = src[0];
        const int y0 = src[1];
        const int v = src[2];
        const int y1 = src[3];

        yuvToBgra(y0, u, v, dst);
        dst += 4;

        if (x + 1 < width) {
            yuvToBgra(y1, u, v, dst);
            dst += 4;
        }
        src += 4;
    }
}

}

struct CameraFrameConverter::Impl {
    std::string lastError;

    AVCodecContext* mjpegCtx = nullptr;
    AVFrame* decodedFrame = nullptr;
    AVPacket* packet = nullptr;

    SwsContext* sws = nullptr;
    int swsWidth = 0;
    int swsHeight = 0;
    int swsFormat = -1;

    ~Impl()
    {
        releaseDecoder();
    }

    void releaseDecoder()
    {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (packet) {
            av_packet_free(&packet);
            packet = nullptr;
        }
        if (decodedFrame) {
            av_frame_free(&decodedFrame);
            decodedFrame = nullptr;
        }
        if (mjpegCtx) {
            avcodec_free_context(&mjpegCtx);
            mjpegCtx = nullptr;
        }
        swsWidth = 0;
        swsHeight = 0;
        swsFormat = -1;
    }

    bool ensureDecoder();
    bool convertMjpeg(const CameraRawFrame& raw, VideoFrame& out);
};

bool CameraFrameConverter::Impl::ensureDecoder()
{
    if (mjpegCtx)
        return true;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        lastError = "找不到 MJPEG 解码器（这版 FFmpeg 没编译进去？）";
        return false;
    }

    mjpegCtx = avcodec_alloc_context3(codec);
    if (!mjpegCtx) {
        lastError = "avcodec_alloc_context3 失败";
        return false;
    }

    mjpegCtx->thread_count = 1;

    const int openRet = avcodec_open2(mjpegCtx, codec, nullptr);
    if (openRet < 0) {
        lastError = "打开 MJPEG 解码器失败: " + avErrorText(openRet);
        avcodec_free_context(&mjpegCtx);
        return false;
    }

    decodedFrame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!decodedFrame || !packet) {
        lastError = "分配解码缓冲区失败";
        releaseDecoder();
        return false;
    }

    return true;
}

bool CameraFrameConverter::Impl::convertMjpeg(const CameraRawFrame& raw, VideoFrame& out)
{
    if (!ensureDecoder())
        return false;

    const auto decode = [this](const uint8_t* data, size_t size) -> int {
        av_packet_unref(packet);
        packet->data = const_cast<uint8_t*>(data);
        packet->size = static_cast<int>(size);

        const int sendRet = avcodec_send_packet(mjpegCtx, packet);
        if (sendRet < 0)
            return sendRet;

        av_frame_unref(decodedFrame);
        return avcodec_receive_frame(mjpegCtx, decodedFrame);
    };

    int ret = decode(raw.bytes.data(), raw.bytes.size());

    if (ret < 0) {
        size_t trimmed = 0;
        for (size_t i = raw.bytes.size(); i >= 2; --i) {
            if (raw.bytes[i - 2] == 0xFF && raw.bytes[i - 1] == 0xD9) {
                trimmed = i;
                break;
            }
        }
        if (trimmed > 2 && trimmed < raw.bytes.size())
            ret = decode(raw.bytes.data(), trimmed);
    }

    if (ret < 0) {
        lastError = "MJPEG 解码失败: " + avErrorText(ret);
        return false;
    }

    const int width = decodedFrame->width;
    const int height = decodedFrame->height;
    if (width <= 0 || height <= 0) {
        lastError = "MJPEG 解码出来的尺寸无效";
        av_frame_unref(decodedFrame);
        return false;
    }

    if (!sws || swsWidth != width || swsHeight != height
        || swsFormat != decodedFrame->format) {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        sws = sws_getContext(width, height,
                             static_cast<AVPixelFormat>(decodedFrame->format), width,
                             height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                             nullptr);
        if (!sws) {
            lastError = "创建 YUV->BGRA 转换上下文失败";
            av_frame_unref(decodedFrame);
            return false;
        }

        const bool sourceIsFullRange =
            decodedFrame->format == AV_PIX_FMT_YUVJ420P
            || decodedFrame->format == AV_PIX_FMT_YUVJ422P
            || decodedFrame->format == AV_PIX_FMT_YUVJ444P
            || decodedFrame->color_range == AVCOL_RANGE_JPEG;

        const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(sws, coefficients, sourceIsFullRange ? 1 : 0,
                                 coefficients, 1, 0, 1 << 16, 1 << 16);

        swsWidth = width;
        swsHeight = height;
        swsFormat = decodedFrame->format;
    }

    out.width = width;
    out.height = height;
    out.bgra.assign(static_cast<size_t>(width) * height * 4, 0);

    uint8_t* dstData[4] = {out.bgra.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {width * 4, 0, 0, 0};
    sws_scale(sws, decodedFrame->data, decodedFrame->linesize, 0, height, dstData,
              dstLinesize);

    av_frame_unref(decodedFrame);
    return true;
}

CameraFrameConverter::CameraFrameConverter() : impl_(std::make_unique<Impl>())
{
}

CameraFrameConverter::~CameraFrameConverter() = default;

const std::string& CameraFrameConverter::lastError() const
{
    return impl_->lastError;
}

VideoFramePtr CameraFrameConverter::convert(const CameraRawFrame& raw)
{
    impl_->lastError.clear();

    if (raw.width <= 0 || raw.height <= 0) {
        impl_->lastError = "帧尺寸无效";
        return nullptr;
    }

    if (raw.format == CameraPixelFormat::Unknown) {
        impl_->lastError = "未知的像素格式，不知道怎么解读";
        return nullptr;
    }

    if (raw.bytes.empty()) {
        impl_->lastError = "帧数据为空";
        return nullptr;
    }

    auto out = std::make_shared<VideoFrame>();
    out->pts = raw.pts;

    if (raw.format == CameraPixelFormat::MJPG) {
        if (!impl_->convertMjpeg(raw, *out))
            return nullptr;
        return out;
    }

    const int rowBytes = packedRowBytes(raw.format, raw.width);
    if (rowBytes <= 0) {
        impl_->lastError = "这种格式没有逐行转换的实现";
        return nullptr;
    }

    const int stride = raw.stride >= rowBytes ? raw.stride : rowBytes;

    const size_t needed =
        static_cast<size_t>(stride) * static_cast<size_t>(raw.height - 1)
        + static_cast<size_t>(rowBytes);

    if (raw.bytes.size() < needed) {
        impl_->lastError = "帧数据不完整：需要 " + std::to_string(needed) + " 字节，实际只有 "
                           + std::to_string(raw.bytes.size()) + " 字节";
        return nullptr;
    }

    out->width = raw.width;
    out->height = raw.height;
    out->bgra.resize(static_cast<size_t>(raw.width) * raw.height * 4);

    const int dstRowBytes = raw.width * 4;
    const uint8_t* srcBase = raw.bytes.data();

    for (int y = 0; y < raw.height; ++y) {
        const uint8_t* src = srcBase + static_cast<size_t>(y) * stride;

        const int dstY = raw.flipVertical ? (raw.height - 1 - y) : y;
        uint8_t* dst = out->bgra.data() + static_cast<size_t>(dstY) * dstRowBytes;

        switch (raw.format) {
        case CameraPixelFormat::RGB24:
            convertRgb24Row(src, dst, raw.width);
            break;
        case CameraPixelFormat::RGB32:
            convertRgb32Row(src, dst, raw.width);
            break;
        case CameraPixelFormat::YUY2:
            convertYuy2Row(src, dst, raw.width);
            break;
        case CameraPixelFormat::UYVY:
            convertUyvyRow(src, dst, raw.width);
            break;
        default:
            break;
        }
    }

    return out;
}
