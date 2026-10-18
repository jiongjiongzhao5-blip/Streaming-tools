#include "core/capture/CameraCapturer.h"
#include "core/capture/CameraFrameConverter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const char* what)
{
    if (condition) {
        ++g_passed;
        std::printf("  [通过] %s\n", what);
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

void check(bool condition, const std::string& what)
{
    check(condition, what.c_str());
}

long long elapsedMs(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
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

CameraRawFrame makeFrame(int width, int height, CameraPixelFormat format,
                         int stride = 0)
{
    CameraRawFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = format;
    frame.stride = stride > 0 ? stride : packedRowBytes(format, width);
    frame.bytes.assign(static_cast<size_t>(frame.stride) * height, 0);
    return frame;
}

void setRgb24(CameraRawFrame& frame, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* p = frame.bytes.data() + static_cast<size_t>(y) * frame.stride
                 + static_cast<size_t>(x) * 3;
    p[0] = b;
    p[1] = g;
    p[2] = r;
}

void setRgb32(CameraRawFrame& frame, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* p = frame.bytes.data() + static_cast<size_t>(y) * frame.stride
                 + static_cast<size_t>(x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 0x00;
}

void setYuy2Pair(CameraRawFrame& frame, int x, int y, uint8_t y0, uint8_t y1,
                 uint8_t u, uint8_t v)
{
    uint8_t* p = frame.bytes.data() + static_cast<size_t>(y) * frame.stride
                 + static_cast<size_t>(x / 2) * 4;
    p[0] = y0;
    p[1] = u;
    p[2] = y1;
    p[3] = v;
}

void setUyvyPair(CameraRawFrame& frame, int x, int y, uint8_t y0, uint8_t y1,
                 uint8_t u, uint8_t v)
{
    uint8_t* p = frame.bytes.data() + static_cast<size_t>(y) * frame.stride
                 + static_cast<size_t>(x / 2) * 4;
    p[0] = u;
    p[1] = y0;
    p[2] = v;
    p[3] = y1;
}

bool pixelIs(const VideoFrame& frame, int x, int y, int b, int g, int r,
             int tolerance = 0)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return false;
    const uint8_t* p =
        frame.bgra.data() + (static_cast<size_t>(y) * frame.width + x) * 4;
    return std::abs(static_cast<int>(p[0]) - b) <= tolerance
           && std::abs(static_cast<int>(p[1]) - g) <= tolerance
           && std::abs(static_cast<int>(p[2]) - r) <= tolerance && p[3] == 255;
}

std::string pixelText(const VideoFrame& frame, int x, int y)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return "<越界>";
    const uint8_t* p =
        frame.bgra.data() + (static_cast<size_t>(y) * frame.width + x) * 4;
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "(B=%d G=%d R=%d A=%d)", p[0], p[1], p[2],
                  p[3]);
    return buffer;
}

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offBits;
};

struct BmpInfoHeader {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bitCount;
    uint32_t compression;
    uint32_t sizeImage;
    int32_t xPelsPerMeter;
    int32_t yPelsPerMeter;
    uint32_t clrUsed;
    uint32_t clrImportant;
};
#pragma pack(pop)

bool saveBmp(const std::string& path, const VideoFrame& frame)
{
    if (frame.width <= 0 || frame.height <= 0
        || frame.bgra.size()
               < static_cast<size_t>(frame.width) * frame.height * 4)
        return false;

    const uint32_t imageBytes =
        static_cast<uint32_t>(frame.width) * frame.height * 4;

    BmpFileHeader fileHeader = {};
    fileHeader.type = 0x4D42;
    fileHeader.size = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + imageBytes;
    fileHeader.offBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);

    BmpInfoHeader infoHeader = {};
    infoHeader.size = sizeof(BmpInfoHeader);
    infoHeader.width = frame.width;
    infoHeader.height = frame.height;
    infoHeader.planes = 1;
    infoHeader.bitCount = 32;
    infoHeader.compression = 0;
    infoHeader.sizeImage = imageBytes;
    infoHeader.xPelsPerMeter = 2835;
    infoHeader.yPelsPerMeter = 2835;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;

    std::fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    std::fwrite(&infoHeader, sizeof(infoHeader), 1, file);

    const size_t stride = static_cast<size_t>(frame.width) * 4;
    for (int y = frame.height - 1; y >= 0; --y) {
        std::fwrite(frame.bgra.data() + static_cast<size_t>(y) * stride, stride, 1,
                    file);
    }

    std::fclose(file);
    return true;
}

