#include "core/capture/AudioCapturer.h"

#include "core/common/BoundedQueue.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace {

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

    bool owned() const { return owned_; }

private:
    bool owned_ = false;
};

std::string friendlyNameOf(IMMDevice* device)
{
    IPropertyStore* store = nullptr;
    if (!device || FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return {};

    std::string name;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value))
        && value.vt == VT_LPWSTR && value.pwszVal) {
        name = toUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    store->Release();
    return name;
}

const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

enum class SampleFormat {
    Float32,
    Int16,
    Unsupported,
};

SampleFormat detectSampleFormat(const WAVEFORMATEX* format)
{
    if (!format)
        return SampleFormat::Unsupported;

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return format->wBitsPerSample == 32 ? SampleFormat::Float32
                                            : SampleFormat::Unsupported;

    if (format->wFormatTag == WAVE_FORMAT_PCM)
        return format->wBitsPerSample == 16 ? SampleFormat::Int16
                                            : SampleFormat::Unsupported;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (ext->SubFormat == kSubtypeIeeeFloat && format->wBitsPerSample == 32)
            return SampleFormat::Float32;
        if (ext->SubFormat == kSubtypePcm && format->wBitsPerSample == 16)
            return SampleFormat::Int16;
    }

    return SampleFormat::Unsupported;
}

void convertSamples(const BYTE* src, size_t sampleCount, SampleFormat format,
                    std::vector<float>& out)
{
    out.resize(sampleCount);

    if (format == SampleFormat::Float32) {
        std::memcpy(out.data(), src, sampleCount * sizeof(float));
        return;
    }

    if (format == SampleFormat::Int16) {
        const auto* p = reinterpret_cast<const int16_t*>(src);
        constexpr float kScale = 1.0f / 32768.0f;
        for (size_t i = 0; i < sampleCount; ++i)
            out[i] = static_cast<float>(p[i]) * kScale;
    }
}

constexpr DWORD kPollIntervalMs = 10;

constexpr REFERENCE_TIME kBufferDuration = 1000000;

constexpr int kMinSilenceMs = 10;

constexpr int kMaxSilenceMs = 200;

}

struct AudioCapturer::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;

    HANDLE event = nullptr;
    std::thread worker;

    std::atomic<bool> stopping{false};

    int sampleRate = 0;
    int channels = 0;
    SampleFormat sampleFormat = SampleFormat::Unsupported;
    bool loopback = false;

    int64_t totalSamples = 0;

    BoundedQueue<AudioChunkPtr> queue{64};

    bool comOwned = false;
    LogCallback log;

    void workerLoop();
    void pushChunk(const float* data, size_t sampleCount, bool silent);
    void padSilence(int64_t frames);
};

void AudioCapturer::Impl::pushChunk(const float* data, size_t sampleCount, bool silent)
{
    auto chunk = std::make_shared<AudioChunk>();
    chunk->sampleRate = sampleRate;
    chunk->channels = channels;
    chunk->pts = totalSamples;

    if (silent || !data)
        chunk->samples.assign(sampleCount, 0.0f);
    else
        chunk->samples.assign(data, data + sampleCount);

    totalSamples += static_cast<int64_t>(sampleCount / (channels > 0 ? channels : 1));
    queue.push(std::move(chunk));
}

void AudioCapturer::Impl::padSilence(int64_t frames)
{
    if (frames <= 0 || sampleRate <= 0 || channels <= 0)
        return;

    auto chunk = std::make_shared<AudioChunk>();
    chunk->sampleRate = sampleRate;
    chunk->channels = channels;
    chunk->pts = totalSamples;
    chunk->samples.assign(static_cast<size_t>(frames) * channels, 0.0f);

    totalSamples += frames;
    queue.push(std::move(chunk));
}

void AudioCapturer::Impl::workerLoop()
{
    ComScope com;

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER startCounter = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&startCounter);

    while (!stopping) {
        WaitForSingleObject(event, kPollIntervalMs);

        if (stopping)
            break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetFrames))
               && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            const HRESULT hr =
                captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr) || frames == 0) {
                if (SUCCEEDED(hr))
                    captureClient->ReleaseBuffer(frames);
                break;
            }

            const size_t sampleCount =
                static_cast<size_t>(frames) * static_cast<size_t>(channels);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pushChunk(nullptr, sampleCount, true);
            } else {
                std::vector<float> converted;
                convertSamples(data, sampleCount, sampleFormat, converted);
                pushChunk(converted.data(), sampleCount, false);
            }

            captureClient->ReleaseBuffer(frames);
        }

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        const double elapsedSeconds =
            static_cast<double>(now.QuadPart - startCounter.QuadPart)
            / static_cast<double>(frequency.QuadPart);
        const int64_t expected =
            static_cast<int64_t>(elapsedSeconds * static_cast<double>(sampleRate));

        int64_t deficit = expected - totalSamples;
        const int64_t minSilence = static_cast<int64_t>(sampleRate) * kMinSilenceMs / 1000;
        if (deficit >= minSilence) {
            const int64_t maxSilence =
                static_cast<int64_t>(sampleRate) * kMaxSilenceMs / 1000;
            deficit = std::min(deficit, maxSilence);
            padSilence(deficit);
        }
    }
}

AudioCapturer::AudioCapturer() : impl_(std::make_unique<Impl>())
{
}

AudioCapturer::~AudioCapturer()
{
    close();
}

