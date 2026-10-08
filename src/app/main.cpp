#include <QtGlobal>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

QString formatVersion(unsigned int version)
{
    return QStringLiteral("%1.%2.%3")
        .arg(AV_VERSION_MAJOR(version))
        .arg(AV_VERSION_MINOR(version))
        .arg(AV_VERSION_MICRO(version));
}

void printLine(const char* name, const QString& value)
{
    std::printf("  %-18s %s\n", name, value.toUtf8().constData());
}

bool hasRtmpOutputProtocol()
{
    void* state = nullptr;
    const char* name = nullptr;
    while ((name = avio_enum_protocols(&state, 1)) != nullptr) {
        if (std::strcmp(name, "rtmp") == 0)
            return true;
    }
    return false;
}

bool checkH264Encoder()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (codec) {
        printLine("H.264 编码器", QStringLiteral("OK  (%1)")
                                     .arg(QString::fromLatin1(codec->long_name)));
        return true;
    }

    printLine("H.264 编码器", QStringLiteral("缺失"));
    std::printf("\n");
    std::printf("  当前 FFmpeg 不带 libx264，需要重新安装：\n");
    std::printf("    C:\\vcpkg\\vcpkg remove  ffmpeg:x64-windows\n");
    std::printf("    C:\\vcpkg\\vcpkg install \"ffmpeg[gpl,x264]:x64-windows\"\n");
    std::printf("  （x264 是 GPL 许可，所以 vcpkg 默认不装，必须显式指定）\n");
    return false;
}

bool checkAacEncoder()
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec) {
        printLine("AAC 编码器", QStringLiteral("OK  (%1)")
                                    .arg(QString::fromLatin1(codec->long_name)));
        return true;
    }

    printLine("AAC 编码器", QStringLiteral("缺失"));
    return false;
}

}

int main()
{
#if defined(_WIN32)
    std::system("chcp 65001 > nul");
#endif

    std::printf("================================================\n");
    std::printf("  LightLiveStreamer 环境自检\n");
    std::printf("================================================\n\n");

    bool ok = true;

    std::printf("[1/4] Qt\n");

    printLine("qVersion()", QString::fromLatin1(qVersion()));
    printLine("QT_VERSION_STR", QString::fromLatin1(QT_VERSION_STR));
    if (std::strcmp(qVersion(), QT_VERSION_STR) != 0) {
        std::printf("  警告：运行时 Qt 和编译时 Qt 版本不一致，"
                    "检查 PATH 里是否有别的 Qt 的 bin 目录。\n");
        ok = false;
    }

    std::printf("\n[2/4] FFmpeg\n");
    printLine("av_version_info", QString::fromLatin1(av_version_info()));
    printLine("libavcodec", formatVersion(avcodec_version()));
    printLine("libavformat", formatVersion(avformat_version()));
    printLine("libavutil", formatVersion(avutil_version()));
    printLine("libswscale", formatVersion(swscale_version()));
    printLine("libswresample", formatVersion(swresample_version()));

    std::printf("\n[3/4] 推流协议\n");
    if (hasRtmpOutputProtocol()) {
        printLine("rtmp (输出)", QStringLiteral("OK"));
    } else {
        printLine("rtmp (输出)", QStringLiteral("缺失"));
        std::printf("  当前 FFmpeg 不能推 RTMP，需要完整版 ffmpeg。\n");
        ok = false;
    }

    std::printf("\n[4/4] 编码器\n");
    const bool hasH264 = checkH264Encoder();
    const bool hasAac = checkAacEncoder();
    ok = ok && hasH264 && hasAac;

    std::printf("\n================================================\n");
    if (ok) {
        std::printf("  环境正常，可以进入下一个模块。\n");
    } else {
        std::printf("  环境有问题，请先按上面的提示修好再继续。\n");
    }
    std::printf("================================================\n");
    std::fflush(stdout);

    return ok ? 0 : 1;
}
