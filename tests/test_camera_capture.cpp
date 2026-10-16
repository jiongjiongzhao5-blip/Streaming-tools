#include "core/capture/CameraCapturer.h"

#include <cctype>
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

bool looksLikeJpeg(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 4)
        return false;
    return bytes[0] == 0xFF && bytes[1] == 0xD8
           && bytes[bytes.size() - 2] == 0xFF && bytes[bytes.size() - 1] == 0xD9;
}

void printHexPrefix(const std::vector<uint8_t>& bytes, size_t count)
{
    const size_t n = bytes.size() < count ? bytes.size() : count;
    for (size_t i = 0; i < n; ++i)
        std::printf("%02X ", bytes[i]);
    if (bytes.size() > n)
        std::printf("...");
    std::printf("\n");
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return written == bytes.size();
}

int packedBytesPerRow(CameraPixelFormat format, int width)
{
    switch (format) {
    case CameraPixelFormat::RGB24:
        return width * 3;
    case CameraPixelFormat::RGB32:
        return width * 4;
    case CameraPixelFormat::YUY2:
    case CameraPixelFormat::UYVY:
        return width * 2;
    default:
        return 0;
    }
}

}

int countDistinctByteValues(const std::vector<uint8_t>& bytes)
{
    bool seen[256] = {};
    int count = 0;
    for (uint8_t b : bytes) {
        if (!seen[b]) {
            seen[b] = true;
            ++count;
        }
    }
    return count;
}

