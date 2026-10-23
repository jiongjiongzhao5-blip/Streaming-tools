#include "core/encode/AudioEncoder.h"
#include "core/encode/VideoEncoder.h"
#include "core/output/StreamOutput.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
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
        std::fwrite(frame.bgra.data() + static_cast<size_t>(y) * stride, stride, 1,
                    file);

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
            uint8_t* p =
                frame->bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = kColors[bar][2];
            p[1] = kColors[bar][1];
            p[2] = kColors[bar][0];
            p[3] = 255;
        }
    }

    const int boxSize = height / 6;
    const int span = width - boxSize;
    const int boxX = span > 0 ? (index * 13) % span : 0;
    const int boxY = height / 2 - boxSize / 2;

    for (int y = boxY; y < boxY + boxSize; ++y) {
        for (int x = boxX; x < boxX + boxSize; ++x) {
            uint8_t* p =
                frame->bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = 0;
            p[1] = 0;
            p[2] = 0;
            p[3] = 255;
        }
    }

    return frame;
}

AudioChunkPtr makeSineChunk(int64_t startSample, int frames)
{
    constexpr int kRate = 48000;
    constexpr int kChannels = 2;

    auto chunk = std::make_shared<AudioChunk>();
    chunk->sampleRate = kRate;
    chunk->channels = kChannels;
    chunk->pts = startSample;
    chunk->samples.resize(static_cast<size_t>(frames) * kChannels);

    for (int i = 0; i < frames; ++i) {
        const double t =
            static_cast<double>(startSample + i) / static_cast<double>(kRate);
        const float value = static_cast<float>(std::sin(2.0 * kPi * 440.0 * t)) * 0.3f;
        for (int ch = 0; ch < kChannels; ++ch)
            chunk->samples[static_cast<size_t>(i) * kChannels + ch] = value;
    }
    return chunk;
}

struct RunResult {
    bool ok = false;
    int videoPackets = 0;
    int audioPackets = 0;
    std::vector<OutputTargetInfo> targets;
};

RunResult runOnce(const std::vector<StreamTarget>& targets, int frames, double seconds,
                  LogCallback log)
{
    RunResult result;

    VideoEncoderConfig videoConfig;
    videoConfig.width = 640;
    videoConfig.height = 360;
    videoConfig.fps = 30;
    videoConfig.bitrateKbps = 800;

    AudioEncoderConfig audioConfig;
    audioConfig.sampleRate = 48000;
    audioConfig.channels = 2;
    audioConfig.bitrateKbps = 128;

    VideoEncoder video;
    AudioEncoder audio;
    if (!video.open(videoConfig, nullptr) || !audio.open(audioConfig, nullptr))
        return result;

    std::vector<AVPacketPtr> audioPackets;
    const int64_t totalSamples = static_cast<int64_t>(seconds * 48000.0);
    for (int64_t pos = 0; pos < totalSamples; pos += 480) {
        int n = 480;
        if (pos + n > totalSamples)
            n = static_cast<int>(totalSamples - pos);

        for (auto& packet : audio.encode(*makeSineChunk(pos, n)))
            audioPackets.push_back(std::move(packet));
    }
    for (auto& packet : audio.flush())
        audioPackets.push_back(std::move(packet));

    StreamOutput output;
    if (!output.open(targets, video.codecParameters(), audio.codecParameters(), log))
        return result;

    size_t audioCursor = 0;
    const auto pushAudioUpTo = [&](int64_t timeMs) {
        while (audioCursor < audioPackets.size()) {
            const AVPacket* packet = audioPackets[audioCursor].get();
            const int64_t audioMs =
                packet->pts == AV_NOPTS_VALUE ? 0 : packet->pts * 1000 / 48000;
            if (audioMs > timeMs)
                break;
            output.writeAudio(*packet, audio.timeBase());
            ++result.audioPackets;
            ++audioCursor;
        }
    };

    for (int i = 0; i < frames; ++i) {
        for (auto& packet : video.encode(*makeTestFrame(640, 360, i))) {
            output.writeVideo(*packet, video.timeBase());
            ++result.videoPackets;
        }
        pushAudioUpTo(static_cast<int64_t>(i + 1) * 1000 / videoConfig.fps);
    }

    for (auto& packet : video.flush()) {
        output.writeVideo(*packet, video.timeBase());
        ++result.videoPackets;
    }
    pushAudioUpTo(INT64_MAX);

    output.flush();
    result.targets = output.targets();
    result.ok = true;

    video.close();
    audio.close();
    output.close();
    return result;
}

