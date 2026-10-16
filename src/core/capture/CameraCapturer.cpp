#include "core/capture/CameraCapturer.h"

#include <windows.h>

#include <dshow.h>
#include <dvdmedia.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace {

struct __declspec(uuid("0579154a-2b53-4994-b0d0-e773148eff85"))
    ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime,
                                               IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer,
                                               long bufferLen) = 0;
};

struct __declspec(uuid("6b652fff-11fe-4fce-92ad-0266b5d7c78f"))
    ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* bufferSize,
                                                       long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback,
                                                  long whichMethodToCallback) = 0;
};

const GUID kClsidSampleGrabber = {
    0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

const GUID kClsidNullRenderer = {
    0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

template <typename T>
void safeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string toUtf8(const wchar_t* wide)
{
    if (!wide || !*wide)
        return {};

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1);
    return out;
}

std::string guidToString(const GUID& guid)
{
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, 64);
    return toUtf8(buffer);
}

void freeMediaType(AM_MEDIA_TYPE& type)
{
    if (type.cbFormat != 0) {
        CoTaskMemFree(type.pbFormat);
        type.cbFormat = 0;
        type.pbFormat = nullptr;
    }
    if (type.pUnk) {
        type.pUnk->Release();
        type.pUnk = nullptr;
    }
}

int dibRowBytes(int width, int bitCount)
{
    return ((width * bitCount + 31) / 32) * 4;
}

CameraPixelFormat formatFromSubtype(const GUID& subtype)
{
    if (subtype == MEDIASUBTYPE_RGB24)
        return CameraPixelFormat::RGB24;
    if (subtype == MEDIASUBTYPE_RGB32 || subtype == MEDIASUBTYPE_ARGB32)
        return CameraPixelFormat::RGB32;
    if (subtype == MEDIASUBTYPE_YUY2)
        return CameraPixelFormat::YUY2;
    if (subtype == MEDIASUBTYPE_UYVY)
        return CameraPixelFormat::UYVY;
    if (subtype == MEDIASUBTYPE_MJPG)
        return CameraPixelFormat::MJPG;
    return CameraPixelFormat::Unknown;
}

const GUID* subtypeOf(CameraPixelFormat format)
{
    switch (format) {
    case CameraPixelFormat::RGB24:
        return &MEDIASUBTYPE_RGB24;
    case CameraPixelFormat::RGB32:
        return &MEDIASUBTYPE_RGB32;
    case CameraPixelFormat::YUY2:
        return &MEDIASUBTYPE_YUY2;
    case CameraPixelFormat::UYVY:
        return &MEDIASUBTYPE_UYVY;
    case CameraPixelFormat::MJPG:
        return &MEDIASUBTYPE_MJPG;
    case CameraPixelFormat::Unknown:
        break;
    }
    return nullptr;
}

std::string friendlyNameOf(IMoniker* moniker)
{
    IPropertyBag* bag = nullptr;
    if (!moniker
        || FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
        return {};

    std::string name;
    VARIANT value;
    VariantInit(&value);
    if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr))
        && value.vt == VT_BSTR)
        name = toUtf8(value.bstrVal);
    VariantClear(&value);
    bag->Release();
    return name;
}

class ComScope {
public:
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = (hr == S_OK || hr == S_FALSE);
    }

    ~ComScope()
    {
        if (owned_)
            CoUninitialize();
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owned_ = false;
};

class SampleGrabberCallback : public ISampleGrabberCB {
public:
    using Handler = std::function<void(const uint8_t* data, size_t size)>;

    explicit SampleGrabberCallback(Handler handler) : handler_(std::move(handler)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return --refCount_;
    }

    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* buffer, long bufferLen) override
    {
        if (handler_ && buffer && bufferLen > 0)
            handler_(buffer, static_cast<size_t>(bufferLen));
        return S_OK;
    }