std::vector<std::string> AudioCapturer::listDevices(AudioSourceType type)
{
    std::vector<std::string> names;

    ComScope com;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return names;

    const EDataFlow flow =
        (type == AudioSourceType::SystemAudio) ? eRender : eCapture;

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                                 &collection))
        && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device)) && device) {
                names.push_back(friendlyNameOf(device));
                device->Release();
            }
        }
        collection->Release();
    }

    enumerator->Release();
    return names;
}

bool AudioCapturer::open(AudioSourceType type, int deviceIndex, LogCallback log)
{
    close();

    impl_->log = std::move(log);

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
        return fail("CoInitializeEx 失败，无法使用 WASAPI");
    } else {
        impl_->comOwned = true;
    }

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&impl_->enumerator))))
        return fail("创建设备枚举器失败");

    const EDataFlow flow =
        (type == AudioSourceType::SystemAudio) ? eRender : eCapture;
    impl_->loopback = (type == AudioSourceType::SystemAudio);

    std::string deviceName;

    if (deviceIndex < 0) {
        if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(flow, eConsole,
                                                              &impl_->device)))
            return fail("找不到默认音频设备");
    } else {
        IMMDeviceCollection* collection = nullptr;
        if (FAILED(impl_->enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                                         &collection))
            || !collection)
            return fail("枚举音频设备失败");

        UINT count = 0;
        collection->GetCount(&count);
        if (static_cast<UINT>(deviceIndex) >= count) {
            collection->Release();
            return fail("音频设备索引越界：一共只有 " + std::to_string(count) + " 个");
        }

        const HRESULT hr = collection->Item(static_cast<UINT>(deviceIndex),
                                            &impl_->device);
        collection->Release();
        if (FAILED(hr) || !impl_->device)
            return fail("打开指定音频设备失败");
    }

    deviceName = friendlyNameOf(impl_->device);

    if (FAILED(impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&impl_->audioClient))))
        return fail("激活 IAudioClient 失败");

    if (FAILED(impl_->audioClient->GetMixFormat(&impl_->mixFormat))
        || !impl_->mixFormat)
        return fail("读取系统混音格式失败");

    impl_->sampleRate = static_cast<int>(impl_->mixFormat->nSamplesPerSec);
    impl_->channels = static_cast<int>(impl_->mixFormat->nChannels);
    impl_->sampleFormat = detectSampleFormat(impl_->mixFormat);

    if (impl_->sampleFormat == SampleFormat::Unsupported)
        return fail("不支持的采样格式：只有 float32 和 16bit PCM 会被处理");

    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->event)
        return fail("创建音频事件失败");

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (impl_->loopback)
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    HRESULT hr = impl_->audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, streamFlags, kBufferDuration, 0, impl_->mixFormat,
        nullptr);

    if (FAILED(hr)) {
        if (impl_->loopback) {
            streamFlags &= ~AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            hr = impl_->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                                kBufferDuration, 0, impl_->mixFormat,
                                                nullptr);
            if (SUCCEEDED(hr))
                say("这台机器的 loopback 不支持事件回调，改用 10ms 轮询");
        }
        if (FAILED(hr))
            return fail("初始化音频客户端失败");
    }

    if (streamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
        if (FAILED(impl_->audioClient->SetEventHandle(impl_->event)))
            say("设置音频事件句柄失败，退回轮询模式");
    }

    if (FAILED(impl_->audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&impl_->captureClient))))
        return fail("获取 IAudioCaptureClient 失败");

    impl_->totalSamples = 0;
    impl_->stopping = false;

    if (FAILED(impl_->audioClient->Start()))
        return fail("启动音频采集失败");

    impl_->worker = std::thread([this] { impl_->workerLoop(); });

    say(std::string(impl_->loopback ? "系统声音" : "麦克风") + "已启动: "
        + (deviceName.empty() ? std::string("<未知设备>") : deviceName));
    say("采集格式: " + std::to_string(impl_->sampleRate) + " Hz, "
        + std::to_string(impl_->channels) + " 声道, "
        + (impl_->sampleFormat == SampleFormat::Float32 ? "float32" : "int16"));

    return true;
}

void AudioCapturer::close()
{
    impl_->stopping = true;
    if (impl_->event)
        SetEvent(impl_->event);

    if (impl_->worker.joinable())
        impl_->worker.join();

    if (impl_->audioClient)
        impl_->audioClient->Stop();

    impl_->queue.clear();

    safeRelease(impl_->captureClient);
    safeRelease(impl_->audioClient);
    safeRelease(impl_->device);
    safeRelease(impl_->enumerator);

    if (impl_->mixFormat) {
        CoTaskMemFree(impl_->mixFormat);
        impl_->mixFormat = nullptr;
    }
    if (impl_->event) {
        CloseHandle(impl_->event);
        impl_->event = nullptr;
    }

    impl_->sampleRate = 0;
    impl_->channels = 0;
    impl_->sampleFormat = SampleFormat::Unsupported;
    impl_->loopback = false;
    impl_->totalSamples = 0;

    if (impl_->comOwned) {
        CoUninitialize();
        impl_->comOwned = false;
    }
}

AudioChunkPtr AudioCapturer::nextChunk(int timeoutMs)
{
    return impl_->queue.tryPop(timeoutMs).value_or(nullptr);
}

int AudioCapturer::sampleRate() const
{
    return impl_->sampleRate;
}

int AudioCapturer::channels() const
{
    return impl_->channels;
}
