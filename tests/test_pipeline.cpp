#include "core/capture/CameraCapturer.h"
#include "core/pipeline/StreamPipeline.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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
        std::fwrite(frame.bgra.data() + static_cast<size_t>(y) * stride, stride, 1, file);

    std::fclose(file);
    return true;
}

struct FlvInfo {
    bool ok = false;
    int streamCount = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int videoPackets = 0;
    int audioPackets = 0;
    int64_t lastVideoMs = 0;
    int64_t lastAudioMs = 0;
    bool sawNegativePts = false;
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

    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(ctx, packet) >= 0) {
        if (packet->pts != AV_NOPTS_VALUE && packet->pts < 0)
            info.sawNegativePts = true;

        if (packet->stream_index == videoIndex) {
            ++info.videoPackets;
            if (packet->pts != AV_NOPTS_VALUE)
                info.lastVideoMs = std::max(info.lastVideoMs, packet->pts);
        } else if (packet->stream_index == audioIndex) {
            ++info.audioPackets;
            if (packet->pts != AV_NOPTS_VALUE)
                info.lastAudioMs = std::max(info.lastAudioMs, packet->pts);
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
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
    std::printf("  模块 9：完整推流管线测试\n");
    std::printf("================================================\n");
    std::printf("  屏幕采集 + 系统声音 -> 编码 -> 封装 -> 两路本地 flv\n");
    std::printf("  跑 3 秒，然后看帧数、时间轴、文件完整性。\n");
    std::printf("================================================\n");

    std::vector<std::string> logs;
    const auto log = [&logs](const std::string& message) {
        logs.push_back(message);
        std::printf("  [管线] %s\n", message.c_str());
    };

    std::printf("\n--- 1. 参数校验 ---\n");
    {
        StreamPipeline pipeline;
        StreamConfig config;

        check(!pipeline.start(config, nullptr), "没有推流目标时拒绝启动");
        std::printf("  失败原因: %s\n", pipeline.lastError().c_str());
        check(!pipeline.isRunning(), "失败后没有处于运行状态");

        StreamConfig bad = config;
        StreamTarget target;
        target.platform = "A";
        target.url = "pipeline_bad.flv";
        bad.targets.push_back(target);
        bad.fps = 0;

        check(!pipeline.start(bad, nullptr), "帧率为 0 时拒绝启动");
    }

    std::printf("\n--- 2. 跑一条完整管线（3 秒，两路输出）---\n");
    {
        StreamConfig config;
        config.sourceType = SourceType::Screen;
        config.audioType = AudioSourceType::SystemAudio;
        config.width = 1280;
        config.height = 720;
        config.fps = 30;
        config.videoKbps = 2000;
        config.audioKbps = 128;

        StreamTarget targetA;
        targetA.platform = "A";
        targetA.url = "pipeline_a.flv";
        StreamTarget targetB;
        targetB.platform = "B";
        targetB.url = "pipeline_b.flv";
        config.targets = {targetA, targetB};

        StreamPipeline pipeline;

        const auto startTime = std::chrono::steady_clock::now();
        const bool started = pipeline.start(config, log);
        const long long startupMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime)
                .count();

        check(started, "管线启动成功");
        std::printf("  启动耗时 %lld 毫秒（含编码器初始化 + 两路连接）\n", startupMs);

        if (!started) {
            std::printf("  启动失败: %s\n", pipeline.lastError().c_str());
            return 1;
        }

        for (int tick = 1; tick <= 6; ++tick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const PipelineStats s = pipeline.stats();
            std::printf("  [%4.1fs] 采集 %lld 帧，编码 %lld 包，丢 %lld 帧，"
                        "音频 %lld 块 / %lld 包，连接 %d/%d\n",
                        s.elapsedSeconds,
                        static_cast<long long>(s.videoFramesCaptured),
                        static_cast<long long>(s.videoPacketsEncoded),
                        static_cast<long long>(s.videoFramesDropped),
                        static_cast<long long>(s.audioChunksCaptured),
                        static_cast<long long>(s.audioPacketsEncoded),
                        s.connectedTargets, s.totalTargets);
        }

        check(pipeline.isRunning(), "跑到一半时仍处于运行状态");

        const PipelineStats finalStats = pipeline.stats();

        const auto stopStart = std::chrono::steady_clock::now();
        pipeline.stop();
        const long long stopMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stopStart)
                .count();

        std::printf("  停止耗时 %lld 毫秒（要把队列里剩下的包写完）\n", stopMs);

        check(!pipeline.isRunning(), "停止后状态正确");
        check(stopMs < 3000, "停止过程没有卡死");

        const double capturedFps =
            finalStats.elapsedSeconds > 0
                ? static_cast<double>(finalStats.videoFramesCaptured)
                      / finalStats.elapsedSeconds
                : 0.0;

        std::printf("  实际采集帧率 %.1f fps（目标 %d）\n", capturedFps, config.fps);

        check(finalStats.videoFramesCaptured >= 70,
              "3 秒采到了 70 帧以上（30fps 的合理下限）");
        check(capturedFps > 20.0 && capturedFps < 40.0,
              "实际帧率落在 20~40 之间，没有被编码拖垮");
        check(finalStats.videoPacketsEncoded >= 60, "编码出了足够多的视频包");
        check(finalStats.audioChunksCaptured > 150, "音频块数量合理（约 300 块）");
        check(finalStats.connectedTargets == 2, "两路都保持着连接");

        std::printf("\n  读回两个文件：\n");
        const FlvInfo a = probeFlv("pipeline_a.flv");
        const FlvInfo b = probeFlv("pipeline_b.flv");

        check(a.ok && b.ok, "两个 flv 都能被正常打开");
        check(a.streamCount == 2 && b.streamCount == 2, "两个文件都是双流");
        check(a.hasVideo && a.hasAudio, "视频流和音频流都在");

        std::printf("    A: 视频 %d 包（末 %lld ms）/ 音频 %d 包（末 %lld ms）\n",
                    a.videoPackets, static_cast<long long>(a.lastVideoMs),
                    a.audioPackets, static_cast<long long>(a.lastAudioMs));
        std::printf("    B: 视频 %d 包（末 %lld ms）/ 音频 %d 包（末 %lld ms）\n",
                    b.videoPackets, static_cast<long long>(b.lastVideoMs),
                    b.audioPackets, static_cast<long long>(b.lastAudioMs));

        check(a.videoPackets == b.videoPackets, "两路视频包数完全一致");

        std::printf("\n  时间轴对齐检查（目标 3 秒 = 3000 ms）：\n");
        check(a.lastVideoMs >= 2700 && a.lastVideoMs <= 3400,
              "视频时间轴落在 3 秒附近");
        check(a.lastAudioMs >= 2700 && a.lastAudioMs <= 3400,
              "音频时间轴落在 3 秒附近");

        const int64_t drift = a.lastVideoMs - a.lastAudioMs;
        std::printf("    视频与音频的时间差 %lld ms\n", static_cast<long long>(drift));
        check(drift > -500 && drift < 500,
              "音视频时间差在 500ms 以内（没有累积漂移）");

        check(!a.sawNegativePts && !b.sawNegativePts, "没有负时间戳");
    }

    std::printf("\n--- 3. 重复启停 ---\n");
    {
        StreamConfig config;
        config.sourceType = SourceType::Screen;
        config.audioType = AudioSourceType::SystemAudio;
        config.width = 640;
        config.height = 360;
        config.fps = 30;
        config.videoKbps = 800;

        StreamTarget target;
        target.platform = "loop";
        target.url = "pipeline_loop.flv";
        config.targets = {target};

        StreamPipeline pipeline;
        bool allOk = true;

        for (int round = 0; round < 3; ++round) {
            if (!pipeline.start(config, nullptr)) {
                allOk = false;
                std::printf("  第 %d 轮启动失败: %s\n", round + 1,
                            pipeline.lastError().c_str());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (pipeline.stats().videoFramesCaptured == 0)
                allOk = false;
            pipeline.stop();
        }

        check(allOk, "连续启动/停止三轮都正常");
    }

    std::printf("\n--- 4. 只录制，不推流 ---\n");
    std::printf("  录制在管线里就是多一路输出，所以没有推流地址也应该能跑。\n");
    {
        StreamConfig config;
        config.sourceType = SourceType::Screen;
        config.audioType = AudioSourceType::SystemAudio;
        config.width = 640;
        config.height = 360;
        config.fps = 30;
        config.videoKbps = 800;
        config.recordPath = "record_only.flv";

        StreamPipeline pipeline;
        check(pipeline.start(config, nullptr), "只有录制路径时也能启动");

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        const PipelineStats s = pipeline.stats();
        pipeline.stop();

        std::printf("  采了 %lld 帧，连接 %d/%d\n",
                    static_cast<long long>(s.videoFramesCaptured),
                    s.connectedTargets, s.totalTargets);

        check(s.videoFramesCaptured > 40, "录制期间正常采集");
        check(s.connectedTargets == 1, "录制那一路处于连接状态");

        const FlvInfo info = probeFlv("record_only.flv");
        check(info.ok, "录制文件能被正常打开");
        check(info.streamCount == 2, "录制文件是双流（音+视频）");
        check(info.videoPackets > 40, "录制文件里有足够的视频包");
        check(info.lastAudioMs > 1500, "录制时长接近 2 秒");
    }

    std::printf("\n--- 5. 推流 + 录制同时进行 ---\n");
    {
        StreamConfig config;
        config.sourceType = SourceType::Screen;
        config.audioType = AudioSourceType::SystemAudio;
        config.width = 640;
        config.height = 360;
        config.fps = 30;
        config.videoKbps = 800;

        StreamTarget stream;
        stream.platform = "stream";
        stream.url = "pipeline_stream.flv";
        config.targets = {stream};

        config.recordPath = "pipeline_record.flv";

        StreamPipeline pipeline;
        check(pipeline.start(config, nullptr), "推流 + 录制可以同时启动");

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        pipeline.stop();

        const FlvInfo a = probeFlv("pipeline_stream.flv");
        const FlvInfo b = probeFlv("pipeline_record.flv");

        std::printf("    推流端: 视频 %d 包 / 音频 %d 包\n", a.videoPackets,
                    a.audioPackets);
        std::printf("    录制端: 视频 %d 包 / 音频 %d 包\n", b.videoPackets,
                    b.audioPackets);

        check(a.ok && b.ok, "两个文件都生成了");
        check(a.videoPackets > 0 && b.videoPackets > 0, "两边都有视频包");
        check(a.videoPackets == b.videoPackets,
              "两份内容的包数一致（同一份编码结果，不是编了两次）");
    }

    std::printf("\n--- 6. 画中画 + 水印（需要两个摄像头）---\n");
    {
        const std::vector<std::string> cameras = CameraCapturer::listDevices();

        if (cameras.size() < 2) {
            std::printf("  这台机器只有 %d 个摄像头，跳过本节。\n",
                        static_cast<int>(cameras.size()));
            std::printf("  想验证的话装个虚拟摄像头（OBS 或 screen-capture-recorder）就行。\n");
        } else {
            StreamConfig config;
            config.sourceType = SourceType::Screen;
            config.audioType = AudioSourceType::SystemAudio;
            config.width = 640;
            config.height = 360;
            config.fps = 30;
            config.videoKbps = 800;

            StreamTarget target;
            target.platform = "compose";
            target.url = "compose_test.flv";
            config.targets = {target};

            config.pipEnabled = true;
            config.pipCameraIndex = 1;
            config.pipCorner = PipCorner::BottomRight;
            config.watermarkText = "LIVE 2026";

            StreamPipeline pipeline;

            std::mutex frameMutex;
            VideoFramePtr captured;

            pipeline.setPreviewCallback([&](const VideoFramePtr& frame) {
                std::lock_guard<std::mutex> lock(frameMutex);
                if (!captured)
                    captured = frame;
            });

            const bool started = pipeline.start(config, log);
            check(started, "带画中画和水印的管线启动成功");

            if (started) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                pipeline.stop();
            }

            VideoFramePtr frame;
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                frame = captured;
            }

            check(frame != nullptr, "抓到了合成后的画面");

            if (frame) {
                check(frame->width == config.width && frame->height == config.height,
                      "合成画面的尺寸等于推流分辨率");
                check(saveBmp("compose_result.bmp", *frame),
                      "已保存为 compose_result.bmp");
                std::printf("  打开这张图确认：右下角有摄像头小窗，左上角有 LIVE 2026\n");
            }

            const FlvInfo info = probeFlv("compose_test.flv");
            check(info.ok && info.streamCount == 2, "同时写出的推流文件也正常");
        }
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::printf("  pipeline_a.flv / pipeline_b.flv 可以直接用播放器打开，\n");
    std::printf("  应该是你的桌面画面加上这段测试期间的系统声音。\n");
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
