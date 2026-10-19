#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include <memory>

struct AVPacketDeleter {
    void operator()(AVPacket* packet) const
    {
        if (packet)
            av_packet_free(&packet);
    }
};

using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
