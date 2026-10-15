#include "core/capture/ScreenCapturer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        std::fwrite(frame.bgra.data() + static_cast<size_t>(y) * stride, stride, 1,
                    file);

    std::fclose(file);
    return true;
}

int averageBrightness(const VideoFrame& frame)
{
    const size_t pixelCount = static_cast<size_t>(frame.width) * frame.height;
    if (pixelCount == 0)
        return 0;

    uint64_t sum = 0;
    const uint8_t* p = frame.bgra.data();
    for (size_t i = 0; i < pixelCount; ++i) {
        sum += p[i * 4 + 0];
        sum += p[i * 4 + 1];
        sum += p[i * 4 + 2];
    }
    return static_cast<int>(sum / (pixelCount * 3));
}

long long elapsedMs(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
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
    std::printf("  模块 4：屏幕采集 —— 测试\n");
    std::printf("================================================\n");

    std::vector<std::string> logs;
    const auto log = [&logs](const std::string& message) {
        logs.push_back(message);
        std::printf("  [采集器日志] %s\n", message.c_str());
    };

    std::printf("\n--- 1. 初始化 ---\n");
    ScreenCapturer capturer;
    const auto initStart = std::chrono::steady_clock::now();
    const bool ok = capturer.init(log);
    const long long initMs = elapsedMs(initStart);

    check(ok, "init() 返回成功");
    std::printf("  初始化耗时 %lld 毫秒\n", initMs);

    bool usingDxgi = false;
    for (const std::string& line : logs) {
        if (line.find("DXGI 屏幕采集已启动") != std::string::npos)
            usingDxgi = true;
    }
    std::printf("  当前使用路径: %s\n", usingDxgi ? "DXGI" : "GDI 兜底");

    if (!ok) {
        std::printf("\n初始化失败，后面的测试无法进行。\n");
        return 1;
    }

    std::printf("\n--- 2. 连续抓帧 ---\n");

    constexpr int kFrames = 60;
    int nullFrames = 0;
    int blackFrames = 0;
    int width = 0;
    int height = 0;
    bool sizeConsistent = true;

    int firstBrightness = -1;
    int lastBrightness = -1;
    int minBrightness = 999;
    int maxBrightness = -1;

    VideoFramePtr firstFrame;
    VideoFramePtr lastFrame;

    const auto captureStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        VideoFramePtr frame = capturer.capture();
        if (!frame) {
            ++nullFrames;
            continue;
        }
        if (!firstFrame) {
            firstFrame = frame;
            width = frame->width;
            height = frame->height;
        } else if (frame->width != width || frame->height != height) {
            sizeConsistent = false;
        }

        const int brightness = averageBrightness(*frame);
        if (firstBrightness < 0)
            firstBrightness = brightness;
        lastBrightness = brightness;
        if (brightness < minBrightness)
            minBrightness = brightness;
        if (brightness > maxBrightness)
            maxBrightness = brightness;
        if (brightness <= 1)
            ++blackFrames;

        lastFrame = frame;
    }
    const long long captureMs = elapsedMs(captureStart);

    std::printf("  抓了 %d 次，得到有效帧 %d 个，空帧 %d 个\n", kFrames,
                kFrames - nullFrames, nullFrames);
    std::printf("  分辨率 %dx%d，总耗时 %lld 毫秒，平均每帧 %.2f 毫秒\n", width,
                height, captureMs, captureMs / static_cast<double>(kFrames));
    std::printf("  亮度: 首帧 %d, 末帧 %d, 最小 %d, 最大 %d, 全黑帧数 %d/%d\n",
                firstBrightness, lastBrightness, minBrightness, maxBrightness,
                blackFrames, kFrames - nullFrames);

    check(nullFrames == 0, "没有空帧");
    check(width > 0 && height > 0, "得到了有效的分辨率");
    check(sizeConsistent, "所有帧的尺寸一致");
    check(width >= 640 && height >= 480, "分辨率看起来是正常的桌面尺寸");

    std::printf("\n--- 3. 静止画面下的兜底行为 ---\n");

    int burstNulls = 0;
    constexpr int kBurst = 200;
    for (int i = 0; i < kBurst; ++i) {
        if (!capturer.capture())
            ++burstNulls;
    }
    std::printf("  连续快速抓 %d 次，空帧 %d 个\n", kBurst, burstNulls);
    check(burstNulls == 0, "快速连抓也没有空帧（屏幕静止时返回上一帧的拷贝）");

    std::printf("\n--- 4. 画面内容检查 ---\n");

    const int validFrames = kFrames - nullFrames;
    if (validFrames > 0) {
        const int nonBlack = validFrames - blackFrames;
        std::printf("  非全黑帧 %d / %d\n", nonBlack, validFrames);
        check(nonBlack > 0, "至少有一部分帧不是全黑（采集确实拿到了画面）");
        check(maxBrightness < 255, "画面不是全白（全白往往说明内存没被正确填充）");

        if (blackFrames > 0) {
            std::printf("\n  提示：有 %d 帧是全黑的。\n", blackFrames);
            std::printf("  这是 DXGI Desktop Duplication 的已知行为：\n");
            std::printf("  刚开始采集时，系统还没有合成出新的桌面画面，\n");
            std::printf("  前几帧可能是空帧。看下面保存的图片就能判断是"
                        "\"只有头几帧黑\"还是\"一直黑\"。\n");
        }
    }

    std::printf("\n--- 5. 保存图片 ---\n");

    if (firstFrame) {
        check(saveBmp("screen_capture_first.bmp", *firstFrame),
              "首帧已保存为 screen_capture_first.bmp");
    }
    if (lastFrame) {
        check(saveBmp("screen_capture_last.bmp", *lastFrame),
              "末帧已保存为 screen_capture_last.bmp");
    }
    std::printf("\n  请打开这两个 bmp 文件，确认内容是你的桌面。\n");
    std::printf("  检查要点：\n");
    std::printf("    - 画面是否上下颠倒（GDI 路径最容易出这个问题）\n");
    std::printf("    - 是否有斜纹（RowPitch 处理错了的典型症状）\n");
    std::printf("    - 是否只抓到屏幕左上角一块（DPI 缩放问题）\n");

    capturer.close();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
