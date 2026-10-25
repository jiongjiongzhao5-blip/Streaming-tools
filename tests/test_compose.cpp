#include "core/compose/VideoCompositor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

void check(bool condition, const std::string& what)
{
    check(condition, what.c_str());
}

VideoFrame makeSolid(int width, int height, uint8_t b, uint8_t g, uint8_t r)
{
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts = 42;
    frame.bgra.resize(static_cast<size_t>(width) * height * 4);

    for (size_t i = 0; i < frame.bgra.size(); i += 4) {
        frame.bgra[i + 0] = b;
        frame.bgra[i + 1] = g;
        frame.bgra[i + 2] = r;
        frame.bgra[i + 3] = 255;
    }
    return frame;
}

bool pixelIs(const VideoFrame& frame, int x, int y, uint8_t b, uint8_t g, uint8_t r,
             int tolerance)
{
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height)
        return false;

    const size_t offset = (static_cast<size_t>(y) * frame.width + x) * 4;
    if (offset + 3 >= frame.bgra.size())
        return false;

    const uint8_t* p = frame.bgra.data() + offset;
    return std::abs(static_cast<int>(p[0]) - b) <= tolerance
           && std::abs(static_cast<int>(p[1]) - g) <= tolerance
           && std::abs(static_cast<int>(p[2]) - r) <= tolerance;
}