struct FlvInfo {
    bool ok = false;
    int streamCount = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int videoPackets = 0;
    int audioPackets = 0;
    int64_t maxVideoMs = 0;
    int64_t maxAudioMs = 0;
    bool sawNegativePts = false;
    VideoFrame firstFrame;
    bool firstFrameDecoded = false;
};

FlvInfo probeFlv(const std::string& path)
{
    FlvInfo info;

    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0)
        return info;

    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        avformat_close_input(&ctx);
        return info;
    }

    info.streamCount = static_cast<int>(ctx->nb_streams);

    int videoIndex = -1;
    int audioIndex = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoIndex < 0)
            videoIndex = static_cast<int>(i);
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioIndex < 0)
            audioIndex = static_cast<int>(i);
    }
    info.hasVideo = videoIndex >= 0;
    info.hasAudio = audioIndex >= 0;

    AVCodecContext* decoder = nullptr;
    AVFrame* decoded = nullptr;
    SwsContext* sws = nullptr;
    if (videoIndex >= 0) {
        const AVCodec* codec = avcodec_find_decoder(
            ctx->streams[videoIndex]->codecpar->codec_id);
        if (codec) {
            decoder = avcodec_alloc_context3(codec);
            if (decoder
                && avcodec_parameters_to_context(
                       decoder, ctx->streams[videoIndex]->codecpar)
                       >= 0
                && avcodec_open2(decoder, codec, nullptr) >= 0) {
                decoded = av_frame_alloc();
            }
        }
    }

    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(ctx, packet) >= 0) {
        const int64_t pts = packet->pts;

        if (packet->stream_index == videoIndex) {
            ++info.videoPackets;
            if (pts != AV_NOPTS_VALUE) {
                info.maxVideoMs = std::max(info.maxVideoMs, pts);
                if (pts < 0)
                    info.sawNegativePts = true;
            }

            if (decoder && decoded && !info.firstFrameDecoded) {
                if (avcodec_send_packet(decoder, packet) >= 0
                    && avcodec_receive_frame(decoder, decoded) >= 0) {
                    const int w = decoded->width;
                    const int h = decoded->height;
                    info.firstFrame.width = w;
                    info.firstFrame.height = h;
                    info.firstFrame.bgra.resize(static_cast<size_t>(w) * h * 4);

                    sws = sws_getContext(w, h,
                                         static_cast<AVPixelFormat>(decoded->format),
                                         w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
                    if (sws) {
                        uint8_t* dstData[4] = {info.firstFrame.bgra.data(), nullptr,
                                               nullptr, nullptr};
                        const int dstLinesize[4] = {w * 4, 0, 0, 0};
                        sws_scale(sws, decoded->data, decoded->linesize, 0, h,
                                  dstData, dstLinesize);
                        info.firstFrameDecoded = true;
                    }
                }
            }
        } else if (packet->stream_index == audioIndex) {
            ++info.audioPackets;
            if (pts != AV_NOPTS_VALUE) {
                info.maxAudioMs = std::max(info.maxAudioMs, pts);
                if (pts < 0)
                    info.sawNegativePts = true;
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    if (sws)
        sws_freeContext(sws);
    if (decoded)
        av_frame_free(&decoded);
    if (decoder)
        avcodec_free_context(&decoder);

    avformat_close_input(&ctx);
    info.ok = true;
    return info;
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
    std::printf("  模块 8：封装与多路分发测试\n");
    std::printf("================================================\n");
    std::printf("  没有真服务器也能测：把 rtmp:// 换成本地文件名，\n");
    std::printf("  同一个 StreamOutput 会写成 .flv，再读回来验证。\n");
    std::printf("================================================\n");

    std::vector<std::string> logs;
    const auto log = [&logs](const std::string& message) {
        logs.push_back(message);
        std::printf("  [输出] %s\n", message.c_str());
    };

    std::printf("\n--- 1. 单路输出到本地 flv ---\n");
    {
        const std::vector<StreamTarget> targets = {{"local", "output_single.flv"}};

        const auto start = std::chrono::steady_clock::now();
        const RunResult run = runOnce(targets, 60, 2.0, log);
        const long long ms = elapsedMs(start);

        check(run.ok, "推流输出建立成功");
        std::printf("  写入 %d 个视频包、%d 个音频包，耗时 %lld 毫秒\n",
                    run.videoPackets, run.audioPackets, ms);

        check(!run.targets.empty() && run.targets[0].connected, "目标保持连接");
        check(!run.targets.empty() && run.targets[0].packetsSent > 0,
              "统计到了写出的包数");

        std::printf("\n  读回 output_single.flv：\n");
        const FlvInfo info = probeFlv("output_single.flv");
        check(info.ok, "文件能被 avformat 正常打开");

        if (info.ok) {
            std::printf("    流数量 %d（视频 %d / 音频 %d）\n", info.streamCount,
                        info.hasVideo ? 1 : 0, info.hasAudio ? 1 : 0);
            std::printf("    视频 %d 包，最后一包 pts=%lld ms\n", info.videoPackets,
                        static_cast<long long>(info.maxVideoMs));
            std::printf("    音频 %d 包，最后一包 pts=%lld ms\n", info.audioPackets,
                        static_cast<long long>(info.maxAudioMs));

            check(info.streamCount == 2, "容器里有两条流");
            check(info.hasVideo && info.hasAudio, "视频流和音频流都在");
            check(info.videoPackets >= 55, "视频包数量对得上（60 帧）");
            check(info.audioPackets >= 90, "音频包数量对得上（2 秒约 94 包）");

            check(info.maxVideoMs >= 1800 && info.maxVideoMs <= 2100,
                  "视频时间轴落在 2 秒附近（时间基换算对了）");
            check(info.maxAudioMs >= 1800 && info.maxAudioMs <= 2200,
                  "音频时间轴落在 2 秒附近（重采样和偏移都没跑偏）");

            check(!info.sawNegativePts,
                  "没有任何负时间戳（AAC 的 priming 被归零了）");

            if (info.firstFrameDecoded) {
                check(info.firstFrame.width == 640 && info.firstFrame.height == 360,
                      "读回来的第一帧尺寸正确");
                check(saveBmp("output_first_frame.bmp", info.firstFrame),
                      "第一帧已保存为 output_first_frame.bmp");
            } else {
                check(false, "第一帧能解码出来");
            }
        }
    }

    std::printf("\n--- 2. 多路分发：一次编码写两路 ---\n");
    {
        const std::vector<StreamTarget> targets = {{"A", "output_a.flv"},
                                                   {"B", "output_b.flv"}};

        logs.clear();
        const RunResult run = runOnce(targets, 30, 1.0, log);

        check(run.ok, "两路都建立成功");
        check(run.targets.size() == 2, "两个目标都在列表里");
        check(run.targets.size() == 2 && run.targets[0].connected
                  && run.targets[1].connected,
              "两条连接都处于连接状态");

        if (run.targets.size() == 2) {
            std::printf("    A: %lld 包 / %lld 字节\n",
                        static_cast<long long>(run.targets[0].packetsSent),
                        static_cast<long long>(run.targets[0].bytesSent));
            std::printf("    B: %lld 包 / %lld 字节\n",
                        static_cast<long long>(run.targets[1].packetsSent),
                        static_cast<long long>(run.targets[1].bytesSent));

            check(run.targets[0].packetsSent == run.targets[1].packetsSent,
                  "两路收到的包数一模一样（同一份数据分发出去）");
        }

        const FlvInfo a = probeFlv("output_a.flv");
        const FlvInfo b = probeFlv("output_b.flv");

        check(a.ok && b.ok, "两个文件都能正常打开");
        check(a.streamCount == 2 && b.streamCount == 2, "两个文件都是双流");
        check(a.videoPackets == b.videoPackets,
              "两个文件的视频包数相同");
    }

    std::printf("\n--- 3. 断线隔离：一路死掉，另一路照常 ---\n");
    std::printf("  第二个目标指向 127.0.0.1:1（没人监听的端口），\n");
    std::printf("  预期：它连不上，但第一路必须一点不受影响。\n");
    {
        const std::vector<StreamTarget> targets = {
            {"live", "output_isolated.flv"},
            {"dead", "rtmp://127.0.0.1:1/live/nothing"}};

        logs.clear();
        const RunResult run = runOnce(targets, 30, 1.0, log);

        check(run.ok, "整体仍算成功（有一路可用就算成功）");
        check(run.targets.size() == 2, "两个目标都在列表里");

        if (run.targets.size() == 2) {
            check(run.targets[0].connected, "文件那一路正常连接");
            check(!run.targets[1].connected, "rtmp 那一路被判定为断开");
            check(!run.targets[1].lastError.empty(), "断开的理由被记录下来了");
            std::printf("    dead 路的原因: %s\n", run.targets[1].lastError.c_str());

            check(run.targets[0].packetsSent > 0, "好的那一路照常发出了包");
            check(run.targets[1].packetsSent == 0, "坏的那一路一个包都没发出去");
        }

        const FlvInfo info = probeFlv("output_isolated.flv");
        check(info.ok && info.streamCount == 2, "隔离之后文件依然完整");
        check(info.videoPackets > 0 && info.audioPackets > 0, "音视频都写进去了");
    }

    std::printf("\n--- 4. 重连接口 ---\n");
    {
        StreamOutput output;
        VideoEncoderConfig config;
        config.width = 640;
        config.height = 360;
        config.fps = 30;

        VideoEncoder encoder;
        encoder.open(config, nullptr);

        const std::vector<StreamTarget> targets = {
            {"dead", "rtmp://127.0.0.1:1/live/nothing"}};

        const bool opened =
            output.open(targets, encoder.codecParameters(), nullptr, nullptr);

        check(!opened, "全部目标都连不上时，open() 返回失败");
        check(output.connectedCount() == 0, "连接数为 0");
        std::printf("  open() 失败原因: %s\n", output.lastError().c_str());

        const auto start = std::chrono::steady_clock::now();
        const int reconnected = output.reconnectDisconnected();
        const long long ms = elapsedMs(start);

        check(reconnected == 0, "对着死地址重连，返回 0");
        std::printf("  重连尝试耗时 %lld 毫秒（有超时保护，不会无限等）\n", ms);

        encoder.close();
    }

    std::printf("\n--- 5. 断线退避重连 ---\n");
    std::printf("  一个好地址 + 一个必失败的地址，跑 10 秒。\n");
    std::printf("  预期：坏的那路按 1s / 2s / 4s 的节奏重试，不会每秒疯狂重连。\n");
    {
        StreamOutput output;

        VideoEncoderConfig config;
        config.width = 320;
        config.height = 180;
        config.fps = 30;

        VideoEncoder encoder;
        encoder.open(config, nullptr);

        const std::vector<StreamTarget> targets = {
            {"ok", "retry_ok.flv"}, {"dead", "Q:/no_such_dir/retry.flv"}};

        const bool opened =
            output.open(targets, encoder.codecParameters(), nullptr, log);
        check(opened, "至少有一路连上");

        int frameIndex = 0;
        const auto start = std::chrono::steady_clock::now();

        while (elapsedMs(start) < 10000) {
            for (auto& packet :
                 encoder.encode(*makeTestFrame(320, 180, frameIndex++)))
                output.writeVideo(*packet, encoder.timeBase());

            output.pollReconnect();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        const auto infos = output.targets();
        check(infos.size() == 2, "两路都在列表里");

        if (infos.size() == 2) {
            std::printf("    ok  : connected=%d, 包数=%lld\n",
                        infos[0].connected ? 1 : 0,
                        static_cast<long long>(infos[0].packetsSent));
            std::printf("    dead: connected=%d, 重试次数=%d\n",
                        infos[1].connected ? 1 : 0, infos[1].retryCount);
            std::printf("    dead 上次错误: %s\n", infos[1].lastError.c_str());

            check(infos[0].connected, "好的那一路一直连着");
            check(infos[0].packetsSent > 150, "好的那一路持续在发包");
            check(!infos[1].connected, "坏的那一路始终断开");

            check(infos[1].retryCount >= 2 && infos[1].retryCount <= 6,
                  "重试次数落在退避区间内（不是每秒重试）");
        }

        encoder.close();
        output.close();
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::printf("  output_single.flv / output_a.flv / output_b.flv\n");
    std::printf("  都可以直接用播放器打开，确认画面和声音。\n");
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
