// ============================================================
//  VoiceChat client — WASAPI endpoint enumeration implementation
//  File: client/src/audio_devices.cpp
// ============================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>                       // instantiate the named GUIDs below
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName

#include "../include/audio_devices.h"

namespace vc::client {

static std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::vector<AudioDevice> enumerateDevices(bool capture)
{
    std::vector<AudioDevice> out;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
        return out;

    IMMDeviceCollection* col = nullptr;
    EDataFlow flow = capture ? eCapture : eRender;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;

            AudioDevice ad;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) { ad.id = id; CoTaskMemFree(id); }

            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    ad.name = wideToUtf8(pv.pwszVal);
                PropVariantClear(&pv);
                ps->Release();
            }
            if (ad.name.empty()) ad.name = "Unknown device";

            out.push_back(std::move(ad));
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

} // namespace vc::client