int brightestIn(const VideoFrame& frame, int x0, int y0, int x1, int y1)
{
    int brightest = 0;

    for (int y = y0; y < y1 && y < frame.height; ++y) {
        for (int x = x0; x < x1 && x < frame.width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * frame.width + x) * 4;
            if (offset + 3 >= frame.bgra.size())
                continue;
            const uint8_t* p = frame.bgra.data() + offset;
            const int value = std::min({p[0], p[1], p[2]});
            if (value > brightest)
                brightest = value;
        }
    }
    return brightest;
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
    std::printf("  模块 15：画面合成（画中画 / 水印）测试\n");
    std::printf("================================================\n");

    std::printf("\n--- 1. 尺寸一致时不做缩放 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;

        VideoCompositor compositor;
        check(compositor.configure(config, nullptr), "合成器配置成功");
        check(compositor.isConfigured(), "处于已配置状态");

        const VideoFrame base = makeSolid(640, 360, 255, 0, 0);
        const VideoFramePtr out = compositor.compose(base, nullptr);

        check(out != nullptr, "合成成功");
        if (out) {
            check(out->width == 640 && out->height == 360, "输出就是画布尺寸");
            check(out->pts == base.pts, "pts 沿用主画面");
            check(out->bgra.size() == 640u * 360u * 4u, "bgra 大小正确");
            check(pixelIs(*out, 10, 10, 255, 0, 0, 2), "主画面内容原样保留");
            check(pixelIs(*out, 630, 350, 255, 0, 0, 2), "右下角也还是主画面");
        }
    }

    std::printf("\n--- 2. 尺寸不一致时缩放到画布 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;

        VideoCompositor compositor;
        compositor.configure(config, nullptr);

        const VideoFrame base = makeSolid(320, 180, 255, 0, 0);
        const VideoFramePtr out = compositor.compose(base, nullptr);

        check(out != nullptr, "缩放后仍能合成");
        if (out) {
            check(out->width == 640 && out->height == 360, "输出是画布尺寸而不是输入尺寸");
            check(pixelIs(*out, 320, 180, 255, 0, 0, 8), "缩放后的中心还是蓝色");
            check(pixelIs(*out, 5, 5, 255, 0, 0, 8), "左上角也是蓝色（没有黑边）");
        }
    }

    std::printf("\n--- 3. 画中画：位置、尺寸、宽高比 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;
        config.pipEnabled = true;
        config.pipCorner = PipCorner::BottomRight;

        VideoCompositor compositor;
        compositor.configure(config, nullptr);

        const VideoFrame base = makeSolid(640, 360, 255, 0, 0);
        const VideoFrame overlay = makeSolid(160, 120, 0, 0, 255);
        const VideoFramePtr out = compositor.compose(base, &overlay);

        check(out != nullptr, "带小窗的合成成功");
        if (out) {
            const int px = compositor.pipX();
            const int py = compositor.pipY();
            const int pw = compositor.pipWidth();
            const int ph = compositor.pipHeight();

            std::printf("    小窗矩形: (%d, %d) %dx%d\n", px, py, pw, ph);

            check(pw == 160, "小窗宽度取画布宽度的四分之一");
            check(ph == 120, "小窗高度保持 4:3 比例（160x120）");

            check(px + pw <= 640 && 640 - (px + pw) <= 16, "小窗贴着右边");
            check(py + ph <= 360 && 360 - (py + ph) <= 16, "小窗贴着下边");

            check(pixelIs(*out, px + pw / 2, py + ph / 2, 0, 0, 255, 6),
                  "小窗中心是摄像头画面（红）");
            check(pixelIs(*out, px + 4, py + 4, 0, 0, 255, 12), "小窗左上角也是红");
            check(pixelIs(*out, px - 8, py + ph / 2, 255, 0, 0, 2),
                  "小窗左边一个像素之外是主画面（蓝）");
            check(pixelIs(*out, 100, 100, 255, 0, 0, 2), "远离小窗的地方仍是主画面");
        }
    }

    std::printf("\n--- 4. 小窗没数据时留空，主画面不受影响 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;
        config.pipEnabled = true;

        VideoCompositor compositor;
        compositor.configure(config, nullptr);

        const VideoFrame base = makeSolid(640, 360, 255, 0, 0);
        const VideoFramePtr out = compositor.compose(base, nullptr);

        check(out != nullptr, "没有小窗画面也能合成");
        if (out) {
            check(pixelIs(*out, 320, 180, 255, 0, 0, 2),
                  "整幅都是主画面，没有黑块也没有崩溃");
        }
    }

    std::printf("\n--- 5. 四个角落 ---\n");
    {
        struct Case {
            PipCorner corner;
            const char* name;
        };
        const Case cases[] = {{PipCorner::TopLeft, "左上"},
                              {PipCorner::TopRight, "右上"},
                              {PipCorner::BottomLeft, "左下"},
                              {PipCorner::BottomRight, "右下"}};

        for (const Case& item : cases) {
            CompositorConfig config;
            config.canvasWidth = 640;
            config.canvasHeight = 360;
            config.pipEnabled = true;
            config.pipCorner = item.corner;

            VideoCompositor compositor;
            compositor.configure(config, nullptr);

            const VideoFrame base = makeSolid(640, 360, 255, 0, 0);
            const VideoFrame overlay = makeSolid(160, 120, 0, 0, 255);
            const VideoFramePtr out = compositor.compose(base, &overlay);

            const bool inLeftHalf = compositor.pipX() < 320;
            const bool inTopHalf = compositor.pipY() < 180;

            const bool expectedLeft = (item.corner == PipCorner::TopLeft
                                       || item.corner == PipCorner::BottomLeft);
            const bool expectedTop = (item.corner == PipCorner::TopLeft
                                      || item.corner == PipCorner::TopRight);

            const bool centered = out
                                  && pixelIs(*out, compositor.pipX() + compositor.pipWidth() / 2,
                                             compositor.pipY() + compositor.pipHeight() / 2,
                                             0, 0, 255, 8);

            check(inLeftHalf == expectedLeft && inTopHalf == expectedTop && centered,
                  std::string(item.name) + "角位置正确");
        }
    }

    std::printf("\n--- 6. 文字水印 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;
        config.watermarkText = "LIVE";
        config.watermarkFontSize = 28;

        VideoCompositor compositor;
        check(compositor.configure(config, nullptr), "带水印的合成器配置成功");

        const VideoFrame base = makeSolid(640, 360, 40, 40, 40);
        const VideoFramePtr out = compositor.compose(base, nullptr);

        check(out != nullptr, "水印合成成功");
        if (out) {
            const int brightest = brightestIn(*out, 0, 0, 260, 80);
            std::printf("    左上角区域最亮像素 = %d\n", brightest);
            check(brightest > 200, "左上角确实画上了白字");
            check(brightestIn(*out, 0, 200, 640, 360) < 60,
                  "水面只出现在左上角，没铺满全屏");
        }

        CompositorConfig chinese = config;
        chinese.watermarkText = "直播中";
        VideoCompositor compositor2;
        check(compositor2.configure(chinese, nullptr), "中文水印配置成功");
        const VideoFramePtr out2 = compositor2.compose(base, nullptr);
        check(out2 != nullptr && brightestIn(*out2, 0, 0, 260, 80) > 200,
              "中文水印也画上去了");
    }

    std::printf("\n--- 7. 异常输入 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 640;
        config.canvasHeight = 360;

        VideoCompositor compositor;
        compositor.configure(config, nullptr);

        VideoFrame empty;
        check(compositor.compose(empty, nullptr) == nullptr, "空主画面返回 nullptr");
        std::printf("    失败原因: %s\n", compositor.lastError().c_str());

        VideoCompositor unconfigured;
        const VideoFrame base = makeSolid(640, 360, 255, 0, 0);
        check(unconfigured.compose(base, nullptr) == nullptr,
              "没配置就合成，返回 nullptr 而不是崩溃");
    }

    std::printf("\n--- 8. 合成耗时 ---\n");
    {
        CompositorConfig config;
        config.canvasWidth = 1280;
        config.canvasHeight = 720;
        config.pipEnabled = true;
        config.watermarkText = "LIVE 2026";

        VideoCompositor compositor;
        compositor.configure(config, nullptr);

        const VideoFrame base = makeSolid(1920, 1080, 255, 0, 0);
        const VideoFrame overlay = makeSolid(640, 480, 0, 0, 255);

        const auto start = std::chrono::steady_clock::now();
        constexpr int kRounds = 30;
        for (int i = 0; i < kRounds; ++i)
            compositor.compose(base, &overlay);
        const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();

        const double perFrame = static_cast<double>(totalMs) / kRounds;
        std::printf("    1920x1080 主画面 -> 1280x720 画布 + 小窗 + 水印\n");
        std::printf("    平均 %.2f 毫秒/帧\n", perFrame);
        check(perFrame < 33.0, "单帧合成耗时低于 33 毫秒，够跑 30fps");
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
