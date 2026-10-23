#include "core/encode/AudioEncoder.h"
#include "core/encode/VideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

constexpr double kPi = 3.14159265358979323846;

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
        || frame.bgra.size() < static_cast<size_t>(frame.width) * frame.height * 4)
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
    for (int y = frame.height - 1; y >= 0; --y)
        std::fwrite(frame.bgra.data() + static_cast<size_t>(y) * stride, stride, 1, file);

    std::fclose(file);
    return true;
}

VideoFramePtr makeTestFrame(int width, int height, int index)
{
    auto frame = std::make_shared<VideoFrame>();
    frame->width = width;
    frame->height = height;
    frame->pts = index;
    frame->bgra.resize(static_cast<size_t>(width) * height * 4);

    static const uint8_t kColors[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int bar = x * 8 / width;
            uint8_t* p = frame->bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = kColors[bar][2];
            p[1] = kColors[bar][1];
            p[2] = kColors[bar][0];
            p[3] = 255;
        }
    }

    const int boxSize = height / 6;
    const int span = width - boxSize;
    const int boxX = span > 0 ? (index * 17) % span : 0;
    const int boxY = height / 2 - boxSize / 2;

    for (int y = boxY; y < boxY + boxSize; ++y) {
        for (int x = boxX; x < boxX + boxSize; ++x) {
            uint8_t* p = frame->bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
            p[3] = 255;
        }
    }

    return frame;
}

AudioChunkPtr makeSineChunk(int sampleRate, int channels, int64_t startSample,
                            int frames)
{
    auto chunk = std::make_shared<AudioChunk>();
    chunk->sampleRate = sampleRate;
    chunk->channels = channels;
    chunk->pts = startSample;
    chunk->samples.resize(static_cast<size_t>(frames) * channels);

    for (int i = 0; i < frames; ++i) {
        const double t =
            static_cast<double>(startSample + i) / static_cast<double>(sampleRate);
        const float value = static_cast<float>(std::sin(2.0 * kPi * 440.0 * t)) * 0.3f;
        for (int ch = 0; ch < channels; ++ch)
            chunk->samples[static_cast<size_t>(i) * channels + ch] = value;
    }
    return chunk;
}

bool decodeFirstFrame(const AVCodecParameters* params, const AVPacket* packet,
                      VideoFrame& out)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return false;

    bool ok = false;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;

    if (avcodec_parameters_to_context(ctx, params) >= 0
        && avcodec_open2(ctx, codec, nullptr) >= 0
        && avcodec_send_packet(ctx, packet) >= 0 && (frame = av_frame_alloc()) != nullptr
        && avcodec_receive_frame(ctx, frame) >= 0) {
        out.width = frame->width;
        out.height = frame->height;
        out.bgra.resize(static_cast<size_t>(frame->width) * frame->height * 4);

        sws = sws_getContext(frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format), frame->width,
                             frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                             nullptr, nullptr);
        if (sws) {
            uint8_t* dstData[4] = {out.bgra.data(), nullptr, nullptr, nullptr};
            const int dstLinesize[4] = {frame->width * 4, 0, 0, 0};
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData,
                      dstLinesize);
            ok = true;
        }
    }

    if (sws)
        sws_freeContext(sws);
    if (frame)
        av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

}

