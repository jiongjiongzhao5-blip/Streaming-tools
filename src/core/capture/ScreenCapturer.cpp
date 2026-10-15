#include "core/capture/ScreenCapturer.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>

namespace {

template <typename T>
void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

VideoFramePtr gdiCapture()
{
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0)
        return nullptr;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldBitmap = SelectObject(memDc, bmp);

    BitBlt(memDc, 0, 0, w, h, screenDc, 0, 0, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    auto frame = std::make_shared<VideoFrame>();
    frame->width = w;
    frame->height = h;
    frame->bgra.resize(static_cast<size_t>(w) * h * 4);

    GetDIBits(memDc, bmp, 0, h, frame->bgra.data(), &bmi, DIB_RGB_COLORS);

    SelectObject(memDc, oldBitmap);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return frame;
}

}

struct ScreenCapturer::Impl {
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* staging = nullptr;

    int width = 0;
    int height = 0;

    VideoFramePtr lastFrame;

    LogCallback log;
};

ScreenCapturer::ScreenCapturer()
    : impl_(std::make_unique<Impl>())
{
}

ScreenCapturer::~ScreenCapturer()
{
    close();
}

bool ScreenCapturer::init(LogCallback log)
{
    close();
    impl_->log = std::move(log);

    const auto fallbackToGdi = [this](const std::string& reason) {
        if (impl_->log)
            impl_->log(reason);
        close();
        return true;
    };

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&impl_->factory))))
        return fallbackToGdi("DXGI 初始化失败，使用 GDI 采集");

    if (FAILED(impl_->factory->EnumAdapters1(0, &impl_->adapter))
        || FAILED(impl_->adapter->EnumOutputs(0, &impl_->output)))
        return fallbackToGdi("枚举显示器失败，使用 GDI 采集");

    DXGI_OUTPUT_DESC outputDesc = {};
    impl_->output->GetDesc(&outputDesc);
    impl_->width = outputDesc.DesktopCoordinates.right
                   - outputDesc.DesktopCoordinates.left;
    impl_->height = outputDesc.DesktopCoordinates.bottom
                    - outputDesc.DesktopCoordinates.top;
    if (impl_->width <= 0 || impl_->height <= 0)
        return fallbackToGdi("显示器尺寸无效，使用 GDI 采集");

    if (FAILED(impl_->output->QueryInterface(IID_PPV_ARGS(&impl_->output1))))
        return fallbackToGdi("获取 IDXGIOutput1 失败，使用 GDI 采集");

    if (FAILED(D3D11CreateDevice(impl_->adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &impl_->device, nullptr, &impl_->context)))
        return fallbackToGdi("创建 D3D11 设备失败，使用 GDI 采集");

    if (FAILED(impl_->output1->DuplicateOutput(impl_->device, &impl_->duplication)))
        return fallbackToGdi(
            "Desktop Duplication 不可用（可能被其它录屏软件占用），使用 GDI 采集");

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = impl_->width;
    stagingDesc.Height = impl_->height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(impl_->device->CreateTexture2D(&stagingDesc, nullptr,
                                              &impl_->staging)))
        return fallbackToGdi("创建 staging 纹理失败，使用 GDI 采集");

    dxgiReady_ = true;
    if (impl_->log)
        impl_->log("DXGI 屏幕采集已启动: " + std::to_string(impl_->width) + "x"
                   + std::to_string(impl_->height));
    return true;
}

void ScreenCapturer::close()
{
    safeRelease(impl_->staging);
    safeRelease(impl_->duplication);
    safeRelease(impl_->context);
    safeRelease(impl_->device);
    safeRelease(impl_->output1);
    safeRelease(impl_->output);
    safeRelease(impl_->adapter);
    safeRelease(impl_->factory);

    impl_->width = 0;
    impl_->height = 0;
    impl_->lastFrame.reset();
    dxgiReady_ = false;
}

VideoFramePtr ScreenCapturer::capture()
{
    if (dxgiReady_) {
        if (VideoFramePtr frame = captureDxgi())
            return frame;

        dxgiReady_ = false;
        if (impl_->log)
            impl_->log("DXGI 采集异常，已切换 GDI 采集");
    }
    return captureGdi();
}

VideoFramePtr ScreenCapturer::captureDxgi()
{
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    const HRESULT acquireHr =
        impl_->duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);

    if (acquireHr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (impl_->lastFrame)
            return std::make_shared<VideoFrame>(*impl_->lastFrame);
        return nullptr;
    }

    if (acquireHr == DXGI_ERROR_ACCESS_LOST) {
        if (impl_->log)
            impl_->log("DXGI 桌面连接丢失");
        return nullptr;
    }

    if (FAILED(acquireHr)) {
        if (impl_->log)
            impl_->log("DXGI 取帧失败");
        return nullptr;
    }

    ID3D11Texture2D* desktopTexture = nullptr;
    if (FAILED(desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture)))) {
        desktopResource->Release();
        impl_->duplication->ReleaseFrame();
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    desktopTexture->GetDesc(&texDesc);

    if (texDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        desktopTexture->Release();
        desktopResource->Release();
        impl_->duplication->ReleaseFrame();
        return nullptr;
    }

    impl_->context->CopyResource(impl_->staging, desktopTexture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(impl_->context->Map(impl_->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        desktopTexture->Release();
        desktopResource->Release();
        impl_->duplication->ReleaseFrame();
        return nullptr;
    }

    auto frame = std::make_shared<VideoFrame>();
    frame->width = static_cast<int>(texDesc.Width);
    frame->height = static_cast<int>(texDesc.Height);
    frame->bgra.resize(static_cast<size_t>(frame->width) * frame->height * 4);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = frame->bgra.data();
    const size_t rowBytes = static_cast<size_t>(frame->width) * 4;
    for (int y = 0; y < frame->height; ++y) {
        std::memcpy(dst, src, rowBytes);
        src += mapped.RowPitch;
        dst += rowBytes;
    }

    impl_->context->Unmap(impl_->staging, 0);

    desktopTexture->Release();
    desktopResource->Release();

    impl_->duplication->ReleaseFrame();

    impl_->lastFrame = frame;
    return frame;
}

VideoFramePtr ScreenCapturer::captureGdi()
{
    return gdiCapture();
}