private:
    Handler handler_;
    std::atomic<ULONG> refCount_{0};
};

}

struct CameraCapturer::Impl {
    Impl()
        : callback([this](const uint8_t* data, size_t size) { onFrame(data, size); })
    {
    }

    IGraphBuilder* graph = nullptr;
    ICaptureGraphBuilder2* graphBuilder = nullptr;
    IMediaControl* control = nullptr;
    IBaseFilter* sourceFilter = nullptr;
    IBaseFilter* grabberFilter = nullptr;
    IBaseFilter* nullRenderer = nullptr;
    ISampleGrabber* grabber = nullptr;

    AM_MEDIA_TYPE connectedType = {};

    SampleGrabberCallback callback;

    bool comOwned = false;

    int width = 0;
    int height = 0;
    int stride = 0;
    CameraPixelFormat format = CameraPixelFormat::Unknown;
    bool flipVertical = false;

    std::mutex mutex;
    std::condition_variable cv;
    CameraRawFramePtr pending;

    std::atomic<bool> stopping{false};
    std::atomic<int64_t> frameCounter{0};

    LogCallback log;

    void onFrame(const uint8_t* data, size_t size)
    {
        if (stopping || !data || size == 0)
            return;

        auto frame = std::make_shared<CameraRawFrame>();
        frame->width = width;
        frame->height = height;
        frame->stride = stride;
        frame->format = format;
        frame->flipVertical = flipVertical;
        frame->pts = frameCounter.fetch_add(1);
        frame->bytes.assign(data, data + size);

        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = std::move(frame);
        }
        cv.notify_one();
    }
};

const char* cameraPixelFormatName(CameraPixelFormat format)
{
    switch (format) {
    case CameraPixelFormat::RGB24:
        return "RGB24";
    case CameraPixelFormat::RGB32:
        return "RGB32";
    case CameraPixelFormat::YUY2:
        return "YUY2";
    case CameraPixelFormat::UYVY:
        return "UYVY";
    case CameraPixelFormat::MJPG:
        return "MJPG";
    case CameraPixelFormat::Unknown:
        break;
    }
    return "Unknown";
}

CameraCapturer::CameraCapturer() : impl_(std::make_unique<Impl>())
{
}

CameraCapturer::~CameraCapturer()
{
    close();
}

std::vector<std::string> CameraCapturer::listDevices()
{
    std::vector<std::string> names;

    ComScope com;

    ICreateDevEnum* deviceEnumerator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&deviceEnumerator))))
        return names;

    IEnumMoniker* enumMoniker = nullptr;
    if (deviceEnumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                                &enumMoniker, 0) == S_OK
        && enumMoniker) {
        IMoniker* moniker = nullptr;
        while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
            names.push_back(friendlyNameOf(moniker));
            moniker->Release();
        }
        enumMoniker->Release();
    }

    deviceEnumerator->Release();
    return names;
}

