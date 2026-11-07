#pragma once
// Follows the Windows default render endpoint volume (dB + mute) and publishes it as a linear gain.
// All COM work lives on one private MTA thread; the audio thread only reads atomics.

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <utility>
#include <atomic>
#include <thread>
#include <string>
#include <cmath>
#include <algorithm>

class WindowsVolumeFollower
{
public:
    WindowsVolumeFollower()
    {
        InitializeCriticalSection(&nameLock);
        changeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopEvent   = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
        worker = std::thread([this] { run(); });
    }

    ~WindowsVolumeFollower()
    {
        SetEvent(stopEvent);
        if (worker.joinable()) worker.join();
        CloseHandle(changeEvent);
        CloseHandle(stopEvent);
        DeleteCriticalSection(&nameLock);
    }

    // Linear gain 0..1 (1 = Windows at 0 dB, 0 = muted / unavailable-as-muted).
    float getGain() const noexcept      { return gain.load(std::memory_order_relaxed); }
    float getDb() const noexcept        { return db.load(std::memory_order_relaxed); }
    bool  isMuted() const noexcept      { return muted.load(std::memory_order_relaxed); }
    bool  isConnected() const noexcept  { return connected.load(std::memory_order_relaxed); }

    // Empty id = follow the Windows default output. Otherwise follow that specific endpoint.
    void setTargetDevice (const std::wstring& id)
    {
        EnterCriticalSection(&nameLock);
        const bool changed = (targetId != id);
        targetId = id;
        LeaveCriticalSection(&nameLock);
        if (changed) { rebind.store(true); SetEvent(changeEvent); }
    }

    // Active render endpoints as (id, friendly name). Safe to call from any thread.
    static std::vector<std::pair<std::wstring, std::wstring>> listRenderDevices()
    {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))
            && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
        {
            UINT count = 0; coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                LPWSTR id = nullptr;
                if (SUCCEEDED(dev->GetId(&id)) && id != nullptr)
                {
                    std::wstring name = id;
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
                    {
                        PROPVARIANT v; PropVariantInit(&v);
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                            name = v.pwszVal;
                        PropVariantClear(&v);
                        props->Release();
                    }
                    out.emplace_back(id, name);
                    CoTaskMemFree(id);
                }
                dev->Release();
            }
        }
        if (coll != nullptr) coll->Release();
        if (en != nullptr) en->Release();
        if (SUCCEEDED(initHr)) CoUninitialize();   // RPC_E_CHANGED_MODE: host already owns COM, leave it
        return out;
    }

    std::wstring getDeviceName() const
    {
        EnterCriticalSection(&nameLock);
        auto copy = deviceName;
        LeaveCriticalSection(&nameLock);
        return copy;
    }

private:
    // ---- COM callbacks -----------------------------------------------------------
    struct VolumeCallback : IAudioEndpointVolumeCallback
    {
        explicit VolumeCallback(WindowsVolumeFollower& o) : owner(o) {}
        ULONG STDMETHODCALLTYPE AddRef() override  { return 1; }   // lifetime owned by the follower
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback))
            { *ppv = static_cast<IAudioEndpointVolumeCallback*>(this); return S_OK; }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override
        { SetEvent(owner.changeEvent); return S_OK; }
        WindowsVolumeFollower& owner;
    };

    struct DeviceNotifier : IMMNotificationClient
    {
        explicit DeviceNotifier(WindowsVolumeFollower& o) : owner(o) {}
        ULONG STDMETHODCALLTYPE AddRef() override  { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
            { *ppv = static_cast<IMMNotificationClient*>(this); return S_OK; }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
        {
            if (flow == eRender && role == eConsole) { owner.rebind.store(true); SetEvent(owner.changeEvent); }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
        { owner.rebind.store(true); SetEvent(owner.changeEvent); return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override   { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
        WindowsVolumeFollower& owner;
    };

    // ---- worker thread -----------------------------------------------------------
    void run()
    {
        const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioEndpointVolume* endpoint = nullptr;
        VolumeCallback volumeCb(*this);
        DeviceNotifier notifier(*this);

        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))))
            enumerator->RegisterEndpointNotificationCallback(&notifier);

        auto unbind = [&]
        {
            if (endpoint != nullptr) { endpoint->UnregisterControlChangeNotify(&volumeCb); endpoint->Release(); endpoint = nullptr; }
            if (device != nullptr)   { device->Release(); device = nullptr; }
            connected.store(false);
        };

        auto bind = [&]
        {
            unbind();
            if (enumerator == nullptr) return;
            EnterCriticalSection(&nameLock); const std::wstring wanted = targetId; LeaveCriticalSection(&nameLock);
            const HRESULT hr = wanted.empty()
                ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
                : enumerator->GetDevice(wanted.c_str(), &device);
            if (FAILED(hr)) { device = nullptr; return; }   // missing device: retried by the 2 s poll
            if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&endpoint)))) { unbind(); return; }
            endpoint->RegisterControlChangeNotify(&volumeCb);

            IPropertyStore* props = nullptr;
            std::wstring name = L"Default output";
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
            {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    name = v.pwszVal;
                PropVariantClear(&v);
                props->Release();
            }
            EnterCriticalSection(&nameLock); deviceName = name; LeaveCriticalSection(&nameLock);
            connected.store(true);
        };

        auto readVolume = [&]
        {
            if (endpoint == nullptr) { gain.store(1.0f); db.store(0.0f); muted.store(false); return; }
            float levelDb = 0.0f; BOOL isMuted = FALSE;
            if (FAILED(endpoint->GetMasterVolumeLevel(&levelDb))) levelDb = 0.0f;
            endpoint->GetMute(&isMuted);
            levelDb = std::clamp(levelDb, -96.0f, 0.0f);
            db.store(levelDb);
            muted.store(isMuted != FALSE);
            gain.store(isMuted ? 0.0f : std::pow(10.0f, levelDb / 20.0f));
        };

        bind();
        readVolume();

        HANDLE handles[2] = { stopEvent, changeEvent };
        for (;;)
        {
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, 2000);   // 2 s safety poll
            if (r == WAIT_OBJECT_0) break;
            if (rebind.exchange(false) || endpoint == nullptr) bind();
            readVolume();
        }

        unbind();
        if (enumerator != nullptr) { enumerator->UnregisterEndpointNotificationCallback(&notifier); enumerator->Release(); }
        if (comOk) CoUninitialize();
    }

    std::atomic<float> gain { 1.0f }, db { 0.0f };
    std::atomic<bool>  muted { false }, connected { false }, rebind { false };
    mutable CRITICAL_SECTION nameLock {};
    std::wstring deviceName, targetId;
    HANDLE changeEvent = nullptr, stopEvent = nullptr;
    std::thread worker;
};
