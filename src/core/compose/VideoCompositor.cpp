#include "core/compose/VideoCompositor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::wstring toWide(const std::string& text)
{
    if (text.empty())
        return {};

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), length);
    return wide;
}

void blitRow(uint8_t* dst, const uint8_t* src, int bytes)
{
    std::memcpy(dst, src, static_cast<size_t>(bytes));
}

}

struct VideoCompositor::Impl {
    CompositorConfig config;
    LogCallback log;
    std::string lastError;

    bool configured = false;

    SwsContext* baseScale = nullptr;
    int baseSrcWidth = 0;
    int baseSrcHeight = 0;

    SwsContext* pipScale = nullptr;
    int pipSrcWidth = 0;
    int pipSrcHeight = 0;

    int pipX = 0;
    int pipY = 0;
    int pipW = 0;
    int pipH = 0;
    bool pipRectValid = false;

    int maskWidth = 0;
    int maskHeight = 0;
    std::vector<uint8_t> mask;

    void emit(const std::string& message)
    {
        if (log)
            log(message);
    }

    void release()
    {
        if (baseScale) {
            sws_freeContext(baseScale);
            baseScale = nullptr;
        }
        if (pipScale) {
            sws_freeContext(pipScale);
            pipScale = nullptr;
        }
        baseSrcWidth = baseSrcHeight = 0;
        pipSrcWidth = pipSrcHeight = 0;
        pipRectValid = false;
        mask.clear();
        maskWidth = maskHeight = 0;
        configured = false;
    }

    bool buildTextMask();
    bool updatePipRect(int overlayWidth, int overlayHeight);
    void blendMask(uint8_t* canvas, int canvasWidth, int canvasHeight, int originX,
                   int originY, uint8_t r, uint8_t g, uint8_t b, float opacity);
};

bool VideoCompositor::Impl::buildTextMask()
{
    mask.clear();
    maskWidth = 0;
    maskHeight = 0;

    if (config.watermarkText.empty())
        return true;

    const std::wstring wide = toWide(config.watermarkText);
    if (wide.empty()) {
        lastError = "水印文字为空或编码转换失败";
        return false;
    }

    const int fontSize = std::max(8, config.watermarkFontSize);

    HDC screenDc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screenDc);
    if (screenDc)
        ReleaseDC(nullptr, screenDc);
    if (!dc) {
        lastError = "创建文字渲染 DC 失败";
        return false;
    }

    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(dc, font);

    SIZE extent = {};
    GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &extent);

    maskWidth = std::max(8, static_cast<int>(extent.cx) + 8);
    maskHeight = std::max(8, static_cast<int>(extent.cy) + 8);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = maskWidth;
    bmi.bmiHeader.biHeight = -maskHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        lastError = "创建文字位图失败";
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    std::memset(bits, 0, static_cast<size_t>(maskWidth) * maskHeight * 4);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    RECT rect = {4, 4, maskWidth, maskHeight};
    DrawTextW(dc, wide.c_str(), static_cast<int>(wide.size()), &rect,
              DT_LEFT | DT_TOP | DT_NOPREFIX);

    GdiFlush();

    mask.resize(static_cast<size_t>(maskWidth) * maskHeight);
    const uint8_t* pixels = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < mask.size(); ++i)
        mask[i] = pixels[i * 4 + 2];

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);

    return true;
}