bool CameraCapturer::open(int deviceIndex, LogCallback log,
                          CameraPixelFormat requestFormat)
{
    close();

    impl_->log = std::move(log);
    impl_->stopping = false;
    impl_->frameCounter = 0;

    const auto say = [this](const std::string& message) {
        if (impl_->log)
            impl_->log(message);
    };
    const auto fail = [this, &say](const std::string& reason) {
        say(reason);
        close();
        return false;
    };

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (comHr == RPC_E_CHANGED_MODE) {
        say("当前线程已用单线程套间(STA)初始化过 COM，直接复用");
    } else if (FAILED(comHr)) {
        return fail("CoInitializeEx 失败，无法使用 DirectShow");
    } else {
        impl_->comOwned = true;
    }

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&impl_->graph))))
        return fail("创建 FilterGraph 失败");

    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&impl_->graphBuilder))))
        return fail("创建 CaptureGraphBuilder2 失败");

    impl_->graphBuilder->SetFiltergraph(impl_->graph);

    if (FAILED(impl_->graph->QueryInterface(IID_PPV_ARGS(&impl_->control))))
        return fail("获取 IMediaControl 失败");

    ICreateDevEnum* deviceEnumerator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&deviceEnumerator))))
        return fail("创建视频设备枚举器失败");

    IEnumMoniker* enumMoniker = nullptr;
    if (deviceEnumerator->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                                &enumMoniker, 0) != S_OK
        || !enumMoniker) {
        deviceEnumerator->Release();
        return fail("系统里没有找到任何视频输入设备（摄像头）");
    }

    std::string deviceName;
    int seen = 0;
    IMoniker* moniker = nullptr;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        if (seen == deviceIndex) {
            deviceName = friendlyNameOf(moniker);
            moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&impl_->sourceFilter));
            moniker->Release();
            break;
        }
        moniker->Release();
        ++seen;
    }
    enumMoniker->Release();
    deviceEnumerator->Release();

    if (!impl_->sourceFilter) {
        return fail("打开第 " + std::to_string(deviceIndex)
                    + " 个摄像头失败（索引越界，或被别的程序独占）");
    }

    say("摄像头 #" + std::to_string(deviceIndex) + ": "
        + (deviceName.empty() ? std::string("<未知设备名>") : deviceName));

    if (FAILED(impl_->graph->AddFilter(impl_->sourceFilter, L"Camera Source")))
        return fail("把摄像头加进过滤器图失败");

    if (FAILED(CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&impl_->grabberFilter))))
        return fail("创建 Sample Grabber 失败");

    if (FAILED(impl_->grabberFilter->QueryInterface(IID_PPV_ARGS(&impl_->grabber))))
        return fail("从 Sample Grabber 上取 ISampleGrabber 接口失败");

    if (FAILED(CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&impl_->nullRenderer))))
        return fail("创建 Null Renderer 失败");

    if (FAILED(impl_->graph->AddFilter(impl_->grabberFilter, L"Sample Grabber"))
        || FAILED(impl_->graph->AddFilter(impl_->nullRenderer, L"Null Renderer")))
        return fail("把 Sample Grabber / Null Renderer 加进过滤器图失败");

    const GUID* wantedSubtype = subtypeOf(requestFormat);
    if (wantedSubtype) {
        AM_MEDIA_TYPE request = {};
        request.majortype = MEDIATYPE_Video;
        request.subtype = *wantedSubtype;
        request.formattype = FORMAT_VideoInfo;
        impl_->grabber->SetMediaType(&request);
        say(std::string("向摄像头请求 ") + cameraPixelFormatName(requestFormat) + " ...");
    } else {
        say("不指定格式，直接接受摄像头给的原生格式");
    }

    HRESULT connectHr = impl_->graphBuilder->RenderStream(
        &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, impl_->sourceFilter,
        impl_->grabberFilter, impl_->nullRenderer);

    if (FAILED(connectHr)) {
        connectHr = impl_->graphBuilder->RenderStream(
            &PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, impl_->sourceFilter,
            impl_->grabberFilter, impl_->nullRenderer);
    }

    if (FAILED(connectHr)) {
        say(std::string(cameraPixelFormatName(requestFormat))
            + " 请求没被接受，改为接受摄像头能给的原生格式");
        impl_->grabber->SetMediaType(nullptr);

        connectHr = impl_->graphBuilder->RenderStream(
            &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, impl_->sourceFilter,
            impl_->grabberFilter, impl_->nullRenderer);
        if (FAILED(connectHr)) {
            connectHr = impl_->graphBuilder->RenderStream(
                &PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, impl_->sourceFilter,
                impl_->grabberFilter, impl_->nullRenderer);
        }
    }

    if (FAILED(connectHr))
        return fail("摄像头与采集器连线失败（格式协商不成功）");

    AM_MEDIA_TYPE connected = {};
    if (FAILED(impl_->grabber->GetConnectedMediaType(&connected)))
        return fail("读取实际协商格式失败");

    impl_->connectedType = connected;

    int bitCount = 0;
    LONG signedHeight = 0;
    if (connected.formattype == FORMAT_VideoInfo
        && connected.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat);
        impl_->width = static_cast<int>(vih->bmiHeader.biWidth);
        signedHeight = vih->bmiHeader.biHeight;
        bitCount = vih->bmiHeader.biBitCount;
    } else if (connected.formattype == FORMAT_VideoInfo2
               && connected.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(connected.pbFormat);
        impl_->width = static_cast<int>(vih2->bmiHeader.biWidth);
        signedHeight = vih2->bmiHeader.biHeight;
        bitCount = vih2->bmiHeader.biBitCount;
    } else {
        return fail("拿到一个不认识的媒体类型格式: formattype = "
                    + guidToString(connected.formattype));
    }

    impl_->height = static_cast<int>(signedHeight < 0 ? -signedHeight : signedHeight);
    impl_->flipVertical = signedHeight > 0;
    impl_->format = formatFromSubtype(connected.subtype);
    impl_->stride = (impl_->format == CameraPixelFormat::MJPG || bitCount <= 0)
                        ? 0
                        : dibRowBytes(impl_->width, bitCount);

    say("实际协商结果: " + std::to_string(impl_->width) + "x"
        + std::to_string(impl_->height) + "  " + cameraPixelFormatName(impl_->format)
        + "  " + std::to_string(bitCount) + " 位/像素  行跨度 "
        + std::to_string(impl_->stride) + " 字节"
        + (impl_->flipVertical ? "  自底向上(需要翻转)" : "  自顶向下"));

    if (impl_->format == CameraPixelFormat::MJPG) {
        say("注意: MJPG 每帧是一整张 JPEG，stride 对它没有意义，"
            "下一轮转换器里需要解码");
    } else if (impl_->format == CameraPixelFormat::Unknown) {
        say("警告: 这个子类型本模块还不认识 -> " + guidToString(connected.subtype)
            + "，下一轮的转换器需要补上对它的支持");
    }

    impl_->grabber->SetOneShot(FALSE);
    impl_->grabber->SetBufferSamples(FALSE);
    impl_->grabber->SetCallback(&impl_->callback, 1);

    if (FAILED(impl_->control->Run()))
        return fail("启动过滤器图失败（摄像头可能被其它程序占用）");

    return true;
}