std::vector<uint8_t> encodeTestJpeg(int width, int height, int y, int u, int v)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec)
        return {};

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return {};

    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->color_range = AVCOL_RANGE_JPEG;
    ctx->time_base = AVRational{1, 25};

    std::vector<uint8_t> result;

    if (avcodec_open2(ctx, codec, nullptr) == 0) {
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();

        if (frame && packet) {
            frame->format = ctx->pix_fmt;
            frame->width = width;
            frame->height = height;

            if (av_frame_get_buffer(frame, 32) == 0) {
                for (int row = 0; row < height; ++row)
                    std::memset(frame->data[0] + row * frame->linesize[0], y, width);
                for (int row = 0; row < height / 2; ++row) {
                    std::memset(frame->data[1] + row * frame->linesize[1], u, width / 2);
                    std::memset(frame->data[2] + row * frame->linesize[2], v, width / 2);
                }

                if (avcodec_send_frame(ctx, frame) == 0
                    && avcodec_receive_packet(ctx, packet) == 0) {
                    result.assign(packet->data, packet->data + packet->size);
                }
            }
        }

        if (packet)
            av_packet_free(&packet);
        if (frame)
            av_frame_free(&frame);
    }

    avcodec_free_context(&ctx);
    return result;
}

}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    static char stdoutBuffer[8192];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    int deviceIndex = 0;
    if (argc > 1) {
        deviceIndex = std::atoi(argv[1]);
        if (deviceIndex < 0)
            deviceIndex = 0;
    }

    std::printf("================================================\n");
    std::printf("  模块 5（下）：原始字节 -> BGRA 转换测试\n");
    std::printf("================================================\n");

    CameraFrameConverter converter;

    std::printf("\n--- 1. RGB24 的颜色顺序 ---\n");
    std::printf("  RGB24 在内存里是 B,G,R。如果 B 和 R 写反了，\n");
    std::printf("  这里第一个像素就会从\"红\"变成\"蓝\"。\n");
    {
        CameraRawFrame raw = makeFrame(2, 2, CameraPixelFormat::RGB24);
        setRgb24(raw, 0, 0, 255, 0, 0);
        setRgb24(raw, 1, 0, 0, 255, 0);
        setRgb24(raw, 0, 1, 0, 0, 255);
        setRgb24(raw, 1, 1, 255, 255, 255);

        VideoFramePtr out = converter.convert(raw);
        check(out != nullptr, "转换成功");
        if (out) {
            check(out->width == 2 && out->height == 2, "输出尺寸正确");
            check(out->bgra.size() == 2 * 2 * 4, "bgra 大小等于 width*height*4");
            check(pixelIs(*out, 0, 0, 0, 0, 255), "(0,0) 是红色");
            check(pixelIs(*out, 1, 0, 0, 255, 0), "(1,0) 是绿色");
            check(pixelIs(*out, 0, 1, 255, 0, 0), "(0,1) 是蓝色");
            check(pixelIs(*out, 1, 1, 255, 255, 255), "(1,1) 是白色");
            if (!pixelIs(*out, 0, 0, 0, 0, 255)) {
                std::printf("  实际值: %s\n", pixelText(*out, 0, 0).c_str());
            }
        }
    }

    std::printf("\n--- 2. 行跨度带填充 ---\n");
    std::printf("  3 像素宽，每行只需 9 字节，却按 12 字节存（补了 3 字节）。\n");
    std::printf("  如果转换器按 9 字节跳行，从第 2 行开始就会读错位置。\n");
    {
        constexpr int kWidth = 3;
        constexpr int kHeight = 3;
        constexpr int kStride = 12;

        CameraRawFrame raw = makeFrame(kWidth, kHeight, CameraPixelFormat::RGB24,
                                       kStride);

        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                if (y == 0)
                    setRgb24(raw, x, y, 255, 0, 0);
                else if (y == 1)
                    setRgb24(raw, x, y, 0, 255, 0);
                else
                    setRgb24(raw, x, y, 0, 0, 255);
            }
            uint8_t* padding =
                raw.bytes.data() + static_cast<size_t>(y) * kStride + kWidth * 3;
            padding[0] = 0xAA;
            padding[1] = 0xBB;
            padding[2] = 0xCC;
        }

        VideoFramePtr out = converter.convert(raw);
        check(out != nullptr, "带填充的帧转换成功");

        bool allGood = out != nullptr;
        for (int x = 0; x < kWidth && out; ++x) {
            if (!pixelIs(*out, x, 0, 0, 0, 255))
                allGood = false;
            if (!pixelIs(*out, x, 1, 0, 255, 0))
                allGood = false;
            if (!pixelIs(*out, x, 2, 255, 0, 0))
                allGood = false;
        }
        check(allGood, "三行颜色各自正确，填充字节被正确跳过");

        if (out) {
            std::printf("  第 2 行第 0 个像素: %s（应为 B=0 G=255 R=0）\n",
                        pixelText(*out, 0, 1).c_str());
        }
    }

    std::printf("\n--- 3. 自底向上 -> 翻转 ---\n");
    std::printf("  源数据第 0 行画红、第 1 行画蓝。flipVertical=false 时\n");
    std::printf("  输出第 0 行应该是红；=true 时应该变成蓝。\n");
    {
        CameraRawFrame raw = makeFrame(2, 2, CameraPixelFormat::RGB24);
        setRgb24(raw, 0, 0, 255, 0, 0);
        setRgb24(raw, 1, 0, 255, 0, 0);
        setRgb24(raw, 0, 1, 0, 0, 255);
        setRgb24(raw, 1, 1, 0, 0, 255);

        raw.flipVertical = false;
        VideoFramePtr straight = converter.convert(raw);
        check(straight != nullptr, "不翻转时转换成功");
        if (straight)
            check(pixelIs(*straight, 0, 0, 0, 0, 255), "不翻转：第 0 行仍是红");

        raw.flipVertical = true;
        VideoFramePtr flipped = converter.convert(raw);
        check(flipped != nullptr, "翻转时转换成功");
        if (flipped) {
            check(pixelIs(*flipped, 0, 0, 255, 0, 0), "翻转后：第 0 行变成蓝");
            check(pixelIs(*flipped, 0, 1, 0, 0, 255), "翻转后：第 1 行变成红");
        }
    }

    std::printf("\n--- 4. RGB32 的第 4 个字节 ---\n");
    std::printf("  构造时把第 4 字节写成 0x00，输出必须是 A=255。\n");
    std::printf("  否则下游看到半透明画面，会以为是自己混色算错了。\n");
    {
        CameraRawFrame raw = makeFrame(1, 1, CameraPixelFormat::RGB32);
        setRgb32(raw, 0, 0, 200, 100, 50);

        VideoFramePtr out = converter.convert(raw);
        check(out != nullptr, "RGB32 转换成功");
        if (out) {
            check(pixelIs(*out, 0, 0, 50, 100, 200), "BGR 三个通道顺序正确");
            const uint8_t alpha = out->bgra[3];
            check(alpha == 255, "第 4 字节被强制成不透明");
            if (alpha != 255)
                std::printf("  实际 alpha = %d\n", alpha);
        }
    }

    std::printf("\n--- 5. YUY2 的宏像素 ---\n");
    std::printf("  4:2:2 里两个像素共用一组 U/V，所以它们只能同色系。\n");
    std::printf("  Y=81 U=90 V=240 是 BT.601 的纯红。\n");
    {
        CameraRawFrame raw = makeFrame(2, 2, CameraPixelFormat::YUY2);
        setYuy2Pair(raw, 0, 0, 81, 81, 90, 240);
        setYuy2Pair(raw, 0, 1, 41, 41, 240, 110);

        VideoFramePtr out = converter.convert(raw);
        check(out != nullptr, "YUY2 转换成功");
        if (out) {
            check(pixelIs(*out, 0, 0, 0, 0, 255, 2), "像素(0,0) 接近纯红");
            check(pixelIs(*out, 1, 0, 0, 0, 255, 2), "像素(1,0) 也是红（共用 U/V）");
            check(pixelIs(*out, 0, 1, 255, 0, 0, 2), "像素(0,1) 接近纯蓝");
            check(pixelIs(*out, 1, 1, 255, 0, 0, 2), "像素(1,1) 也是蓝");

            std::printf("  实际值 (0,0): %s\n", pixelText(*out, 0, 0).c_str());
            std::printf("  实际值 (0,1): %s\n", pixelText(*out, 0, 1).c_str());
        }

        CameraRawFrame gray = makeFrame(2, 1, CameraPixelFormat::YUY2);
        setYuy2Pair(gray, 0, 0, 128, 128, 128, 128);
        VideoFramePtr grayOut = converter.convert(gray);
        check(grayOut != nullptr, "中性灰转换成功");
        if (grayOut) {
            check(pixelIs(*grayOut, 0, 0, 130, 130, 130, 3),
                  "Y=U=V=128 得到约 130 的灰（说明做了 16~235 -> 0~255 的拉伸）");
            std::printf("  实际值: %s（偏移处理后应约为 130，不是 128 也不是 149）\n",
                        pixelText(*grayOut, 0, 0).c_str());
        }
    }

    std::printf("\n--- 6. UYVY 的字节顺序 ---\n");
    std::printf("  UYVY 只是把 U/V 挪到前面。读错顺序的话红蓝会对调。\n");
    {
        CameraRawFrame raw = makeFrame(2, 1, CameraPixelFormat::UYVY);
        setUyvyPair(raw, 0, 0, 81, 81, 90, 240);

        VideoFramePtr out = converter.convert(raw);
        check(out != nullptr, "UYVY 转换成功");
        if (out) {
            check(pixelIs(*out, 0, 0, 0, 0, 255, 2), "像素(0,0) 是红而不是蓝");
            check(pixelIs(*out, 1, 0, 0, 0, 255, 2), "像素(1,0) 也是红");
            std::printf("  若这里的 R 和 B 对调，说明把 UYVY 当成 YUY2 读了\n");
        }
    }

    std::printf("\n--- 7. 异常输入 ---\n");
    {
        CameraRawFrame shortData = makeFrame(4, 4, CameraPixelFormat::RGB24);
        shortData.bytes.resize(shortData.bytes.size() - 1);
        check(converter.convert(shortData) == nullptr, "数据不够时返回 nullptr");
        std::printf("  失败原因: %s\n", converter.lastError().c_str());
        check(!converter.lastError().empty(), "同时给出了失败原因");

        CameraRawFrame unknown = makeFrame(2, 2, CameraPixelFormat::RGB24);
        unknown.format = CameraPixelFormat::Unknown;
        check(converter.convert(unknown) == nullptr, "未知格式返回 nullptr");

        CameraRawFrame empty = makeFrame(2, 2, CameraPixelFormat::RGB24);
        empty.bytes.clear();
        check(converter.convert(empty) == nullptr, "空数据返回 nullptr");

        CameraRawFrame zeroSize = makeFrame(1, 1, CameraPixelFormat::RGB24);
        zeroSize.width = 0;
        check(converter.convert(zeroSize) == nullptr, "尺寸为 0 返回 nullptr");
    }

    std::printf("\n--- 8. MJPG 解码 ---\n");
    std::printf("  MJPG 每帧就是一张完整 JPEG，得先解码才能谈 stride。\n");
    std::printf("  这里现场编一张纯红 64x64 的 JPEG 当输入，结果完全确定。\n");
    std::printf("  注意 YUV 值要用**全范围**的：JPEG 的 Y 是 0..255（Y=76 U=85 V=255），\n");
    std::printf("  不是摄像头那种有限范围的 16..235（Y=81 U=90 V=240）。\n");
    {
        const std::vector<uint8_t> jpeg = encodeTestJpeg(64, 64, 76, 85, 255);

        if (jpeg.empty()) {
            std::printf("  这版 FFmpeg 没带 MJPEG 编码器，本节跳过（不是解码失败）\n");
        } else {
            std::printf("  生成了 %zu 字节的 JPEG，开头是 %02X %02X（JPEG 的 SOI 标记）\n",
                        jpeg.size(), jpeg[0], jpeg[1]);

            CameraRawFrame raw;
            raw.width = 64;
            raw.height = 64;
            raw.format = CameraPixelFormat::MJPG;
            raw.stride = 0;
            raw.bytes = jpeg;

            VideoFramePtr out = converter.convert(raw);
            check(out != nullptr, "MJPG 解码成功");
            if (out) {
                check(out->width == 64 && out->height == 64, "解码出来的尺寸正确");
                check(out->bgra.size() == 64 * 64 * 4, "bgra 大小正确");
                check(pixelIs(*out, 32, 32, 0, 0, 255, 16), "中心像素接近纯红");
                std::printf("  中心像素: %s（应为 B≈0 G≈0 R≈255）\n",
                            pixelText(*out, 32, 32).c_str());
            }

            CameraRawFrame padded = raw;
            padded.bytes.resize(jpeg.size() + 4096, 0);
            check(converter.convert(padded) != nullptr,
                  "帧尾补了 4096 个零字节，仍能靠 EOI 兜底解出来");

            CameraRawFrame garbage;
            garbage.width = 64;
            garbage.height = 64;
            garbage.format = CameraPixelFormat::MJPG;
            garbage.bytes.assign(1024, 0x5A);
            check(converter.convert(garbage) == nullptr, "喂垃圾数据时返回 nullptr");
            std::printf("  失败原因: %s\n", converter.lastError().c_str());
        }
    }

    std::printf("\n--- 9. 真实摄像头端到端 ---\n");
    {
        CameraCapturer capturer;
        const auto log = [](const std::string& message) {
            std::printf("  [采集器] %s\n", message.c_str());
        };

        if (!capturer.open(deviceIndex, log)) {
            std::printf("  打不开摄像头，这一节跳过（不是转换器的问题）\n");
        } else {
            std::printf("  摄像头: %d x %d, %s\n", capturer.width(),
                        capturer.height(),
                        cameraPixelFormatName(capturer.pixelFormat()));

            VideoFramePtr first;
            VideoFramePtr last;
            int failures = 0;

            const auto start = std::chrono::steady_clock::now();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i) {
                CameraRawFramePtr raw = capturer.nextFrame(2000);
                if (!raw) {
                    ++failures;
                    continue;
                }
                VideoFramePtr frame = converter.convert(*raw);
                if (!frame) {
                    std::printf("  转换失败: %s\n", converter.lastError().c_str());
                    ++failures;
                    continue;
                }
                if (!first)
                    first = frame;
                last = frame;
            }
            const long long ms = elapsedMs(start);

            check(failures == 0, "10 帧全部取到并转换成功");
            if (first) {
                check(first->width == capturer.width()
                          && first->height == capturer.height(),
                      "输出尺寸与协商值一致");
                check(first->bgra.size()
                          == static_cast<size_t>(first->width) * first->height * 4,
                      "bgra 大小严格等于 width*height*4（没有 stride 残留）");

                std::printf("  10 帧总耗时 %lld 毫秒（含等待摄像头出帧）\n", ms);

                check(saveBmp("camera_converted_first.bmp", *first),
                      "首帧已保存为 camera_converted_first.bmp");
                if (last)
                    check(saveBmp("camera_converted_last.bmp", *last),
                          "末帧已保存为 camera_converted_last.bmp");

                std::printf("\n  请打开这两个 bmp 确认三件事：\n");
                std::printf("    1. 画面正不正 —— 上下颠倒说明 flipVertical 没生效\n");
                std::printf("    2. 颜色对不对 —— 人脸发蓝/天空发橙说明 U/V 弄反了\n");
                std::printf("    3. 有没有斜纹 —— 有就是 stride 处理错了\n");
            }

            capturer.close();
        }
    }

    std::printf("\n--- 10. 转换耗时 ---\n");
    {
        CameraRawFrame raw = makeFrame(1280, 720, CameraPixelFormat::RGB24);
        for (size_t i = 0; i < raw.bytes.size(); ++i)
            raw.bytes[i] = static_cast<uint8_t>((i * 7) & 0xFF);

        constexpr int kRounds = 30;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kRounds; ++i)
            converter.convert(raw);
        const long long ms = elapsedMs(start);

        const double perFrame = static_cast<double>(ms) / kRounds;
        std::printf("  RGB24 1280x720 转换 %d 次，共 %lld 毫秒，平均 %.2f 毫秒/帧\n",
                    kRounds, ms, perFrame);
        std::printf("  按 30fps 算，一帧的预算是 33.3 毫秒\n");
        check(perFrame < 20.0, "单帧转换耗时低于 20 毫秒，够跑 30fps");
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