bool VideoCompositor::Impl::updatePipRect(int overlayWidth, int overlayHeight)
{
    if (pipRectValid && pipSrcWidth == overlayWidth && pipSrcHeight == overlayHeight)
        return true;

    int width = config.pipWidth > 0 ? config.pipWidth : config.canvasWidth / 4;
    width = std::max(16, width);
    if (width > config.canvasWidth)
        width = config.canvasWidth;
    width &= ~1;

    int height = static_cast<int>(static_cast<int64_t>(width) * overlayHeight / overlayWidth);
    height = std::max(16, height);
    if (height > config.canvasHeight)
        height = config.canvasHeight;
    height &= ~1;

    const int margin = std::max(8, config.canvasWidth / 64);

    int x = margin;
    int y = margin;
    switch (config.pipCorner) {
    case PipCorner::TopLeft:
        break;
    case PipCorner::TopRight:
        x = config.canvasWidth - width - margin;
        break;
    case PipCorner::BottomLeft:
        y = config.canvasHeight - height - margin;
        break;
    case PipCorner::BottomRight:
        x = config.canvasWidth - width - margin;
        y = config.canvasHeight - height - margin;
        break;
    }

    pipX = std::max(0, x);
    pipY = std::max(0, y);
    pipW = width;
    pipH = height;
    pipSrcWidth = overlayWidth;
    pipSrcHeight = overlayHeight;
    pipRectValid = true;

    if (pipScale) {
        sws_freeContext(pipScale);
        pipScale = nullptr;
    }
    pipScale = sws_getContext(overlayWidth, overlayHeight, AV_PIX_FMT_BGRA, width,
                              height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                              nullptr);
    if (!pipScale) {
        lastError = "创建画中画缩放上下文失败";
        pipRectValid = false;
        return false;
    }
    return true;
}

void VideoCompositor::Impl::blendMask(uint8_t* canvas, int canvasWidth, int canvasHeight,
                                      int originX, int originY, uint8_t r, uint8_t g,
                                      uint8_t b, float opacity)
{
    if (mask.empty())
        return;

    for (int my = 0; my < maskHeight; ++my) {
        const int y = originY + my;
        if (y < 0 || y >= canvasHeight)
            continue;

        const uint8_t* maskRow = mask.data() + static_cast<size_t>(my) * maskWidth;
        uint8_t* dstRow =
            canvas + static_cast<size_t>(y) * canvasWidth * 4 + static_cast<size_t>(originX) * 4;

        const int xBegin = std::max(0, -originX);
        const int xEnd = std::min(maskWidth, canvasWidth - originX);

        for (int mx = xBegin; mx < xEnd; ++mx) {
            const int coverage = static_cast<int>(maskRow[mx] * opacity);
            if (coverage <= 0)
                continue;

            uint8_t* p = dstRow + static_cast<size_t>(mx) * 4;
            const int inverse = 255 - coverage;
            p[0] = static_cast<uint8_t>((b * coverage + p[0] * inverse) / 255);
            p[1] = static_cast<uint8_t>((g * coverage + p[1] * inverse) / 255);
            p[2] = static_cast<uint8_t>((r * coverage + p[2] * inverse) / 255);
            p[3] = 255;
        }
    }
}

VideoCompositor::VideoCompositor() : impl_(std::make_unique<Impl>())
{
}

VideoCompositor::~VideoCompositor()
{
    close();
}

bool VideoCompositor::configure(const CompositorConfig& config, LogCallback log)
{
    close();

    impl_->config = config;
    impl_->log = std::move(log);
    impl_->lastError.clear();

    if (config.canvasWidth <= 0 || config.canvasHeight <= 0) {
        impl_->lastError = "画布尺寸无效";
        return false;
    }

    impl_->config.canvasWidth = config.canvasWidth & ~1;
    impl_->config.canvasHeight = config.canvasHeight & ~1;

    if (!impl_->buildTextMask()) {
        close();
        return false;
    }

    impl_->configured = true;

    impl_->emit("合成器已就绪: 画布 " + std::to_string(impl_->config.canvasWidth) + "x"
                + std::to_string(impl_->config.canvasHeight)
                + (config.pipEnabled ? "，画中画开启" : "")
                + (config.watermarkText.empty() ? "" : "，水印: " + config.watermarkText));

    return true;
}

void VideoCompositor::close()
{
    impl_->release();
}

bool VideoCompositor::isConfigured() const
{
    return impl_->configured;
}