void CameraCapturer::close()
{
    impl_->stopping = true;

    if (impl_->control)
        impl_->control->Stop();

    if (impl_->grabber)
        impl_->grabber->SetCallback(nullptr, 0);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pending.reset();
    }

    freeMediaType(impl_->connectedType);

    safeRelease(impl_->grabber);
    safeRelease(impl_->nullRenderer);
    safeRelease(impl_->grabberFilter);
    safeRelease(impl_->sourceFilter);
    safeRelease(impl_->control);
    safeRelease(impl_->graphBuilder);
    safeRelease(impl_->graph);

    impl_->width = 0;
    impl_->height = 0;
    impl_->stride = 0;
    impl_->format = CameraPixelFormat::Unknown;
    impl_->flipVertical = false;
    impl_->frameCounter = 0;

    if (impl_->comOwned) {
        CoUninitialize();
        impl_->comOwned = false;
    }
}

CameraRawFramePtr CameraCapturer::nextFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(impl_->mutex);

    const bool arrived = impl_->cv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return static_cast<bool>(impl_->pending); });

    if (!arrived)
        return nullptr;

    CameraRawFramePtr frame = std::move(impl_->pending);
    impl_->pending.reset();
    return frame;
}

int CameraCapturer::width() const
{
    return impl_->width;
}

int CameraCapturer::height() const
{
    return impl_->height;
}

CameraPixelFormat CameraCapturer::pixelFormat() const
{
    return impl_->format;
}
