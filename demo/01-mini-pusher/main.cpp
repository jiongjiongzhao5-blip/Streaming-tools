extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <cstdio>
#include <cstdlib>

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFps = 25;
constexpr int kVideoKbps = 1200;
constexpr int kFrameCount = 150;

[[noreturn]] void fail(const char* what, int err = 0)
{
    if (err != 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, buffer, sizeof(buffer));
        std::fprintf(stderr, "[错误] %s: %s\n", what, buffer);
    } else {
        std::fprintf(stderr, "[错误] %s\n", what);
    }
    std::exit(1);
}

AVCodecContext* createVideoEncoder()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        fail("找不到 libx264，请确认 vcpkg 装的是 ffmpeg[gpl,x264]");

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        fail("avcodec_alloc_context3 失败");

    ctx->time_base = AVRational{1, kFps};
    ctx->framerate = AVRational{kFps, 1};

    ctx->width = kWidth;
    ctx->height = kHeight;

    ctx->gop_size = kFps * 2;

    ctx->max_b_frames = 0;

    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->bit_rate = kVideoKbps * 1000;

    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(ctx, codec, nullptr) < 0)
        fail("avcodec_open2 失败");

    return ctx;
}

AVFrame* generateFrame(AVCodecContext* enc, int index)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame)
        fail("av_frame_alloc 失败");

    frame->format = enc->pix_fmt;
    frame->width = enc->width;
    frame->height = enc->height;

    if (av_frame_get_buffer(frame, 32) < 0)
        fail("av_frame_get_buffer 失败");

    const int w = frame->width;
    const int h = frame->height;

    static const uint8_t barY[8] = {180, 162, 131, 112, 84, 65, 35, 16};
    static const uint8_t barU[8] = {128, 44, 156, 44, 156, 100, 212, 128};
    static const uint8_t barV[8] = {128, 28, 28, 212, 212, 100, 100, 128};

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int bar = x * 8 / w;
            frame->data[0][y * frame->linesize[0] + x] = barY[bar];
        }
    }

    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            const int bar = x * 8 / (w / 2);
            frame->data[1][y * frame->linesize[1] + x] = barU[bar];
            frame->data[2][y * frame->linesize[2] + x] = barV[bar];
        }
    }

    const int barW = 40;
    const int barX = (index * 8) % (w + barW) - barW;
    for (int y = 0; y < h; ++y) {
        for (int x = barX; x < barX + barW; ++x) {
            if (x < 0 || x >= w)
                continue;
            frame->data[0][y * frame->linesize[0] + x] = 235;
            frame->data[1][(y / 2) * frame->linesize[1] + x / 2] = 128;
            frame->data[2][(y / 2) * frame->linesize[2] + x / 2] = 128;
        }
    }

    frame->pts = index;

    return frame;
}

AVFormatContext* openOutput(const char* url, AVCodecContext* enc,
                            AVStream** outStream)
{
    AVFormatContext* fmt = nullptr;

    if (avformat_alloc_output_context2(&fmt, nullptr, "flv", url) < 0 || !fmt)
        fail("avformat_alloc_output_context2 失败");

    AVStream* stream = avformat_new_stream(fmt, nullptr);
    if (!stream)
        fail("avformat_new_stream 失败");

    AVCodecParameters* par = avcodec_parameters_alloc();
    if (!par || avcodec_parameters_from_context(par, enc) < 0) {
        avcodec_parameters_free(&par);
        fail("avcodec_parameters_from_context 失败");
    }
    avcodec_parameters_copy(stream->codecpar, par);
    avcodec_parameters_free(&par);

    stream->time_base = AVRational{1, 1000};

    if (avio_open(&fmt->pb, url, AVIO_FLAG_WRITE) < 0)
        fail("avio_open 失败（文件不可写 或 地址连不上）");

    if (avformat_write_header(fmt, nullptr) < 0)
        fail("avformat_write_header 失败");

    *outStream = stream;
    return fmt;
}

void encodeAndWrite(AVCodecContext* enc, AVFormatContext* fmt,
                    AVStream* stream, AVFrame* frame)
{
    const int sendRet = avcodec_send_frame(enc, frame);
    if (sendRet < 0)
        fail("avcodec_send_frame 失败", sendRet);

    AVPacket* packet = av_packet_alloc();
    if (!packet)
        fail("av_packet_alloc 失败");

    while (true) {
        const int ret = avcodec_receive_packet(enc, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            fail("avcodec_receive_packet 失败", ret);

        av_packet_rescale_ts(packet, enc->time_base, stream->time_base);

        packet->stream_index = stream->index;

        if (av_interleaved_write_frame(fmt, packet) < 0)
            fail("av_interleaved_write_frame 失败");

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

int main(int argc, char** argv)
{
    const char* url = argc > 1 ? argv[1] : "out.flv";

    std::printf("Demo1 最小推流器\n");
    std::printf("  输出   : %s\n", url);
    std::printf("  参数   : %dx%d @ %d fps, %d kbps, %d 帧\n",
                kWidth, kHeight, kFps, kVideoKbps, kFrameCount);

    AVCodecContext* enc = createVideoEncoder();

    AVStream* stream = nullptr;
    AVFormatContext* fmt = openOutput(url, enc, &stream);

    for (int i = 0; i < kFrameCount; ++i) {
        AVFrame* frame = generateFrame(enc, i);
        encodeAndWrite(enc, fmt, stream, frame);
        av_frame_free(&frame);

        if (i % 25 == 0)
            std::printf("  已送编码器 %d 帧...\n", i);
    }

    encodeAndWrite(enc, fmt, stream, nullptr);

    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    avcodec_free_context(&enc);

    std::printf("完成：共编码 %d 帧，已输出到 %s\n", kFrameCount, url);
    return 0;
}