VideoFramePtr VideoCompositor::compose(const VideoFrame& base, const VideoFrame* overlay)
{
    impl_->lastError.clear();

    if (!impl_->configured) {
        impl_->lastError = "合成器还没配置";
        return nullptr;
    }

    const int canvasWidth = impl_->config.canvasWidth;
    const int canvasHeight = impl_->config.canvasHeight;

    if (canvasWidth <= 0 || canvasHeight <= 0) {
        impl_->lastError = "画布尺寸无效";
        return nullptr;
    }

    const size_t baseBytes = static_cast<size_t>(base.width) * base.height * 4;
    if (base.width <= 0 || base.height <= 0 || base.bgra.size() < baseBytes) {
        impl_->lastError = "主画面数据无效";
        return nullptr;
    }

    auto out = std::make_shared<VideoFrame>();
    out->width = canvasWidth;
    out->height = canvasHeight;
    out->pts = base.pts;

    if (base.width == canvasWidth && base.height == canvasHeight) {
        out->bgra = base.bgra;
    } else {
        if (!impl_->baseScale || impl_->baseSrcWidth != base.width
            || impl_->baseSrcHeight != base.height) {
            if (impl_->baseScale) {
                sws_freeContext(impl_->baseScale);
                impl_->baseScale = nullptr;
            }
            impl_->baseScale =
                sws_getContext(base.width, base.height, AV_PIX_FMT_BGRA, canvasWidth,
                               canvasHeight, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                               nullptr, nullptr);
            if (!impl_->baseScale) {
                impl_->lastError = "创建主画面缩放上下文失败";
                return nullptr;
            }
            impl_->baseSrcWidth = base.width;
            impl_->baseSrcHeight = base.height;
        }

        out->bgra.resize(static_cast<size_t>(canvasWidth) * canvasHeight * 4);

        const uint8_t* srcData[4] = {base.bgra.data(), nullptr, nullptr, nullptr};
        const int srcLinesize[4] = {base.width * 4, 0, 0, 0};
        uint8_t* dstData[4] = {out->bgra.data(), nullptr, nullptr, nullptr};
        const int dstLinesize[4] = {canvasWidth * 4, 0, 0, 0};

        sws_scale(impl_->baseScale, srcData, srcLinesize, 0, base.height, dstData,
                  dstLinesize);
    }

    if (impl_->config.pipEnabled && overlay && overlay->width > 0 && overlay->height > 0
        && overlay->bgra.size() >= static_cast<size_t>(overlay->width) * overlay->height * 4) {
        if (impl_->updatePipRect(overlay->width, overlay->height)) {
            std::vector<uint8_t> pipBuffer(
                static_cast<size_t>(impl_->pipW) * impl_->pipH * 4);

            const uint8_t* srcData[4] = {overlay->bgra.data(), nullptr, nullptr, nullptr};
            const int srcLinesize[4] = {overlay->width * 4, 0, 0, 0};
            uint8_t* dstData[4] = {pipBuffer.data(), nullptr, nullptr, nullptr};
            const int dstLinesize[4] = {impl_->pipW * 4, 0, 0, 0};

            sws_scale(impl_->pipScale, srcData, srcLinesize, 0, overlay->height, dstData,
                      dstLinesize);

            for (int y = 0; y < impl_->pipH; ++y) {
                const int canvasY = impl_->pipY + y;
                if (canvasY < 0 || canvasY >= canvasHeight)
                    continue;
                blitRow(out->bgra.data()
                            + (static_cast<size_t>(canvasY) * canvasWidth + impl_->pipX) * 4,
                        pipBuffer.data() + static_cast<size_t>(y) * impl_->pipW * 4,
                        impl_->pipW * 4);
            }
        }
    }

    if (!impl_->mask.empty()) {
        const int x = std::max(8, canvasWidth / 64);
        const int y = std::max(8, canvasHeight / 64);

        impl_->blendMask(out->bgra.data(), canvasWidth, canvasHeight, x + 2, y + 2, 0, 0,
                         0, 0.55f);
        impl_->blendMask(out->bgra.data(), canvasWidth, canvasHeight, x, y, 255, 255, 255,
                         1.0f);
    }

    return out;
}

int VideoCompositor::pipX() const
{
    return impl_->pipX;
}

int VideoCompositor::pipY() const
{
    return impl_->pipY;
}

int VideoCompositor::pipWidth() const
{
    return impl_->pipW;
}

int VideoCompositor::pipHeight() const
{
    return impl_->pipH;
}

const std::string& VideoCompositor::lastError() const
{
    return impl_->lastError;
}