CameraPixelFormat parseFormat(const char* text)
{
    std::string name(text);
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (name == "rgb24")
        return CameraPixelFormat::RGB24;
    if (name == "rgb32")
        return CameraPixelFormat::RGB32;
    if (name == "yuy2")
        return CameraPixelFormat::YUY2;
    if (name == "uyvy")
        return CameraPixelFormat::UYVY;
    if (name == "mjpg")
        return CameraPixelFormat::MJPG;
    if (name == "any" || name.empty())
        return CameraPixelFormat::Unknown;

    std::printf("  [提示] \"%s\" 不是已知格式名，按 any 处理（不指定格式）。\n",
                text);
    std::printf("         可选: rgb24 / rgb32 / yuy2 / uyvy / mjpg / any\n");
    return CameraPixelFormat::Unknown;
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    int deviceIndex = 0;
    if (argc > 1) {
        deviceIndex = std::atoi(argv[1]);
        if (deviceIndex < 0)
            deviceIndex = 0;
    }

    CameraPixelFormat requestFormat = CameraPixelFormat::RGB24;
    if (argc > 2)
        requestFormat = parseFormat(argv[2]);

    static char stdoutBuffer[8192];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    std::printf("================================================\n");
    std::printf("  模块 5（上）：摄像头采集 —— 过滤器图测试\n");
    std::printf("================================================\n");
    std::printf("  用法: test_camera_capture.exe [设备索引] [期望格式]\n");
    std::printf("        期望格式: rgb24 / rgb32 / yuy2 / uyvy / mjpg / any\n");
    std::printf("        对着同一台摄像头换个格式名再跑一遍，就能亲眼看到\n");
    std::printf("        \"我请求的格式\"和\"我拿到手的格式\"往往不是同一个。\n");
    std::printf("================================================\n");

    std::printf("\n--- 1. 枚举摄像头 ---\n");
    const std::vector<std::string> devices = CameraCapturer::listDevices();
    std::printf("  找到 %d 个视频输入设备：\n", static_cast<int>(devices.size()));
    for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("    [%d] %s\n", static_cast<int>(i),
                    devices[i].empty() ? "<无名字>" : devices[i].c_str());
    }

    if (devices.empty()) {
        std::printf("\n  这个机器上没有摄像头，后面的检查跳过。\n");
        std::printf("  （这不是代码的错，插上一个 USB 摄像头再跑一次即可。）\n");
        std::printf("\n================================================\n");
        std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
        std::printf("================================================\n");
        std::fflush(stdout);
        return g_failed == 0 ? 0 : 1;
    }

    if (deviceIndex >= static_cast<int>(devices.size())) {
        std::printf("\n  指定的设备索引 %d 越界（一共只有 %d 个设备）。\n", deviceIndex,
                    static_cast<int>(devices.size()));
        return 1;
    }

    std::printf("\n--- 2. 打开第 %d 个摄像头 ---\n", deviceIndex);

    std::vector<std::string> logs;
    const auto log = [&logs](const std::string& message) {
        logs.push_back(message);
        std::printf("  [采集器日志] %s\n", message.c_str());
    };

    CameraCapturer capturer;
    const auto openStart = std::chrono::steady_clock::now();
    const bool ok = capturer.open(deviceIndex, log, requestFormat);
    const long long openMs = elapsedMs(openStart);

    check(ok, "open() 返回成功");
    std::printf("  建图耗时 %lld 毫秒\n", openMs);

    if (!ok) {
        std::printf("\n打开失败，后面的测试无法进行。\n");
        std::printf("  常见原因：摄像头被别的程序（钉钉/腾讯会议/相机应用）占用了。\n");
        return 1;
    }

    const int width = capturer.width();
    const int height = capturer.height();
    const CameraPixelFormat format = capturer.pixelFormat();

    check(width > 0 && height > 0, "协商出了有效的分辨率");
    check(format != CameraPixelFormat::Unknown, "协商出了本模块认识的一种像素格式");

    std::printf("\n  协商结果一览：\n");
    std::printf("    分辨率      %d x %d\n", width, height);
    std::printf("    请求的格式  %s%s\n", cameraPixelFormatName(requestFormat),
                requestFormat == CameraPixelFormat::Unknown ? "（不指定，随它给）" : "");
    std::printf("    拿到手的    %s\n", cameraPixelFormatName(format));
    std::printf("    每行字节    %d\n", packedBytesPerRow(format, width));

    if (requestFormat != CameraPixelFormat::Unknown && requestFormat != format) {
        std::printf("    → 请求的和拿到的不一样。DirectShow 为了让两端接上，\n");
        std::printf("      要么在中间插了个转换滤镜，要么干脆没搭理你的请求。\n");
    }

    std::printf("\n--- 3. 连续取帧 ---\n");

    constexpr int kFrames = 30;
    constexpr int kTimeoutMs = 2000;

    int timeouts = 0;
    size_t minSize = 0;
    size_t maxSize = 0;
    bool sizeConsistent = true;
    bool strideConsistent = true;
    int strideOfFirst = -1;

    CameraRawFramePtr firstFrame;
    CameraRawFramePtr lastFrame;

    const auto grabStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        CameraRawFramePtr frame = capturer.nextFrame(kTimeoutMs);
        if (!frame) {
            ++timeouts;
            continue;
        }

        if (!firstFrame) {
            firstFrame = frame;
            strideOfFirst = frame->stride;
            minSize = maxSize = frame->bytes.size();
        } else {
            if (frame->bytes.size() < minSize)
                minSize = frame->bytes.size();
            if (frame->bytes.size() > maxSize)
                maxSize = frame->bytes.size();
            if (frame->bytes.size() != firstFrame->bytes.size())
                sizeConsistent = false;
            if (frame->stride != strideOfFirst)
                strideConsistent = false;
        }
        lastFrame = frame;
    }
    const long long grabMs = elapsedMs(grabStart);

    check(timeouts == 0, "连续 30 帧没有超时");
    check(sizeConsistent && strideConsistent, "每帧的字节数和行跨度都保持一致");
    check(firstFrame != nullptr, "至少取到了第一帧");

    if (!firstFrame)
        return 1;

    std::printf("  取到 %d 帧，用了 %lld 毫秒\n", kFrames - timeouts, grabMs);
    if (grabMs > 0) {
        std::printf("  估算帧率 %.1f fps（含超时等待，仅供参考）\n",
                    1000.0 * (kFrames - timeouts) / grabMs);
    }
    std::printf("  每帧字节数 %zu ~ %zu（首帧 %zu）\n", minSize, maxSize,
                firstFrame->bytes.size());
    std::printf("  首帧 pts=%lld  行跨度=%d  翻转=%s\n",
                static_cast<long long>(firstFrame->pts), firstFrame->stride,
                firstFrame->flipVertical ? "是" : "否");

    std::printf("\n--- 4. 解读这些字节 ---\n");

    const std::string frameBase = "camera_frame_dev" + std::to_string(deviceIndex);

    const int distinctValues = countDistinctByteValues(firstFrame->bytes);
    std::printf("  首帧里用到了 %d 种不同的字节值\n", distinctValues);
    if (distinctValues <= 4) {
        std::printf("  [注意] 这一帧几乎是纯色 —— 真实画面不可能只用到这么几种字节值。\n");
        std::printf("         这不是采集代码的问题，常见原因是：\n");
        std::printf("           - 摄像头被镜头盖/物理挡板挡住，或对着完全均匀的墙面\n");
        std::printf("           - Windows 隐私设置里禁止了摄像头\n");
        std::printf("           - 摄像头被别的程序占用，驱动只往外吐空缓冲\n");
        std::printf("         （全零的 YUV 经过色彩转换后正好是这种 (0,134,0) 的绿色，\n");
        std::printf("           所以\"纯绿\"往往等于\"根本没拿到画面\"。）\n");
        std::printf("         可以换个设备对照一下：test_camera_capture.exe 1\n");
    }

    const size_t expectedStride =
        strideOfFirst > 0
            ? static_cast<size_t>(strideOfFirst) * static_cast<size_t>(height)
            : 0;

    if (format == CameraPixelFormat::MJPG) {
        std::printf("  MJPG：每帧是一整张 JPEG，stride 无意义（当前 %d）\n",
                    strideOfFirst);
        std::printf("  首帧开头 16 字节: ");
        printHexPrefix(firstFrame->bytes, 16);
        check(looksLikeJpeg(firstFrame->bytes),
              "首帧以 FF D8 开头、FF D9 结尾，确实是一张完整 JPEG");
        check(writeFile(frameBase + ".jpg", firstFrame->bytes),
              "首帧已保存为 " + frameBase + ".jpg");
        std::printf("  请直接双击打开 %s.jpg —— 能正常显示就说明字节完全没问题。\n",
                    frameBase.c_str());
    } else if (format == CameraPixelFormat::Unknown) {
        std::printf("  未知子类型，只能确认字节数不为零。\n");
        check(!firstFrame->bytes.empty(), "取到了非空字节");
    } else {
        std::printf("  每行理论字节 %d，实际行跨度 %d\n",
                    packedBytesPerRow(format, width), strideOfFirst);
        if (strideOfFirst == packedBytesPerRow(format, width))
            std::printf("  → 这一路没有行填充，stride 恰好等于 宽度 x 每像素字节数\n");
        else
            std::printf("  → 驱动做了行填充（每行多 %d 字节），"
                        "下一轮转换器必须按 stride 跳行，不能按宽度算\n",
                        strideOfFirst - packedBytesPerRow(format, width));

        check(firstFrame->bytes.size() >= expectedStride,
              "字节数足够装下 stride x height");
        if (firstFrame->bytes.size() > expectedStride) {
            std::printf("  [注意] 驱动报告了 %zu 字节，而一帧真正需要的是 %zu 字节，"
                        "多出 %zu 字节。\n",
                        firstFrame->bytes.size(), expectedStride,
                        firstFrame->bytes.size() - expectedStride);
            std::printf("         多出来的那部分不是画面（实测全是零填充）。\n");
            std::printf("         下一轮转换时必须按 stride x height 读，"
                        "不能把整个 bytes 都当成像素。\n");
        }
        check(writeFile(frameBase + ".raw", firstFrame->bytes),
              "首帧原始字节已保存为 " + frameBase + ".raw");
        std::printf("  首帧开头 16 字节: ");
        printHexPrefix(firstFrame->bytes, 16);
        std::printf("  提示：.raw 是裸数据，用普通看图软件打不开，这是正常的。\n");
        std::printf("  下一轮把它转成 BGRA 之后，才能真正看到画面。\n");
    }

    std::printf("\n--- 5. 画面在动吗 ---\n");

    if (firstFrame && lastFrame) {
        const std::vector<uint8_t>& a = firstFrame->bytes;
        const std::vector<uint8_t>& b = lastFrame->bytes;
        size_t different = 0;
        const size_t compareSize = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < compareSize; ++i) {
            if (a[i] != b[i])
                ++different;
        }
        const double ratio =
            compareSize > 0 ? 100.0 * different / compareSize : 0.0;
        std::printf("  首帧与末帧有 %.1f%% 的字节不同\n", ratio);
        std::printf("  （对着静止的墙拍，这个比例接近 0 是正常的；"
                    "晃一下手再跑，数字应该明显变大）\n");
    }

    std::printf("\n--- 6. 重复开关的稳定性 ---\n");

    for (int round = 0; round < 3; ++round) {
        capturer.close();
        const bool reopened = capturer.open(deviceIndex, nullptr, requestFormat);
        if (!reopened) {
            check(false, "反复 close()/open() 三次都成功");
            break;
        }
        if (!capturer.nextFrame(kTimeoutMs)) {
            check(false, "重开之后还能取到帧");
            break;
        }
        if (round == 2)
            check(true, "反复 close()/open() 三次都成功");
    }

    capturer.close();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::printf("  本轮结论应该长这样：\n");
    std::printf("    - 有分辨率、有格式、有稳定的字节数 -> \"接线\"通了\n");
    std::printf("    - MJPG 的图能打开 / RGB24 的字节数对得上 -> \"解读\"对了\n");
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
