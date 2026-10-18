#include "core/capture/AudioCapturer.h"

#if defined(_WIN32)
#include <windows.h>

#include <mmsystem.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

double computeRms(const std::vector<float>& samples)
{
    if (samples.empty())
        return 0.0;
    double sum = 0.0;
    for (float v : samples)
        sum += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

bool saveWav(const std::string& path, const std::vector<float>& samples,
             int sampleRate, int channels)
{
    if (samples.empty() || sampleRate <= 0 || channels <= 0)
        return false;

    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
        pcm[i] = static_cast<int16_t>(clamped * 32767.0f);
    }

    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(int16_t));
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * blockAlign;

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;

    const auto writeU32 = [file](uint32_t v) { std::fwrite(&v, 4, 1, file); };
    const auto writeU16 = [file](uint16_t v) { std::fwrite(&v, 2, 1, file); };

    std::fwrite("RIFF", 1, 4, file);
    writeU32(36 + dataBytes);
    std::fwrite("WAVE", 1, 4, file);

    std::fwrite("fmt ", 1, 4, file);
    writeU32(16);
    writeU16(1);
    writeU16(static_cast<uint16_t>(channels));
    writeU32(static_cast<uint32_t>(sampleRate));
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(16);

    std::fwrite("data", 1, 4, file);
    writeU32(dataBytes);
    std::fwrite(pcm.data(), 1, dataBytes, file);

    std::fclose(file);
    return true;
}

struct CaptureReport {
    int chunks = 0;
    int64_t totalFrames = 0;
    long long wallMs = 0;
    bool ptsMonotonic = true;
    bool ptsContinuous = true;
    bool metaConsistent = true;
    int ptsGaps = 0;
    double rms = 0.0;
    std::vector<float> samples;
    std::map<int64_t, int> sizeHistogram;
};