int main()
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    static char stdoutBuffer[8192];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    std::printf("================================================\n");
    std::printf("  模块 7：编码（H.264 / AAC）测试\n");
    std::printf("================================================\n");

    const auto log = [](const std::string& message) {
        std::printf("  [编码器] %s\n", message.c_str());
    };

    std::printf("\n--- 1. 视频编码 ---\n");
    {
        VideoEncoderConfig config;
        config.width = 1280;
        config.height = 720;
        config.fps = 30;
        config.bitrateKbps = 3000;

        VideoEncoder encoder;
        check(encoder.open(config, log), "视频编码器打开成功");

        if (encoder.isOpen()) {
            check(encoder.timeBase().num == 1 && encoder.timeBase().den == 30,
                  "时基是 1/30（pts 直接就是帧序号）");

            const auto params = encoder.codecParameters();
            check(params != nullptr && params->width == 1280 && params->height == 720,
                  "codec parameters 里的尺寸正确");
            check(params != nullptr && params->extradata_size > 0,
                  "带上了 SPS/PPS（extradata）");

            std::printf("  extradata 大小: %d 字节\n",
                        params ? params->extradata_size : 0);

            std::vector<AVPacketPtr> packets;
            size_t totalBytes = 0;
            int keyFrames = 0;

            const auto encodeStart = std::chrono::steady_clock::now();
            constexpr int kFrames = 60;
            for (int i = 0; i < kFrames; ++i)
            {
                const VideoFramePtr frame = makeTestFrame(1280, 720, i);
                auto produced = encoder.encode(*frame);
                for (auto& packet : produced) {
                    if (packet->flags & AV_PKT_FLAG_KEY)
                        ++keyFrames;
                    totalBytes += static_cast<size_t>(packet->size);
                    packets.push_back(std::move(packet));
                }
            }

            auto tail = encoder.flush();
            for (auto& packet : tail) {
                if (packet->flags & AV_PKT_FLAG_KEY)
                    ++keyFrames;
                totalBytes += static_cast<size_t>(packet->size);
                packets.push_back(std::move(packet));
            }
            const long long encodeMs = elapsedMs(encodeStart);

            std::printf("  %d 帧 -> %d 个包，共 %zu 字节（%.1f kB/帧）\n", kFrames,
                        static_cast<int>(packets.size()), totalBytes,
                        packets.empty() ? 0.0
                                        : static_cast<double>(totalBytes)
                                              / packets.size() / 1024.0);
            std::printf("  耗时 %lld 毫秒，平均 %.2f 毫秒/帧\n", encodeMs,
                        static_cast<double>(encodeMs) / kFrames);

            check(!packets.empty(), "编码出了包");
            check(packets.size() >= static_cast<size_t>(kFrames) / 2,
                  "包的数量级合理（不会一帧一个包这么理想，但也不该少一半）");
            check(!packets.empty() && (packets.front()->flags & AV_PKT_FLAG_KEY) != 0,
                  "第一个包是关键帧（否则播放器起不来）");
            check(keyFrames >= 1, "至少有一个关键帧");

            bool ptsIncreasing = true;
            for (size_t i = 1; i < packets.size(); ++i) {
                if (packets[i]->pts <= packets[i - 1]->pts)
                    ptsIncreasing = false;
            }
            check(ptsIncreasing, "pts 严格递增，没有倒流");
            check(!packets.empty() && packets.front()->pts == 0,
                  "第一包的 pts 是 0");

            check(static_cast<double>(encodeMs) / kFrames < 33.0,
                  "单帧编码耗时低于 33 毫秒，够跑 30fps");

            if (!packets.empty()) {
                VideoFrame decoded;
                const bool decodedOk =
                    decodeFirstFrame(encoder.codecParameters(), packets.front().get(),
                                     decoded);
                check(decodedOk, "第一个包能被解码回来（说明 SPS/PPS 和码流是自洽的）");
                if (decodedOk) {
                    check(decoded.width == 1280 && decoded.height == 720,
                          "解码出来的尺寸正确");
                    check(saveBmp("encode_video_frame.bmp", decoded),
                          "已保存为 encode_video_frame.bmp");
                    std::printf("  打开这张图确认：8 条彩条、颜色顺序、中间的黑方块\n");
                    std::printf("  红蓝对调说明 BGRA 的顺序读反了；整片发灰说明色彩范围没处理好\n");
                }
            }

            encoder.close();
            check(!encoder.isOpen(), "close() 之后编码器状态正确");
        }
    }

    std::printf("\n--- 2. 音频编码 ---\n");
    std::printf("  AAC 每帧固定 1024 个样本，而采集给的块大小是随机的，\n");
    std::printf("  所以这里故意用 480/512/960 三种大小交替送，考验缓冲逻辑。\n");
    {
        AudioEncoderConfig config;
        config.sampleRate = 48000;
        config.channels = 2;
        config.bitrateKbps = 128;

        AudioEncoder encoder;
        check(encoder.open(config, log), "音频编码器打开成功");

        if (encoder.isOpen()) {
            check(encoder.timeBase().num == 1 && encoder.timeBase().den == 48000,
                  "时基是 1/48000");

            const auto params = encoder.codecParameters();
            check(params != nullptr && params->sample_rate == 48000,
                  "codec parameters 里的采样率正确");
            check(params != nullptr && params->extradata_size > 0,
                  "带上了 AudioSpecificConfig（extradata）");

            const int64_t totalSamples = 48000;
            const int blockSizes[3] = {480, 512, 960};
            int blockIndex = 0;
            int64_t position = 0;

            std::vector<AVPacketPtr> packets;
            size_t totalBytes = 0;

            while (position < totalSamples) {
                int frames = blockSizes[blockIndex++ % 3];
                if (position + frames > totalSamples)
                    frames = static_cast<int>(totalSamples - position);

                const AudioChunkPtr chunk =
                    makeSineChunk(48000, 2, position, frames);
                auto produced = encoder.encode(*chunk);
                for (auto& packet : produced) {
                    totalBytes += static_cast<size_t>(packet->size);
                    packets.push_back(std::move(packet));
                }
                position += frames;
            }

            auto tail = encoder.flush();
            for (auto& packet : tail) {
                totalBytes += static_cast<size_t>(packet->size);
                packets.push_back(std::move(packet));
            }

            const double expected = static_cast<double>(totalSamples) / 1024.0;
            std::printf("  1 秒 -> %d 个包（理论 %.2f 个），共 %zu 字节\n",
                        static_cast<int>(packets.size()), expected, totalBytes);
            std::printf("  平均每包 %zu 字节，相当于 %.0f kbps\n",
                        packets.empty() ? 0 : totalBytes / packets.size(),
                        packets.empty()
                            ? 0.0
                            : static_cast<double>(totalBytes) * 8.0 / 1024.0);

            check(!packets.empty(), "编码出了包");
            check(std::fabs(static_cast<double>(packets.size()) - expected) <= 2.0,
                  "包数量与 时长×采样率/1024 吻合（缓冲没攒错）");

            bool ptsStepOk = true;
            for (size_t i = 1; i < packets.size(); ++i) {
                if (packets[i]->pts - packets[i - 1]->pts != 1024)
                    ptsStepOk = false;
            }
            check(ptsStepOk, "相邻两包的 pts 正好差 1024 个样本");

            if (!packets.empty()) {
                const long long firstPts = static_cast<long long>(packets.front()->pts);
                std::printf("  第一包的 pts = %lld（负数代表 AAC 的编码延迟）\n", firstPts);
                check(firstPts <= 0, "第一包的 pts <= 0，编码延迟被如实反映");
            }

            encoder.close();
        }
    }

    std::printf("\n--- 3. 重采样：44.1kHz 输入 -> 48kHz 输出 ---\n");
    std::printf("  设备给 44100 而推流要 48000 时，直接改字段是没用的，\n");
    std::printf("  必须真的重采样。这里验证 1 秒还是 1 秒。\n");
    {
        AudioEncoderConfig config;
        config.sampleRate = 48000;
        config.channels = 2;

        AudioEncoder encoder;
        if (!encoder.open(config, log)) {
            check(false, "打开重采样用的编码器");
        } else {
            const int64_t totalSamples = 44100;
            int64_t position = 0;

            std::vector<AVPacketPtr> packets;
            while (position < totalSamples) {
                int frames = 441;
                if (position + frames > totalSamples)
                    frames = static_cast<int>(totalSamples - position);

                const AudioChunkPtr chunk =
                    makeSineChunk(44100, 2, position, frames);
                auto produced = encoder.encode(*chunk);
                for (auto& packet : produced)
                    packets.push_back(std::move(packet));
                position += frames;
            }

            auto tail = encoder.flush();
            for (auto& packet : tail)
                packets.push_back(std::move(packet));

            const double expected = 48000.0 / 1024.0;
            std::printf("  1 秒 44.1kHz -> %d 个包（按 48kHz 应该是 %.2f 个）\n",
                        static_cast<int>(packets.size()), expected);
            check(std::fabs(static_cast<double>(packets.size()) - expected) <= 2.0,
                  "重采样后时长没变，还是 1 秒");

            encoder.close();
        }
    }

    std::printf("\n--- 4. 异常输入 ---\n");
    {
        VideoEncoder encoder;
        VideoEncoderConfig config;
        config.width = 1279;
        config.height = 720;
        check(!encoder.open(config, log), "宽高是奇数时拒绝打开（YUV420 要求偶数）");
        std::printf("  失败原因: %s\n", encoder.lastError().c_str());

        VideoEncoderConfig goodConfig;
        goodConfig.width = 1280;
        goodConfig.height = 720;

        VideoEncoder good;
        good.open(goodConfig, nullptr);
        VideoFrame wrongSize;
        wrongSize.width = 640;
        wrongSize.height = 480;
        wrongSize.bgra.resize(640 * 480 * 4);
        check(good.isOpen() && good.encode(wrongSize).empty(),
              "送进来的帧尺寸不对时，不编码并返回空");
        std::printf("  失败原因: %s\n", good.lastError().c_str());
    }

    std::printf("\n--- 5. 强制关键帧（重连后要用）---\n");
    std::printf("  GOP 设成 300，正常情况下一段里只有开头那个 I 帧。\n");
    std::printf("  中途请求一次，那一帧必须变成关键帧，否则重连后观众要等两秒才出画面。\n");
    {
        VideoEncoderConfig config;
        config.width = 640;
        config.height = 360;
        config.fps = 30;
        config.gopSize = 300;

        VideoEncoder encoder;
        if (!encoder.open(config, nullptr)) {
            check(false, "打开编码器");
        } else {
            std::map<int64_t, bool> keyFlags;

            for (int i = 0; i < 30; ++i) {
                if (i == 10)
                    encoder.requestKeyFrame();

                for (auto& packet : encoder.encode(*makeTestFrame(640, 360, i))) {
                    if (packet->pts != AV_NOPTS_VALUE)
                        keyFlags[packet->pts] = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                }
            }
            for (auto& packet : encoder.flush()) {
                if (packet->pts != AV_NOPTS_VALUE)
                    keyFlags[packet->pts] = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            }

            const auto keyAt = [&keyFlags](int64_t pts) {
                const auto it = keyFlags.find(pts);
                return it != keyFlags.end() && it->second;
            };

            check(keyAt(0), "第 0 帧是天然的关键帧");
            check(keyAt(10), "请求之后，第 10 帧被强制编成了关键帧");
            check(!keyAt(5) && !keyAt(15),
                  "其它帧没有被连带变成关键帧（说明只插了这一个 IDR）");

            encoder.close();
        }
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