CaptureReport captureFor(AudioCapturer& capturer, int durationMs)
{
    CaptureReport report;

    const int expectedRate = capturer.sampleRate();
    const int expectedChannels = capturer.channels();

    int64_t expectedPts = 0;
    int64_t lastPts = -1;
    bool first = true;

    const auto start = std::chrono::steady_clock::now();
    while (elapsedMs(start) < durationMs) {
        AudioChunkPtr chunk = capturer.nextChunk(50);
        if (!chunk)
            continue;

        const int64_t frames =
            chunk->channels > 0
                ? static_cast<int64_t>(chunk->samples.size() / chunk->channels)
                : 0;

        if (chunk->sampleRate != expectedRate || chunk->channels != expectedChannels)
            report.metaConsistent = false;

        if (lastPts >= 0 && chunk->pts <= lastPts)
            report.ptsMonotonic = false;

        if (!first && chunk->pts != expectedPts) {
            report.ptsContinuous = false;
            ++report.ptsGaps;
        }
        first = false;
        lastPts = chunk->pts;
        expectedPts = chunk->pts + frames;

        ++report.chunks;
        report.totalFrames += frames;
        ++report.sizeHistogram[frames];
        report.samples.insert(report.samples.end(), chunk->samples.begin(),
                              chunk->samples.end());
    }

    report.wallMs = elapsedMs(start);
    report.rms = computeRms(report.samples);
    return report;
}

}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    static char stdoutBuffer[8192];
    std::setvbuf(stdout, stdoutBuffer, _IOLBF, sizeof(stdoutBuffer));

    int durationMs = 2000;
    bool withSound = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sound") == 0) {
            withSound = true;
        } else {
            const int requested = std::atoi(argv[i]);
            if (requested >= 500 && requested <= 30000)
                durationMs = requested;
        }
    }

    std::printf("================================================\n");
    std::printf("  模块 6：音频采集（WASAPI）测试\n");
    std::printf("================================================\n");
    std::printf("  用法: test_audio_capture.exe [采集毫秒数] [--sound]\n");
    std::printf("        --sound  采集时自己播放一段系统提示音，用来证明\n");
    std::printf("                  loopback 真的能收到声音，而不是只有静音补帧\n");
    std::printf("================================================\n");

    std::printf("\n--- 1. 枚举设备 ---\n");
    {
        const auto speakers = AudioCapturer::listDevices(AudioSourceType::SystemAudio);
        const auto mics = AudioCapturer::listDevices(AudioSourceType::Microphone);

        std::printf("  播放设备（用来抓系统声音）%d 个：\n",
                    static_cast<int>(speakers.size()));
        for (size_t i = 0; i < speakers.size(); ++i)
            std::printf("    [%d] %s\n", static_cast<int>(i),
                        speakers[i].empty() ? "<无名字>" : speakers[i].c_str());

        std::printf("  录音设备（麦克风）%d 个：\n", static_cast<int>(mics.size()));
        for (size_t i = 0; i < mics.size(); ++i)
            std::printf("    [%d] %s\n", static_cast<int>(i),
                        mics[i].empty() ? "<无名字>" : mics[i].c_str());

        check(!speakers.empty(), "至少有一个播放设备（系统声音要靠它采）");
    }

    std::vector<std::string> logs;
    const auto log = [&logs](const std::string& message) {
        logs.push_back(message);
        std::printf("  [采集器日志] %s\n", message.c_str());
    };

    std::printf("\n--- 2. 采集系统声音 %d 毫秒 ---\n", durationMs);
    std::printf("  建议这段时间放点音乐或者视频，不然采到的会全是静音。\n");
    std::printf("  但即便是静音，下面的帧数也必须对得上 —— 那才是重点。\n");

    AudioCapturer capturer;
    if (!capturer.open(AudioSourceType::SystemAudio, -1, log)) {
        std::printf("\n  打开系统声音失败，后面的测试无法进行。\n");
        std::printf("================================================\n");
        std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
        std::printf("================================================\n");
        std::fflush(stdout);
        return 1;
    }

    const int sampleRate = capturer.sampleRate();
    const int channels = capturer.channels();

    check(sampleRate > 0, "协商出了有效的采样率");
    check(channels > 0, "协商出了有效的声道数");
    std::printf("  实际格式: %d Hz, %d 声道\n", sampleRate, channels);

    if (withSound) {
        const BOOL started =
            PlaySoundW(L"C:\\Windows\\Media\\Alarm01.wav", nullptr,
                       SND_FILENAME | SND_ASYNC | SND_LOOP);
        std::printf("  [测试] %s正在循环播放系统提示音\n",
                    started ? "" : "播放失败（找不到文件），");
    }

    const CaptureReport report = captureFor(capturer, durationMs);
    const long long wallMs = report.wallMs;

    if (withSound)
        PlaySoundW(nullptr, nullptr, 0);

    capturer.close();

    std::printf("\n  采到 %d 块，共 %lld 个采样帧\n", report.chunks,
                static_cast<long long>(report.totalFrames));
    std::printf("  实际耗时 %lld 毫秒，相当于 %lld 帧\n", wallMs,
                static_cast<long long>(static_cast<int64_t>(sampleRate) * wallMs / 1000));
    std::printf("  整体 RMS = %.5f %s\n", report.rms,
                report.rms < 0.0005 ? "（几乎是静音）" : "（有声音）");

    std::vector<std::pair<int64_t, int>> sizes(report.sizeHistogram.begin(),
                                               report.sizeHistogram.end());
    std::sort(sizes.begin(), sizes.end(),
              [](const std::pair<int64_t, int>& a, const std::pair<int64_t, int>& b) {
                  return a.second > b.second;
              });

    std::printf("  块大小分布（前 5 种）：\n");
    for (size_t i = 0; i < sizes.size() && i < 5; ++i) {
        std::printf("    %6lld 帧 (%.1f 毫秒)  x %d 次\n",
                    static_cast<long long>(sizes[i].first),
                    1000.0 * static_cast<double>(sizes[i].first) / sampleRate,
                    sizes[i].second);
    }

    if (report.rms < 0.0005) {
        std::printf("\n  RMS 为 0 有两种可能：系统真的没出声，或者系统音量被拉到 0 了。\n");
        std::printf("  看上面的分布就能分辨：\n");
        std::printf("    - 几乎全是 10 毫秒左右一块 -> 数据全是补的静音，\n");
        std::printf("      loopback 一个包都没给，先把系统音量调大再跑。\n");
        std::printf("    - 出现了 20 毫秒或 1024 帧之类的其它大小 -> loopback 在工作，\n");
        std::printf("      只是那两秒确实安静。\n");
    }

    std::printf("\n--- 3. 时间轴是否连续 ---\n");
    std::printf("  这一节是音频采集的重点。视频掉一帧只是卡一下，\n");
    std::printf("  音频少一块就是音画不同步，而且会一直累积。\n");

    check(report.chunks > 0, "至少采到了一块数据");
    check(report.metaConsistent, "每一块都自带正确的采样率和声道数");

    const int64_t expectedFrames =
        static_cast<int64_t>(sampleRate) * wallMs / 1000;
    const double errorRatio =
        expectedFrames > 0
            ? std::fabs(static_cast<double>(report.totalFrames - expectedFrames))
                  / static_cast<double>(expectedFrames)
            : 1.0;

    std::printf("  应有 %lld 帧，实有 %lld 帧，偏差 %.2f%%\n",
                static_cast<long long>(expectedFrames),
                static_cast<long long>(report.totalFrames), errorRatio * 100.0);

    check(errorRatio < 0.05,
          "总帧数与真实时间对得上（偏差 < 5%，说明没有静默丢时间）");
    check(report.ptsMonotonic, "pts 始终向前，没有倒流");
    check(report.ptsContinuous, "pts 连续，块与块之间没有空洞");

    if (report.ptsGaps > 0)
        std::printf("  提示：出现了 %d 处 pts 跳变，通常是消费者太慢、队列丢了旧块\n",
                    report.ptsGaps);

    if (withSound) {
        const bool heardSomething = report.rms > 0.0005;
        check(heardSomething, "播放提示音期间，loopback 收到了非零样本");
        if (!heardSomething) {
            std::printf("\n  loopback 没收到任何声音，而测试确实在播放。可能是：\n");
            std::printf("    - 系统音量被调到 0 或静音了（最常见）\n");
            std::printf("    - 默认播放设备是 HDMI/虚拟声卡，声音去了别处\n");
            std::printf("    - 这台机器的音频驱动不支持 loopback\n");
        }
    }

    if (report.rms < 0.0005) {
        std::printf("\n  这一轮采到的几乎是静音。如果刚才确实没在放声音，\n");
        std::printf("  那么\"帧数还对得上\"恰好证明了静音补帧在起作用 ——\n");
        std::printf("  WASAPI 的 loopback 在静音时一个包都不给，全靠补。\n");
    }

    std::printf("\n--- 4. 保存成 wav ---\n");
    check(saveWav("audio_system.wav", report.samples, sampleRate, channels),
          "已保存为 audio_system.wav");
    std::printf("  双击 audio_system.wav 应该能听到刚才系统播放的声音。\n");
    std::printf("  如果听到的是噪声、断断续续或者速度不对，说明声道/采样率理解错了。\n");

    std::printf("\n--- 5. 麦克风 ---\n");
    {
        AudioCapturer mic;
        if (!mic.open(AudioSourceType::Microphone, -1, log)) {
            std::printf("  没有可用的麦克风，跳过（不算失败）\n");
        } else {
            const int micRate = mic.sampleRate();
            const int micChannels = mic.channels();
            std::printf("  麦克风格式: %d Hz, %d 声道\n", micRate, micChannels);

            const CaptureReport micReport = captureFor(mic, 1000);
            mic.close();

            check(micReport.chunks > 0, "麦克风采到了数据");

            const int64_t micExpected =
                static_cast<int64_t>(micRate) * micReport.wallMs / 1000;
            const double micError =
                micExpected > 0
                    ? std::fabs(static_cast<double>(micReport.totalFrames - micExpected))
                          / static_cast<double>(micExpected)
                    : 1.0;
            std::printf("  %lld 毫秒采到 %lld 帧，偏差 %.2f%%\n", micReport.wallMs,
                        static_cast<long long>(micReport.totalFrames),
                        micError * 100.0);
            check(micError < 0.10, "麦克风的时间轴也对得上（偏差 < 10%）");

            check(saveWav("audio_mic.wav", micReport.samples, micRate, micChannels),
                  "已保存为 audio_mic.wav");
            if (micReport.rms < 0.0005)
                std::printf("  （麦克风没收到声音，对着它说句话再跑一次会更有意思）\n");
        }
    }

    std::printf("\n--- 6. 反复开关的稳定性 ---\n");
    {
        AudioCapturer loop;
        bool allOk = true;
        for (int round = 0; round < 3; ++round) {
            if (!loop.open(AudioSourceType::SystemAudio, -1, nullptr)) {
                allOk = false;
                break;
            }
            if (!loop.nextChunk(500)) {
                allOk = false;
                break;
            }
            loop.close();
        }
        check(allOk, "连续 open/取数/close 三次都没问题");
    }

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");
    std::printf("  音频验收只要记住一条：\n");
    std::printf("    采了多久，就该有多少帧。少一帧都会在几分钟后变成音画不同步。\n");
    std::printf("================================================\n");
    std::fflush(stdout);

    return g_failed == 0 ? 0 : 1;
}
