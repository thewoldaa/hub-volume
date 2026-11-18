// Copyright (C) 2026 CraftKal
// SPDX-License-Identifier: GPL-3.0-only
//
// Hub Volume - pengunci volume perangkat, mikrofon, dan aplikasi untuk Windows 10/11.
// Win32 / C++17 / WASAPI / GDI+. Satu executable, tanpa dependensi runtime.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <algorithm>
using std::max;
using std::min;

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <propsys.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib")
#endif

namespace G = Gdiplus;

namespace {

constexpr wchar_t kMainClass[]   = L"HubVolume.Main";
constexpr wchar_t kFlyoutClass[] = L"HubVolume.Flyout";
constexpr wchar_t kMutexName[]   = L"Local\\HubVolume.SingleInstance.6f1d2c9a";

constexpr UINT WM_APP_TRAY     = WM_APP + 1;
constexpr UINT WM_APP_VOLUME   = WM_APP + 2;
constexpr UINT WM_APP_SESSIONS = WM_APP + 3;
constexpr UINT WM_APP_DEVICES  = WM_APP + 4;
constexpr UINT WM_APP_SHOW     = WM_APP + 5;
constexpr UINT WM_APP_HIDE     = WM_APP + 6;
constexpr UINT WM_APP_QUIT     = WM_APP + 7;

constexpr UINT_PTR kTimerEnforce     = 1;
constexpr UINT_PTR kTimerDevices     = 2;
constexpr UINT_PTR kTimerHealth      = 3;
constexpr UINT_PTR kTimerEngineRetry = 4;
constexpr UINT_PTR kTimerTray        = 5;

// Jeda minimum antar koreksi untuk target yang sama, supaya aplikasi lain yang
// terus melawan kunci tidak membuat proses ini berputar tanpa batas.
constexpr ULONGLONG kMinEnforceGapMs = 12;
constexpr UINT kDeviceDebounceMs  = 400;
constexpr UINT kHealthIntervalMs  = 30000;
constexpr UINT kEngineRetryMs     = 5000;
constexpr float kLevelTolerance   = 0.004f;

// Penanda perubahan volume yang berasal dari aplikasi ini sendiri.
const GUID kOurContext = {0x7c1f3a52, 0x2d4e, 0x4b8a,
                          {0x9e, 0x61, 0x5a, 0x0c, 0x83, 0xd7, 0x42, 0xb9}};

namespace util {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring TakeCoString(LPWSTR p) {
    std::wstring r = p ? p : L"";
    if (p) CoTaskMemFree(p);
    return r;
}

template <class F>
F Proc(const wchar_t* module, const char* name) {
    HMODULE m = GetModuleHandleW(module);
    if (!m) return nullptr;
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

std::wstring ExePath() {
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(nullptr, buf, 1024);
    if (n == 0 || n >= 1024) return {};
    return std::wstring(buf, n);
}

}

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com o) noexcept { std::swap(p_, o.p_); return *this; }
    ~Com() { Reset(); }

    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void Attach(T* p) { Reset(); p_ = p; }

    T** Put() { Reset(); return &p_; }
    void** PutVoid() { Reset(); return reinterpret_cast<void**>(&p_); }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

    template <class U>
    HRESULT As(Com<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(__uuidof(U), out.PutVoid());
    }

private:
    T* p_ = nullptr;
};

struct Volume {
    float level = 0.f;
    bool  muted = false;
};

bool SameVolume(const Volume& a, const Volume& b) {
    return std::fabs(a.level - b.level) < kLevelTolerance && a.muted == b.muted;
}

enum class Kind { Output, App, Input };

struct LockRec {
    Volume       target;
    unsigned     restores = 0;
    std::wstring title;
    bool         input = false;
};

struct Row {
    Kind         kind = Kind::Output;
    std::wstring key, title;
    HICON        icon = nullptr;
    Volume       now;
    bool         live = false;
    bool         locked = false;
    Volume       lockedTo;
    unsigned     restores = 0;
};

struct Config {
    std::map<std::wstring, LockRec> devLocks;
    std::map<std::wstring, LockRec> appLocks;
    bool trayHintShown = false;

    static std::wstring Dir() {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::wstring base;
        if (n > 0 && n < MAX_PATH) {
            base.assign(buf, n);
        } else {
            base = util::ExePath();
            size_t slash = base.find_last_of(L'\\');
            base = (slash == std::wstring::npos) ? L"." : base.substr(0, slash);
        }
        return base + L"\\HubVolume";
    }
    static std::wstring Path() { return Dir() + L"\\config.txt"; }

    static std::vector<std::string> Split(const std::string& line) {
        std::vector<std::string> f;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        return f;
    }
    static float Permille(const std::string& s) {
        long v = std::strtol(s.c_str(), nullptr, 10);
        return static_cast<float>(std::clamp<long>(v, 0, 1000)) / 1000.f;
    }
    static std::string Clean(const std::wstring& w) {
        std::string s = util::Narrow(w);
        for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    }

    void Load() {
        HANDLE h = CreateFileW(Path().c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        std::string text;

        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (1 << 20)) {
            text.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(h);

        size_t pos = 0;
        bool first = true;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first) { first = false; if (line.rfind("HubVolume-config", 0) != 0) return; continue; }
            auto f = Split(line);
            if (f.empty()) continue;
            if (f[0] == "hint" && f.size() >= 2) {
                trayHintShown = (f[1] == "1");
            } else if (f[0] == "dev" && f.size() >= 6) {
                LockRec r;
                r.target.level = Permille(f[2]);
                r.target.muted = (f[3] == "1");
                r.input = (f[4] == "1");
                r.title = util::Widen(f[5]);
                if (!f[1].empty()) devLocks[util::Widen(f[1])] = r;
            } else if (f[0] == "app" && f.size() >= 5) {
                LockRec r;
                r.target.level = Permille(f[2]);
                r.target.muted = (f[3] == "1");
                r.title = util::Widen(f[4]);
                if (!f[1].empty()) appLocks[util::Widen(f[1])] = r;
            }
        }
    }

    // Ditulis ke berkas sementara lalu di-rename agar config tidak pernah
    // tertinggal dalam keadaan setengah jadi.
    bool Save() const {
        std::string out = "HubVolume-config 1\n";
        out += std::string("hint\t") + (trayHintShown ? "1" : "0") + "\n";
        for (const auto& kv : devLocks) {
            const LockRec& r = kv.second;
            out += "dev\t" + Clean(kv.first) + "\t" +
                   std::to_string(static_cast<int>(std::lround(r.target.level * 1000.f))) + "\t" +
                   (r.target.muted ? "1" : "0") + "\t" + (r.input ? "1" : "0") + "\t" + Clean(r.title) + "\n";
        }
        for (const auto& kv : appLocks) {
            const LockRec& r = kv.second;
            out += "app\t" + Clean(kv.first) + "\t" +
                   std::to_string(static_cast<int>(std::lround(r.target.level * 1000.f))) + "\t" +
                   (r.target.muted ? "1" : "0") + "\t" + Clean(r.title) + "\n";
        }

        CreateDirectoryW(Dir().c_str(), nullptr);
        const std::wstring path = Path();
        const std::wstring tmp = path + L".tmp";

        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
                  written == out.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) { DeleteFileW(tmp.c_str()); return false; }

        for (int attempt = 0; attempt < 3; ++attempt) {
            if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            Sleep(25);
        }
        DeleteFileW(tmp.c_str());
        return false;
    }
};

namespace Startup {

constexpr wchar_t kRunKey[]      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValue[]       = L"HubVolume";

std::wstring Command() { return L"\"" + util::ExePath() + L"\" --background"; }

bool ReadRun(std::wstring* out) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    if (out) *out = buf;
    return true;
}

// Task Manager > Startup menulis penolakan pengguna ke StartupApproved\Run;
// bit terendah byte pertama berarti entri dimatikan.
bool DisabledByUser() {
    BYTE buf[64];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValue, RRF_RT_REG_BINARY, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return size >= 1 && (buf[0] & 1) != 0;
}

bool Enabled() { return ReadRun(nullptr) && !DisabledByUser(); }

bool Set(bool on) {
    if (on) {
        const std::wstring cmd = Command();
        if (cmd.size() < 8) return false;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        LONG rc = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                                 static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);

        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValue);
        return rc == ERROR_SUCCESS;
    }
    LONG rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

void SelfHeal() {
    std::wstring current;
    if (ReadRun(&current) && current != Command()) Set(true);
}

}

// Callback COM tidak boleh memanggil balik ke API audio; semuanya hanya
// mem-post pesan dan dikerjakan di thread utama.
template <class Iface>
class CallbackBase : public Iface {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(Iface))) {
            *ppv = static_cast<Iface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

protected:
    virtual ~CallbackBase() = default;

private:
    std::atomic<ULONG> ref_{1};
};

class EndpointVolumeCallback final : public CallbackBase<IAudioEndpointVolumeCallback> {
public:
    EndpointVolumeCallback(HWND notify, uint32_t id) : notify_(notify), id_(id) {}
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (!data) return E_INVALIDARG;
        if (IsEqualGUID(data->guidEventContext, kOurContext)) return S_OK;
        PostMessageW(notify_, WM_APP_VOLUME, static_cast<WPARAM>(id_), 0);
        return S_OK;
    }

private:
    HWND notify_;
    uint32_t id_;
};

class SessionEvents final : public CallbackBase<IAudioSessionEvents> {
public:
    SessionEvents(HWND notify, uint32_t sessionId, uint32_t endpointId)
        : notify_(notify), sessionId_(sessionId), endpointId_(endpointId) {}

    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID ctx) override {
        if (ctx && IsEqualGUID(*ctx, kOurContext)) return S_OK;
        PostMessageW(notify_, WM_APP_VOLUME, static_cast<WPARAM>(sessionId_), 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState state) override {
        if (state == AudioSessionStateExpired)
            PostMessageW(notify_, WM_APP_SESSIONS, static_cast<WPARAM>(endpointId_), 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
        PostMessageW(notify_, WM_APP_SESSIONS, static_cast<WPARAM>(endpointId_), 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }

private:
    HWND notify_;
    uint32_t sessionId_, endpointId_;
};

class SessionNotifier final : public CallbackBase<IAudioSessionNotification> {
public:
    SessionNotifier(HWND notify, uint32_t endpointId) : notify_(notify), endpointId_(endpointId) {}
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        PostMessageW(notify_, WM_APP_SESSIONS, static_cast<WPARAM>(endpointId_), 0);
        return S_OK;
    }

private:
    HWND notify_;
    uint32_t endpointId_;
};

class DeviceNotifier final : public CallbackBase<IMMNotificationClient> {
public:
    explicit DeviceNotifier(HWND notify) : notify_(notify) {}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Poke(); }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT Poke() { PostMessageW(notify_, WM_APP_DEVICES, 0, 0); return S_OK; }
    HWND notify_;
};

class Engine {
public:
    explicit Engine(Config& cfg) : cfg_(cfg) {}
    ~Engine() {
        Teardown();
        for (auto& kv : identities_)
            if (kv.second.icon) DestroyIcon(kv.second.icon);
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::function<void()> onLocksChanged;
    std::function<void()> onStateChanged;

    void Start(HWND notify) { notify_ = notify; TryInit(); }
    void Stop() {
        KillTimer(notify_, kTimerEngineRetry);
        Teardown();
    }
    bool Ready() const { return static_cast<bool>(enumerator_); }
    size_t LockCount() const { return cfg_.devLocks.size() + cfg_.appLocks.size(); }

    void OnVolumeMessage(uint32_t id) {
        if (Endpoint* ep = FindEndpointById(id)) {
            if (cfg_.devLocks.count(ep->deviceId)) { ep->dirty = true; Pump(); }
            else if (observed_) { ReadEndpoint(*ep, &ep->cache); NotifyState(); }
            return;
        }
        if (AppSession* s = FindSessionById(id)) {
            if (cfg_.appLocks.count(s->key)) { s->dirty = true; Pump(); }
            else if (observed_) { ReadSession(*s, &s->cache); NotifyState(); }
        }
    }

    void OnSessionsMessage(uint32_t endpointId) {
        for (auto& ep : endpoints_) {
            if (ep->id != endpointId) continue;
            ResyncSessions(*ep);
            Pump();
            if (observed_) NotifyState();
            return;
        }
    }

    void ScheduleDeviceResync() { SetTimer(notify_, kTimerDevices, kDeviceDebounceMs, nullptr); }

    void OnTimer(UINT_PTR id) {
        switch (id) {
            case kTimerEnforce: Pump(); break;
            case kTimerDevices:
                KillTimer(notify_, kTimerDevices);
                if (Ready()) { ResyncDevices(); Pump(); if (observed_) NotifyState(); }
                break;
            case kTimerHealth: HealthCheck(); break;
            case kTimerEngineRetry: TryInit(); break;
        }
    }

    // Nilai yang tidak dikunci hanya dibaca saat jendela mixer terbuka.
    void SetObserved(bool on) {
        observed_ = on;
        if (!on || !Ready()) return;
        for (auto& ep : endpoints_) {
            ReadEndpoint(*ep, &ep->cache);
            if (ep->flow == eRender) ResyncSessions(*ep);
            for (auto& s : ep->sessions) ReadSession(*s, &s->cache);
        }
    }

    std::vector<Row> Snapshot() {
        std::vector<Row> out;
        std::set<std::wstring> liveDev;

        for (auto& ep : endpoints_) {
            Row r;
            r.kind = ep->flow == eRender ? Kind::Output : Kind::Input;
            r.key = ep->deviceId;
            r.title = ep->name;
            r.now = ep->cache;
            r.live = true;
            auto it = cfg_.devLocks.find(ep->deviceId);
            if (it != cfg_.devLocks.end()) { r.locked = true; r.lockedTo = it->second.target; r.restores = it->second.restores; }
            liveDev.insert(ep->deviceId);
            out.push_back(std::move(r));
        }
        for (const auto& kv : cfg_.devLocks) {
            if (liveDev.count(kv.first)) continue;
            Row r;
            r.kind = kv.second.input ? Kind::Input : Kind::Output;
            r.key = kv.first;
            r.title = kv.second.title.empty() ? L"Perangkat audio" : kv.second.title;
            r.now = kv.second.target;
            r.locked = true;
            r.lockedTo = kv.second.target;
            r.restores = kv.second.restores;
            out.push_back(std::move(r));
        }

        std::map<std::wstring, Row> apps;
        for (auto& ep : endpoints_) {
            for (auto& s : ep->sessions) {
                auto ins = apps.emplace(s->key, Row{});
                Row& r = ins.first->second;
                if (ins.second) {
                    r.kind = Kind::App;
                    r.key = s->key;
                    r.title = s->name;
                    r.icon = s->icon;
                    r.now = s->cache;
                    r.live = true;
                    auto it = cfg_.appLocks.find(s->key);
                    if (it != cfg_.appLocks.end()) { r.locked = true; r.lockedTo = it->second.target; r.restores = it->second.restores; }
                }
            }
        }
        for (const auto& kv : cfg_.appLocks) {
            if (apps.count(kv.first)) continue;
            Row r;
            r.kind = Kind::App;
            r.key = kv.first;
            r.title = kv.second.title.empty() ? kv.first : kv.second.title;
            r.now = kv.second.target;
            r.locked = true;
            r.lockedTo = kv.second.target;
            r.restores = kv.second.restores;
            apps.emplace(kv.first, std::move(r));
        }
        for (auto& kv : apps) out.push_back(std::move(kv.second));

        std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
            if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            return util::Lower(a.title) < util::Lower(b.title);
        });
        return out;
    }

    void SetLevel(Kind k, const std::wstring& key, float level) {
        level = std::clamp(level, 0.f, 1.f);
        if (IsLocked(k, key)) return;
        if (k == Kind::App) {
            for (auto& ep : endpoints_)
                for (auto& s : ep->sessions)
                    if (s->key == key) { s->vol->SetMasterVolume(level, &kOurContext); ReadSession(*s, &s->cache); }
        } else if (Endpoint* ep = FindEndpoint(key)) {
            ep->vol->SetMasterVolumeLevelScalar(level, &kOurContext);
            ReadEndpoint(*ep, &ep->cache);
        }
    }

    void SetMuted(Kind k, const std::wstring& key, bool muted) {
        if (IsLocked(k, key)) return;
        if (k == Kind::App) {
            for (auto& ep : endpoints_)
                for (auto& s : ep->sessions)
                    if (s->key == key) { s->vol->SetMute(muted, &kOurContext); ReadSession(*s, &s->cache); }
        } else if (Endpoint* ep = FindEndpoint(key)) {
            ep->vol->SetMute(muted, &kOurContext);
            ReadEndpoint(*ep, &ep->cache);
        }
    }

    void ToggleLock(Kind k, const std::wstring& key) {
        auto& locks = (k == Kind::App) ? cfg_.appLocks : cfg_.devLocks;
        auto it = locks.find(key);
        if (it != locks.end()) {
            locks.erase(it);
        } else {

            LockRec rec;
            rec.input = (k == Kind::Input);
            bool got = false;
            if (k == Kind::App) {
                for (auto& ep : endpoints_)
                    for (auto& s : ep->sessions)
                        if (!got && s->key == key) { got = ReadSession(*s, &rec.target); rec.title = s->name; }
            } else if (Endpoint* ep = FindEndpoint(key)) {
                got = ReadEndpoint(*ep, &rec.target);
                rec.title = ep->name;
            }
            if (!got) return;
            locks[key] = rec;
            MarkDirty(k, key);
            Pump();
        }
        cfg_.Save();
        UpdateHealthTimer();
        NotifyLocks();
    }

    void UnlockAll() {
        if (LockCount() == 0) return;
        cfg_.devLocks.clear();
        cfg_.appLocks.clear();
        cfg_.Save();
        UpdateHealthTimer();
        NotifyLocks();
    }

private:
    struct Identity {
        std::wstring key, name;
        HICON icon = nullptr;
    };

    struct AppSession {
        uint32_t id = 0;
        std::wstring instanceId, key, name;
        HICON icon = nullptr;
        Com<IAudioSessionControl2> ctl;
        Com<ISimpleAudioVolume> vol;
        Com<IAudioSessionEvents> events;
        bool registered = false;
        Volume cache;
        bool dirty = false;
        ULONGLONG lastEnforce = 0;
        ~AppSession() {

            if (registered && ctl && events) ctl->UnregisterAudioSessionNotification(events.Get());
        }
    };

    struct Endpoint {
        uint32_t id = 0;
        std::wstring deviceId, name;
        EDataFlow flow = eRender;
        Com<IMMDevice> device;
        Com<IAudioEndpointVolume> vol;
        Com<IAudioEndpointVolumeCallback> cb;
        bool cbRegistered = false;
        Com<IAudioSessionManager2> mgr;
        Com<IAudioSessionNotification> notifier;
        bool notifierRegistered = false;
        Volume cache;
        bool dirty = false;
        ULONGLONG lastEnforce = 0;
        std::vector<std::unique_ptr<AppSession>> sessions;
        ~Endpoint() {
            if (cbRegistered && vol && cb) vol->UnregisterControlChangeNotify(cb.Get());
            if (notifierRegistered && mgr && notifier) mgr->UnregisterSessionNotification(notifier.Get());
        }
    };

    HRESULT Init() {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), enumerator_.PutVoid());
        if (FAILED(hr)) return hr;
        notifier_.Attach(new DeviceNotifier(notify_));
        hr = enumerator_->RegisterEndpointNotificationCallback(notifier_.Get());
        if (FAILED(hr)) return hr;
        ResyncDevices();
        return S_OK;
    }

    void TryInit() {
        if (SUCCEEDED(Init())) {
            KillTimer(notify_, kTimerEngineRetry);
            Pump();
            UpdateHealthTimer();
            if (observed_) NotifyState();
        } else {
            Teardown();
            SetTimer(notify_, kTimerEngineRetry, kEngineRetryMs, nullptr);
        }
        NotifyLocks();
    }

    void Teardown() {
        if (notify_) {
            KillTimer(notify_, kTimerEnforce);
            KillTimer(notify_, kTimerDevices);
            KillTimer(notify_, kTimerHealth);
        }
        endpoints_.clear();
        if (enumerator_ && notifier_) enumerator_->UnregisterEndpointNotificationCallback(notifier_.Get());
        notifier_.Reset();
        enumerator_.Reset();
    }

    void UpdateHealthTimer() {
        if (!notify_) return;
        if (Ready() && LockCount() > 0) SetTimer(notify_, kTimerHealth, kHealthIntervalMs, nullptr);
        else KillTimer(notify_, kTimerHealth);
    }

    void HealthCheck() {
        if (!Ready()) return;
        for (auto& ep : endpoints_) {
            BOOL m = FALSE;
            if (FAILED(ep->vol->GetMute(&m))) {
                Teardown();
                TryInit();
                return;
            }
        }
    }

    Endpoint* FindEndpoint(const std::wstring& deviceId) {
        for (auto& ep : endpoints_) if (ep->deviceId == deviceId) return ep.get();
        return nullptr;
    }
    Endpoint* FindEndpointById(uint32_t id) {
        for (auto& ep : endpoints_) if (ep->id == id) return ep.get();
        return nullptr;
    }
    AppSession* FindSessionById(uint32_t id) {
        for (auto& ep : endpoints_)
            for (auto& s : ep->sessions) if (s->id == id) return s.get();
        return nullptr;
    }
    bool IsLocked(Kind k, const std::wstring& key) const {
        return (k == Kind::App) ? cfg_.appLocks.count(key) != 0 : cfg_.devLocks.count(key) != 0;
    }
    void MarkDirty(Kind k, const std::wstring& key) {
        if (k == Kind::App) {
            for (auto& ep : endpoints_)
                for (auto& s : ep->sessions) if (s->key == key) s->dirty = true;
        } else if (Endpoint* ep = FindEndpoint(key)) {
            ep->dirty = true;
        }
    }

    static bool ReadEndpoint(Endpoint& ep, Volume* out) {
        float l = 0.f; BOOL m = FALSE;
        if (FAILED(ep.vol->GetMasterVolumeLevelScalar(&l)) || FAILED(ep.vol->GetMute(&m))) return false;
        out->level = l; out->muted = (m != FALSE);
        return true;
    }
    static bool ReadSession(AppSession& s, Volume* out) {
        float l = 0.f; BOOL m = FALSE;
        if (FAILED(s.vol->GetMasterVolume(&l)) || FAILED(s.vol->GetMute(&m))) return false;
        out->level = l; out->muted = (m != FALSE);
        return true;
    }

    void EnforceEndpoint(Endpoint& ep) {
        auto it = cfg_.devLocks.find(ep.deviceId);
        if (it == cfg_.devLocks.end()) return;
        const Volume want = it->second.target;
        Volume cur;
        if (!ReadEndpoint(ep, &cur)) return;
        if (SameVolume(cur, want)) { ep.cache = cur; return; }
        if (std::fabs(cur.level - want.level) >= kLevelTolerance)
            ep.vol->SetMasterVolumeLevelScalar(want.level, &kOurContext);
        if (cur.muted != want.muted) ep.vol->SetMute(want.muted, &kOurContext);
        it->second.restores++;
        ReadEndpoint(ep, &ep.cache);
    }

    void EnforceSession(AppSession& s) {
        auto it = cfg_.appLocks.find(s.key);
        if (it == cfg_.appLocks.end()) return;
        const Volume want = it->second.target;
        Volume cur;
        if (!ReadSession(s, &cur)) return;
        if (SameVolume(cur, want)) { s.cache = cur; return; }
        if (std::fabs(cur.level - want.level) >= kLevelTolerance)
            s.vol->SetMasterVolume(want.level, &kOurContext);
        if (cur.muted != want.muted) s.vol->SetMute(want.muted, &kOurContext);
        it->second.restores++;
        ReadSession(s, &s.cache);
    }

    // Koreksi yang terlalu cepat ditunda dan diselesaikan oleh timer.
    void Pump() {
        const ULONGLONG now = GetTickCount64();
        bool deferred = false, changed = false;
        auto step = [&](bool& dirty, ULONGLONG& last, const std::function<void()>& enforce) {
            if (!dirty) return;
            if (now - last < kMinEnforceGapMs) { deferred = true; return; }
            dirty = false;
            last = now;
            enforce();
            changed = true;
        };
        for (auto& ep : endpoints_) {
            Endpoint* e = ep.get();
            step(e->dirty, e->lastEnforce, [&] { EnforceEndpoint(*e); });
            for (auto& s : e->sessions) {
                AppSession* p = s.get();
                step(p->dirty, p->lastEnforce, [&] { EnforceSession(*p); });
            }
        }
        if (deferred) SetTimer(notify_, kTimerEnforce, static_cast<UINT>(kMinEnforceGapMs), nullptr);
        else KillTimer(notify_, kTimerEnforce);
        if (changed && observed_) NotifyState();
    }

    void NotifyState() { if (onStateChanged) onStateChanged(); }
    void NotifyLocks() { if (onLocksChanged) onLocksChanged(); }

    static std::wstring DeviceName(IMMDevice* dev) {
        Com<IPropertyStore> ps;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, ps.Put()))) return L"Perangkat audio";
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::wstring name;
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        return name.empty() ? L"Perangkat audio" : name;
    }

    void ResyncDevices() {
        if (!enumerator_) return;
        std::set<std::wstring> seen;
        const EDataFlow flows[] = {eRender, eCapture};
        for (EDataFlow flow : flows) {
            Com<IMMDeviceCollection> coll;
            if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.Put()))) continue;
            UINT n = 0;
            if (FAILED(coll->GetCount(&n))) continue;
            for (UINT i = 0; i < n; ++i) {
                Com<IMMDevice> dev;
                if (FAILED(coll->Item(i, dev.Put()))) continue;
                LPWSTR raw = nullptr;
                if (FAILED(dev->GetId(&raw))) continue;
                std::wstring id = util::TakeCoString(raw);
                seen.insert(id);
                if (!FindEndpoint(id)) AddEndpoint(dev.Get(), id, flow);
            }
        }
        for (auto it = endpoints_.begin(); it != endpoints_.end();) {
            if (seen.count((*it)->deviceId)) ++it;
            else it = endpoints_.erase(it);
        }
        for (auto& ep : endpoints_) if (ep->flow == eRender) ResyncSessions(*ep);
    }

    void AddEndpoint(IMMDevice* dev, const std::wstring& id, EDataFlow flow) {
        auto ep = std::make_unique<Endpoint>();
        ep->id = nextId_++;
        ep->deviceId = id;
        ep->flow = flow;
        ep->name = DeviceName(dev);
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, ep->vol.PutVoid()))) return;

        ep->cb.Attach(new EndpointVolumeCallback(notify_, ep->id));
        ep->cbRegistered = SUCCEEDED(ep->vol->RegisterControlChangeNotify(ep->cb.Get()));
        if (!ep->cbRegistered) return;

        if (flow == eRender &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, ep->mgr.PutVoid()))) {
            ep->notifier.Attach(new SessionNotifier(notify_, ep->id));
            ep->notifierRegistered = SUCCEEDED(ep->mgr->RegisterSessionNotification(ep->notifier.Get()));
        }

        ReadEndpoint(*ep, &ep->cache);
        auto it = cfg_.devLocks.find(id);
        if (it != cfg_.devLocks.end()) { it->second.title = ep->name; ep->dirty = true; }
        endpoints_.push_back(std::move(ep));
    }

    static std::wstring ProcessPath(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return {};
        wchar_t buf[1024];
        DWORD n = 1024;
        BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &n);
        CloseHandle(h);
        return ok ? std::wstring(buf, n) : std::wstring();
    }

    static std::wstring FileDescription(const std::wstring& path) {
        DWORD dummy = 0;
        DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
        if (!size) return {};
        std::vector<BYTE> buf(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
        struct LangCp { WORD lang, cp; };
        LangCp* tr = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &len) ||
            !tr || len < sizeof(LangCp))
            return {};
        wchar_t sub[80];
        swprintf(sub, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
        wchar_t* val = nullptr;
        UINT vlen = 0;
        if (!VerQueryValueW(buf.data(), sub, reinterpret_cast<LPVOID*>(&val), &vlen) || !val || !vlen) return {};
        return val;
    }

    Identity Resolve(DWORD pid, bool system) {
        if (system) return Identity{L"#system", L"Suara Sistem", nullptr};
        const std::wstring path = ProcessPath(pid);
        if (path.empty()) return Identity{L"pid:" + std::to_wstring(pid), L"Proses " + std::to_wstring(pid), nullptr};

        const std::wstring cacheKey = util::Lower(path);
        auto it = identities_.find(cacheKey);
        if (it != identities_.end()) return it->second;

        Identity idn;
        size_t slash = path.find_last_of(L'\\');
        std::wstring file = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        idn.key = util::Lower(file);
        idn.name = FileDescription(path);
        if (idn.name.empty()) {
            std::wstring baseName = file;
            size_t dot = baseName.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) baseName.erase(dot);
            if (!baseName.empty()) baseName[0] = static_cast<wchar_t>(towupper(baseName[0]));
            idn.name = baseName;
        }
        HICON large = nullptr;
        if (ExtractIconExW(path.c_str(), 0, &large, nullptr, 1) >= 1) idn.icon = large;
        identities_[cacheKey] = idn;
        return idn;
    }

    void ResyncSessions(Endpoint& ep) {
        if (!ep.mgr) return;
        Com<IAudioSessionEnumerator> en;
        if (FAILED(ep.mgr->GetSessionEnumerator(en.Put()))) return;
        int count = 0;
        if (FAILED(en->GetCount(&count))) return;

        std::set<std::wstring> seen;
        for (int i = 0; i < count; ++i) {
            Com<IAudioSessionControl> ctl;
            if (FAILED(en->GetSession(i, ctl.Put()))) continue;
            AudioSessionState st = AudioSessionStateInactive;
            ctl->GetState(&st);
            if (st == AudioSessionStateExpired) continue;
            Com<IAudioSessionControl2> ctl2;
            if (FAILED(ctl.As(ctl2))) continue;
            LPWSTR raw = nullptr;
            if (FAILED(ctl2->GetSessionInstanceIdentifier(&raw))) continue;
            const std::wstring inst = util::TakeCoString(raw);
            seen.insert(inst);

            bool known = false;
            for (auto& s : ep.sessions) if (s->instanceId == inst) { known = true; break; }
            if (!known) AddSession(ep, ctl2, inst);
        }
        for (auto it = ep.sessions.begin(); it != ep.sessions.end();) {
            if (seen.count((*it)->instanceId)) ++it;
            else it = ep.sessions.erase(it);
        }
    }

    void AddSession(Endpoint& ep, const Com<IAudioSessionControl2>& ctl2, const std::wstring& inst) {
        auto s = std::make_unique<AppSession>();
        s->id = nextId_++;
        s->instanceId = inst;
        s->ctl = ctl2;
        if (FAILED(ctl2->QueryInterface(__uuidof(ISimpleAudioVolume), s->vol.PutVoid()))) return;

        DWORD pid = 0;
        ctl2->GetProcessId(&pid);
        const bool system = (ctl2->IsSystemSoundsSession() == S_OK);
        Identity idn = Resolve(pid, system);
        s->key = idn.key;
        s->name = idn.name;
        s->icon = idn.icon;

        s->events.Attach(new SessionEvents(notify_, s->id, ep.id));
        s->registered = SUCCEEDED(ctl2->RegisterAudioSessionNotification(s->events.Get()));
        ReadSession(*s, &s->cache);

        auto it = cfg_.appLocks.find(s->key);
        if (it != cfg_.appLocks.end()) { it->second.title = s->name; s->dirty = true; }
        ep.sessions.push_back(std::move(s));
    }

    Config& cfg_;
    HWND notify_ = nullptr;
    bool observed_ = false;
    uint32_t nextId_ = 1;
    Com<IMMDeviceEnumerator> enumerator_;
    Com<IMMNotificationClient> notifier_;
    std::vector<std::unique_ptr<Endpoint>> endpoints_;
    std::map<std::wstring, Identity> identities_;
};

// Sesi GDI+ hidup hanya selama jendela mixer terbuka.
class GdiplusSession {
public:
    GdiplusSession() {
        G::GdiplusStartupInput in;
        ok_ = (G::GdiplusStartup(&token_, &in, nullptr) == G::Ok);
    }
    ~GdiplusSession() { if (ok_) G::GdiplusShutdown(token_); }
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    bool ok() const { return ok_; }

private:
    ULONG_PTR token_ = 0;
    bool ok_ = false;
};

namespace theme {
inline G::Color Bg()        { return G::Color(255, 26, 27, 32); }
inline G::Color Card()      { return G::Color(255, 37, 39, 46); }
inline G::Color Tile()      { return G::Color(255, 55, 58, 70); }
inline G::Color Border()    { return G::Color(255, 56, 59, 68); }
inline G::Color Text()      { return G::Color(255, 236, 238, 242); }
inline G::Color Muted()     { return G::Color(255, 150, 156, 168); }
inline G::Color Accent()    { return G::Color(255, 84, 150, 255); }
inline G::Color Amber()     { return G::Color(255, 255, 183, 51); }
inline G::Color AmberBg()   { return G::Color(255, 70, 56, 22); }
inline G::Color AmberBgHot(){ return G::Color(255, 88, 70, 26); }
inline G::Color Track()     { return G::Color(255, 68, 71, 82); }
inline G::Color Button()    { return G::Color(255, 52, 55, 65); }
inline G::Color ButtonHot() { return G::Color(255, 66, 70, 82); }
}

void RoundPath(G::GraphicsPath& p, const G::RectF& r, float rad) {
    const float d = rad * 2.f;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(G::Graphics& g, const G::Color& c, const G::RectF& r, float rad) {
    G::GraphicsPath p;
    RoundPath(p, r, rad);
    G::SolidBrush b(c);
    g.FillPath(&b, &p);
}

namespace glyph {

void Speaker(G::Graphics& g, float x, float y, float size, const G::Color& c, bool muted, bool waves = true) {
    G::GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16.f, size / 16.f);
    G::SolidBrush b(c);
    const G::PointF body[] = {{1.5f, 6.f}, {4.8f, 6.f}, {9.f, 2.8f}, {9.f, 13.2f}, {4.8f, 10.f}, {1.5f, 10.f}};
    g.FillPolygon(&b, body, 6);
    G::Pen pen(c, 1.3f);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    if (muted) {
        g.DrawLine(&pen, 11.f, 5.8f, 15.f, 10.2f);
        g.DrawLine(&pen, 15.f, 5.8f, 11.f, 10.2f);
    } else if (waves) {
        g.DrawArc(&pen, 9.f - 3.2f, 8.f - 3.2f, 6.4f, 6.4f, -50.f, 100.f);
        g.DrawArc(&pen, 9.f - 5.8f, 8.f - 5.8f, 11.6f, 11.6f, -50.f, 100.f);
    }
    g.Restore(st);
}

void Mic(G::Graphics& g, float x, float y, float size, const G::Color& c, bool muted) {
    G::GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16.f, size / 16.f);
    G::SolidBrush b(c);
    FillRound(g, c, G::RectF(5.8f, 1.5f, 4.4f, 8.f), 2.2f);
    G::Pen pen(c, 1.3f);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    g.DrawArc(&pen, 3.6f, 3.6f, 8.8f, 7.6f, 0.f, 180.f);
    g.DrawLine(&pen, 8.f, 11.4f, 8.f, 14.f);
    g.DrawLine(&pen, 5.6f, 14.f, 10.4f, 14.f);
    if (muted) g.DrawLine(&pen, 2.5f, 2.5f, 13.5f, 13.5f);
    g.Restore(st);
}

void Lock(G::Graphics& g, float x, float y, float size, const G::Color& c, bool locked) {
    G::GraphicsState st = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16.f, size / 16.f);
    FillRound(g, c, G::RectF(3.2f, 7.4f, 9.6f, 7.2f), 1.6f);
    G::Pen pen(c, 1.5f);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    if (locked) {
        g.DrawArc(&pen, 5.f, 2.f, 6.f, 7.f, 180.f, 180.f);
        g.DrawLine(&pen, 5.f, 5.5f, 5.f, 7.6f);
        g.DrawLine(&pen, 11.f, 5.5f, 11.f, 7.6f);
    } else {
        g.DrawArc(&pen, 5.f, 0.4f, 6.f, 7.f, 180.f, 180.f);
        g.DrawLine(&pen, 5.f, 3.9f, 5.f, 7.6f);
    }
    g.Restore(st);
}

}

namespace mascot {

constexpr float kDisplayW = 96.f;
constexpr float kAspect = 232.f / 162.f;
constexpr float kPi = 3.14159265f;

const unsigned char kPng[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,232,0,0,0,162,
    8,3,0,0,0,124,180,113,64,0,0,3,0,80,76,84,69,96,35,79,43,27,84,27,
    22,48,44,31,92,154,156,165,31,10,46,42,36,85,147,41,93,154,98,228,156,46,101,212,
    101,204,89,35,82,94,87,107,90,31,89,44,26,83,82,40,99,154,74,114,225,87,125,226,
    209,232,44,9,50,180,162,185,72,17,59,80,9,56,101,20,55,113,87,112,102,90,137,118,
    108,142,51,15,52,88,52,152,159,92,116,207,178,189,225,236,232,145,51,96,151,122,161,57,
    37,129,41,83,82,124,100,192,154,43,93,164,75,121,150,107,148,70,24,62,95,139,135,150,
    46,160,157,71,112,173,143,209,182,208,183,178,226,219,205,179,213,0,85,0,59,255,255,83,
    61,135,140,0,59,184,85,130,255,62,161,255,87,167,195,166,189,193,158,201,0,0,0,251,
    136,175,251,230,239,251,148,183,235,199,249,250,166,202,215,155,243,245,121,169,253,250,252,219,
    194,251,248,151,200,246,215,240,247,140,200,202,167,245,235,184,250,249,182,210,214,184,250,250,
    200,215,231,168,248,246,118,155,236,103,150,237,106,168,182,151,239,210,87,141,232,150,233,230,
    89,146,251,210,220,209,120,207,206,136,232,211,102,148,212,118,174,211,134,211,175,136,240,49,
    24,75,245,163,188,176,55,109,211,104,173,213,134,175,207,73,136,21,6,43,203,71,120,234,
    122,199,46,6,46,40,24,80,45,6,52,76,7,51,205,183,201,205,88,173,178,71,117,59,
    36,82,74,39,83,175,120,237,213,150,212,179,87,145,18,2,46,78,54,110,189,162,240,214,
    86,122,209,117,153,46,8,49,78,25,73,140,71,112,177,103,147,144,37,88,177,119,209,206,
    105,201,211,152,179,176,152,176,213,199,213,144,87,141,144,104,137,146,55,107,244,227,245,51,
    19,57,21,8,71,208,166,183,110,70,108,236,216,241,109,57,107,146,119,207,176,72,140,56,
    38,108,176,88,175,21,6,72,111,22,77,139,26,81,140,86,118,174,135,172,44,21,54,109,
    38,82,174,118,146,200,59,114,77,22,58,148,118,146,169,42,102,111,73,140,179,102,175,172,
    136,212,178,165,183,40,24,83,113,86,117,145,71,145,175,104,207,208,168,202,76,42,108,87,
    70,117,139,106,199,175,150,211,180,168,201,110,85,144,142,88,169,148,135,172,229,140,231,17,
    1,48,55,37,83,79,55,138,119,104,144,140,101,178,75,8,57,168,39,91,19,3,42,17,
    2,50,24,7,71,48,38,80,140,120,176,173,133,153,15,1,42,122,100,179,172,72,166,177,
    90,204,200,120,228,21,5,73,44,11,72,82,54,87,89,71,141,176,57,141,9,0,28,75,
    11,57,115,40,103,148,41,104,146,56,138,40,27,101,58,40,100,70,52,111,172,120,173,23,
    6,42,50,6,50,44,4,44,112,88,173,148,86,199,172,106,234,216,212,220,11,0,29,42,
    23,81,155,131,211,229,76,138,23,8,70,29,18,74,44,26,76,110,54,91,104,59,137,150,
    131,152,207,132,156,219,215,233,232,92,166,5,0,26,41,25,57,38,23,81,76,23,68,82,
    21,66,108,9,56,108,75,171,21,7,71,29,18,50,75,17,60,107,13,70,188,180,198,242,
    131,158,36,21,78,80,10,71,101,26,73,155,146,170,177,82,123,199,74,169,8,0,28,44,
    24,85,67,45,108,74,54,85,74,69,85,79,70,110,140,39,88,151,73,174,151,99,123,177,
    53,91,202,60,133,255,255,255,38,13,71,69,18,61,69,55,106,4,73,14,85,0,0,1,
    0,116,82,78,83,27,91,31,225,6,92,33,242,255,9,254,87,18,231,155,158,16,254,221,
    148,238,99,161,246,254,11,255,224,255,254,255,4,102,217,255,14,255,153,142,6,26,7,255,
    85,236,5,4,217,6,2,2,1,100,1,1,9,5,0,254,254,254,254,254,254,254,254,254,
    254,254,254,254,254,254,254,254,254,254,254,254,255,254,254,254,254,254,254,254,254,254,254,253,
    254,254,254,254,255,3,254,255,251,3,215,251,254,255,254,255,254,254,254,255,246,254,255,255,
    254,2,254,254,254,252,254,255,254,254,254,255,255,254,217,255,2,254,255,217,254,254,255,253,
    254,249,254,253,254,254,2,254,254,254,255,255,254,255,254,254,254,215,255,254,255,254,254,255,
    255,253,254,255,254,254,255,172,5,254,255,254,3,254,110,213,211,44,254,254,145,254,255,255,
    255,174,252,254,254,255,208,216,254,254,255,252,3,8,254,75,108,176,255,255,254,253,242,174,
    255,254,146,2,53,255,255,255,254,253,255,184,48,106,8,215,254,254,110,4,217,252,254,255,
    82,251,2,254,255,255,144,145,2,25,17,19,4,255,254,255,255,2,3,2,40,228,125,116,
    249,0,0,59,26,73,68,65,84,120,218,221,157,7,124,28,213,185,232,69,39,16,210,123,
    185,185,189,189,91,94,127,111,167,237,25,77,221,50,163,209,106,155,180,141,149,140,10,170,
    182,80,151,109,5,236,216,114,199,189,224,142,141,109,108,199,24,108,12,132,118,41,9,132,
    22,74,232,55,36,144,92,82,110,122,114,83,95,222,247,157,51,51,59,43,75,182,192,247,
    7,247,119,15,70,94,175,180,187,243,159,175,127,231,155,81,77,232,63,222,250,217,151,191,
    60,229,153,43,175,59,231,55,173,249,143,70,249,21,6,249,149,231,191,255,237,231,190,68,
    215,115,223,255,67,40,116,221,117,255,169,64,231,124,5,191,254,225,181,159,222,208,180,172,
    111,255,145,19,7,14,28,56,113,228,222,190,166,91,207,3,212,255,60,160,40,203,11,207,
    251,233,13,203,246,28,25,159,207,203,149,213,246,169,21,95,250,229,57,146,214,252,135,194,
    124,254,167,203,250,142,140,63,69,233,120,92,30,234,119,151,173,184,45,52,231,63,3,232,
    28,20,230,13,125,71,246,1,27,175,138,181,222,82,61,212,61,64,250,245,119,31,116,206,
    156,57,215,93,119,221,239,127,63,7,215,151,223,242,203,111,14,133,222,251,218,195,247,254,
    80,5,200,218,224,82,85,213,19,235,158,21,127,56,23,153,254,123,128,206,185,110,26,238,
    57,111,229,44,133,66,223,190,251,222,109,188,172,214,70,124,70,17,23,96,170,170,40,82,
    214,11,110,125,119,85,151,234,211,197,231,125,251,22,136,6,207,125,251,219,231,125,255,226,
    79,206,16,13,103,214,218,219,110,93,54,206,243,98,80,150,34,91,32,82,81,101,26,252,
    212,138,247,157,131,67,58,87,208,175,95,23,186,240,251,207,221,186,2,194,193,158,61,247,
    238,233,219,189,172,233,238,21,183,126,233,181,231,127,137,193,240,103,95,57,251,59,192,27,
    188,175,233,136,202,35,220,20,76,20,38,93,240,4,144,126,170,247,43,111,223,76,207,17,
    20,206,240,107,189,77,251,15,124,215,119,26,178,58,127,219,129,253,125,77,77,79,159,247,
    7,202,122,214,119,120,254,214,190,125,232,126,68,113,170,60,43,156,181,17,32,229,155,158,
    123,251,34,173,57,71,181,253,197,151,86,28,121,197,69,228,121,63,36,240,243,247,125,106,
    207,178,11,206,163,114,61,179,121,191,182,98,127,173,26,153,86,105,85,151,18,87,4,78,
    229,145,227,239,146,141,206,9,253,97,69,83,27,131,12,132,132,72,164,22,188,136,204,183,
    29,184,119,217,211,151,135,66,159,188,249,12,230,249,220,138,19,254,75,61,249,193,27,168,
    232,132,170,28,112,45,47,239,91,113,241,219,214,221,154,115,228,92,198,156,191,235,72,2,
    170,70,15,150,255,238,145,190,11,46,247,19,88,120,201,215,33,10,93,233,38,234,95,159,
    19,250,220,161,21,91,85,143,18,95,164,235,250,220,186,186,136,251,14,193,5,34,125,101,
    197,121,111,91,119,107,206,133,243,194,227,148,147,199,211,207,172,172,66,90,235,10,166,246,
    68,223,13,183,177,188,199,15,176,159,251,228,123,127,121,33,62,248,221,173,43,110,164,106,
    139,148,122,29,174,185,141,141,58,190,182,246,244,37,203,203,190,253,110,128,94,23,250,82,
    211,43,84,152,212,158,84,49,184,60,212,136,168,206,63,178,251,125,159,12,93,124,243,28,
    26,134,206,123,238,208,241,222,222,21,43,122,143,31,250,62,188,193,141,98,4,189,173,71,
    89,183,117,233,141,211,49,210,55,227,229,61,183,191,11,160,215,133,190,191,98,31,56,89,
    61,32,201,41,164,34,83,71,117,223,254,38,40,63,66,183,253,244,214,222,21,77,77,125,
    71,126,184,109,223,190,125,7,238,93,113,129,43,207,200,92,138,169,215,45,125,124,105,68,
    172,157,9,84,148,79,60,29,186,248,93,176,209,227,123,128,179,78,103,129,33,16,13,60,
    84,176,184,185,17,170,193,145,83,15,63,29,122,173,233,222,3,109,79,169,149,64,116,98,
    197,82,180,207,8,19,230,220,198,83,111,28,213,103,196,68,109,150,15,188,63,244,179,119,
    26,244,186,208,121,43,230,131,60,117,29,100,32,158,46,84,56,252,198,198,165,176,192,179,
    128,84,183,46,187,161,233,0,115,207,248,79,176,94,121,223,27,167,68,15,179,14,132,249,
    248,82,93,20,35,181,103,32,149,199,223,23,250,202,59,15,122,8,4,170,227,10,58,92,
    79,152,115,1,19,254,52,34,236,92,138,118,184,79,150,25,36,91,181,135,15,71,60,219,
    92,186,247,20,156,144,51,72,211,5,221,247,116,232,147,239,184,234,126,238,248,1,185,118,
    174,206,68,234,39,225,232,125,35,145,185,128,183,119,239,210,191,107,100,24,207,232,0,182,
    244,4,31,169,21,125,82,113,235,159,110,101,152,207,44,93,250,204,220,136,120,54,76,10,
    250,165,208,127,127,135,65,191,30,186,173,183,77,166,156,64,58,205,65,233,115,111,124,102,
    233,81,160,5,216,198,165,117,181,250,227,212,241,248,2,21,31,63,12,39,164,238,153,189,
    123,159,209,107,103,65,73,221,238,129,47,189,227,94,247,186,208,249,77,60,120,34,119,213,
    158,118,164,204,80,245,173,123,79,157,218,187,180,110,239,82,253,141,27,197,218,160,230,222,
    187,183,113,233,227,111,28,173,139,204,142,146,130,30,57,244,142,131,206,9,157,183,140,143,
    232,254,154,193,137,160,189,234,55,46,61,245,231,159,249,243,123,33,100,50,68,134,123,239,
    222,207,28,222,59,183,118,182,148,20,20,226,232,59,157,2,222,28,122,173,79,213,131,107,
    70,127,137,146,221,122,106,217,195,122,109,36,160,187,250,159,254,233,9,239,53,98,237,236,
    120,249,171,191,253,46,72,244,226,190,90,125,202,138,68,102,8,128,192,49,247,222,125,52,
    178,178,96,162,71,244,195,207,68,240,21,21,93,62,155,165,70,212,249,43,158,127,219,221,
    148,115,1,141,76,5,157,203,142,252,52,229,165,127,239,125,70,116,41,49,194,214,213,61,
    238,255,139,173,185,51,158,40,95,115,127,216,251,55,239,120,153,246,229,208,197,203,246,233,
    211,45,207,19,51,5,173,172,165,167,34,62,37,134,157,199,27,235,78,95,115,245,51,176,
    202,125,135,222,141,194,251,43,77,91,245,51,172,185,20,41,184,222,208,235,2,207,133,151,
    238,117,127,114,10,235,76,168,144,50,247,158,67,111,247,28,114,221,27,142,158,25,211,21,
    95,163,39,198,199,247,134,235,168,44,217,83,127,254,240,238,195,247,110,62,181,116,43,213,
    90,125,238,92,55,229,133,255,35,211,107,238,5,135,66,87,190,243,29,134,57,161,231,54,
    71,244,179,172,185,21,96,248,115,47,212,39,117,35,119,94,250,153,195,47,110,126,113,247,
    192,196,143,134,122,22,244,44,92,176,96,193,254,163,39,158,169,171,115,197,91,133,26,169,
    178,208,115,105,214,159,3,232,183,151,233,179,90,46,236,210,195,77,135,247,222,244,208,157,
    186,190,243,111,203,235,243,14,145,76,83,146,36,65,224,56,174,31,144,23,44,56,122,226,
    198,185,174,101,159,102,171,17,181,173,239,181,119,165,129,61,39,244,252,138,173,179,226,156,
    91,167,207,109,124,188,239,129,137,142,252,196,166,81,172,96,196,75,75,28,32,246,207,203,
    31,59,182,106,213,177,178,99,113,28,137,91,233,31,13,14,110,222,127,98,43,77,124,131,
    129,7,51,198,125,135,223,127,78,219,76,231,82,143,222,250,120,100,86,34,173,59,117,120,
    96,109,62,97,197,227,214,78,90,136,234,138,205,73,92,218,113,210,233,180,51,175,35,127,
    236,146,99,243,226,192,74,128,214,233,24,92,178,120,255,137,27,35,181,149,78,5,84,186,
    7,250,222,15,39,247,202,43,175,124,231,65,103,167,187,145,186,163,155,215,174,205,207,3,
    160,4,199,253,184,150,118,126,147,81,145,227,4,65,145,216,18,184,116,199,111,126,51,143,
    8,32,86,224,229,226,214,143,6,31,24,216,189,249,212,1,87,149,231,158,56,188,162,233,
    162,139,105,51,241,237,178,158,11,232,87,86,156,58,179,72,35,115,151,238,95,50,56,47,
    30,39,4,228,197,9,137,20,246,126,229,8,128,138,105,202,4,184,57,1,23,103,149,243,
    14,181,87,92,132,139,59,195,111,254,224,7,87,92,177,140,174,221,61,67,59,118,55,245,
    222,127,251,163,63,127,187,172,231,214,28,123,248,76,148,250,51,251,119,79,204,227,4,46,
    78,151,96,90,63,230,121,208,66,57,172,152,130,41,57,236,121,206,167,139,151,203,113,252,
    7,254,143,103,33,94,30,94,229,144,66,161,144,209,232,42,244,44,233,236,237,253,231,91,
    206,127,91,172,231,0,10,37,233,138,253,145,185,51,80,110,125,124,247,64,7,30,56,195,
    140,91,166,153,200,202,134,170,26,98,24,148,149,51,5,78,96,223,225,188,37,196,203,14,
    35,21,168,84,5,161,103,213,100,179,70,31,195,66,216,226,198,61,215,126,141,177,94,243,
    214,88,207,173,129,253,218,178,27,35,211,82,158,234,251,193,155,128,137,114,162,139,88,156,
    40,180,215,138,134,42,198,140,148,132,199,109,130,48,9,40,117,16,149,75,148,137,64,21,
    26,207,16,225,36,101,226,65,162,17,78,243,150,162,72,230,182,237,215,126,237,254,91,190,
    193,12,246,157,217,146,248,114,232,253,135,167,144,130,231,216,122,98,255,224,218,215,59,92,
    125,100,66,179,156,104,148,107,87,13,67,52,12,35,41,48,235,180,4,137,10,43,72,26,
    47,91,240,93,13,56,169,84,5,49,51,236,104,193,197,105,18,207,127,4,89,47,250,198,
    239,222,130,18,159,3,232,119,174,252,78,40,244,254,123,125,82,140,244,207,156,218,127,116,
    139,83,58,118,172,72,165,70,92,181,117,142,73,162,105,39,145,210,48,98,73,87,83,9,
    81,64,137,227,156,235,108,61,161,90,64,169,113,46,41,23,149,242,29,138,226,138,147,161,
    114,18,47,203,31,249,196,213,189,247,255,228,209,11,103,171,196,111,23,212,61,145,127,53,
    231,130,101,247,62,195,162,229,210,83,247,46,235,219,81,224,72,121,85,135,105,198,61,78,
    98,149,75,155,122,68,211,180,195,70,21,40,216,36,39,229,4,52,70,142,11,144,90,9,
    80,220,36,231,10,149,51,197,201,188,166,80,78,197,213,95,141,83,176,59,220,118,164,179,
    247,107,135,30,189,152,29,207,245,255,254,160,46,228,207,31,253,201,63,247,246,54,117,46,
    217,189,108,217,225,195,203,30,222,125,247,15,134,20,197,118,242,171,250,69,179,162,183,137,
    146,101,29,19,32,223,43,196,92,208,81,15,20,212,86,18,92,71,27,39,238,147,240,178,
    4,60,161,37,125,82,53,189,137,83,0,175,66,10,127,75,180,17,62,255,165,123,90,192,
    57,49,131,189,230,154,127,79,208,239,80,202,207,157,127,209,63,127,173,247,130,79,108,44,
    114,112,4,153,45,91,150,183,147,201,9,73,33,142,149,31,22,212,168,224,115,150,211,132,
    148,59,36,41,42,249,160,201,138,69,114,130,71,234,6,25,120,0,238,136,145,50,159,4,
    39,67,45,122,164,174,68,241,145,98,178,141,60,117,124,59,141,176,239,61,179,96,107,222,
    134,44,255,236,209,159,220,223,219,249,137,109,79,169,38,251,104,60,213,205,147,131,162,226,
    56,164,156,55,65,77,5,166,184,224,91,8,24,221,166,34,72,84,26,245,64,155,125,80,
    20,41,37,21,92,38,250,183,75,170,100,233,63,241,103,84,97,19,81,52,95,158,222,50,
    221,237,13,254,143,22,82,193,222,198,142,241,59,231,8,122,61,165,188,237,22,208,215,123,
    94,154,15,167,82,210,42,126,95,155,88,32,114,40,188,124,84,52,163,212,19,197,137,64,
    28,78,176,56,43,47,112,0,154,140,197,166,74,20,220,42,85,94,119,121,129,197,37,149,
    82,21,82,37,207,41,149,24,227,46,79,129,169,18,143,47,190,22,189,211,47,102,16,108,
    205,91,162,60,31,40,175,221,190,13,223,93,85,56,77,169,196,183,252,144,137,70,150,207,
    139,192,105,226,193,97,152,116,192,181,112,18,228,118,0,170,36,13,87,164,1,80,68,11,
    144,50,37,6,119,12,41,161,224,147,98,64,85,165,32,169,15,11,15,68,127,188,140,223,
    6,130,237,61,196,44,118,206,156,155,223,6,40,165,188,248,150,251,191,214,180,125,27,125,
    95,196,12,44,101,114,157,68,20,142,43,51,78,38,80,42,22,219,22,200,38,14,65,133,
    230,105,65,33,217,157,134,148,99,164,98,128,84,89,175,205,176,164,10,171,252,212,15,23,
    83,139,69,193,206,185,249,230,183,4,250,29,164,252,197,163,84,150,238,224,73,84,9,72,
    19,56,123,134,64,189,8,231,228,69,19,220,43,181,80,136,23,192,73,108,200,11,64,115,
    17,244,160,7,122,87,53,104,80,121,93,123,5,229,133,252,16,206,129,152,117,73,5,83,
    229,74,51,145,106,74,44,192,202,127,247,83,247,244,246,30,250,62,14,138,252,236,230,217,
    130,94,143,148,159,124,244,16,82,186,239,165,74,224,110,170,56,157,137,180,16,229,184,244,
    177,168,9,68,166,130,65,130,128,113,162,226,10,66,121,30,3,173,55,220,149,10,120,31,
    250,224,116,82,80,94,120,189,4,120,73,79,166,38,159,41,41,211,65,186,42,44,25,106,
    5,246,149,241,253,87,55,61,29,28,138,169,57,43,101,232,27,183,127,173,101,207,184,247,
    38,162,164,64,150,35,41,1,251,44,12,247,155,162,68,226,195,146,201,161,123,165,71,198,
    197,53,44,165,225,97,62,13,199,14,206,85,241,65,149,32,85,53,37,80,75,28,83,94,
    76,17,133,168,154,100,85,92,156,19,85,103,84,153,22,180,158,46,45,28,214,13,127,116,
    16,44,246,72,31,176,254,206,101,172,57,139,97,222,118,203,253,189,87,191,244,148,247,98,
    83,113,207,160,31,188,53,133,172,202,64,64,225,72,190,104,226,225,73,76,86,113,13,4,
    107,227,195,77,164,26,84,12,107,57,161,18,83,148,41,164,112,166,56,33,7,26,33,73,
    22,36,78,170,218,204,72,45,120,88,202,206,32,83,4,13,179,165,199,12,209,149,45,255,
    221,141,125,55,0,234,53,103,0,165,194,124,47,56,217,171,143,180,249,51,97,38,243,120,
    149,180,147,114,78,140,137,40,134,114,127,148,208,99,207,177,210,35,198,81,5,20,72,30,
    159,149,148,10,168,174,249,108,57,137,158,131,106,80,36,5,145,154,162,64,36,65,226,99,
    120,162,144,84,225,205,82,108,234,231,211,19,95,31,0,117,113,93,90,254,0,206,255,92,
    55,19,40,197,252,198,79,64,101,183,249,38,46,42,211,186,1,50,41,137,2,212,209,86,
    135,233,5,124,164,179,98,168,185,248,208,202,187,207,42,177,10,40,0,228,224,15,252,133,
    190,72,242,196,203,56,145,84,225,136,105,70,65,172,156,33,30,196,12,31,222,210,146,120,
    174,221,240,116,106,10,103,120,154,21,19,33,16,242,71,154,110,9,93,57,45,40,234,236,
    123,33,150,92,253,146,63,252,38,198,148,233,221,157,221,99,70,33,12,152,36,159,147,240,
    145,107,121,113,197,208,64,40,104,140,206,122,239,89,240,70,200,42,26,89,214,42,98,22,
    25,23,88,223,136,170,66,142,114,34,41,72,53,106,154,4,144,69,67,163,93,7,98,17,
    81,238,79,26,74,80,158,120,12,51,113,82,201,2,234,120,47,144,214,76,27,76,206,191,
    189,183,101,251,119,125,149,53,234,15,30,172,87,166,115,4,164,39,138,156,144,252,245,227,
    190,190,233,10,198,226,140,122,208,92,228,84,74,101,207,235,128,72,193,126,96,121,160,32,
    84,9,236,218,251,23,126,97,160,212,163,161,72,193,132,57,73,140,53,211,4,31,170,119,
    149,119,194,134,114,154,215,157,145,19,150,40,203,7,122,159,159,42,81,20,230,223,95,116,
    188,247,234,141,94,26,9,148,108,77,3,170,144,180,40,97,12,144,72,153,54,39,77,166,
    130,156,101,24,7,65,4,112,180,138,82,158,135,207,41,8,26,118,65,83,76,136,244,171,
    137,206,21,23,115,216,46,40,60,148,32,118,74,209,12,216,131,97,132,105,223,129,179,45,
    73,54,219,13,227,52,87,116,6,78,61,108,200,242,158,67,213,160,215,255,30,50,131,219,
    123,123,61,203,4,74,29,126,246,12,156,170,164,64,128,51,149,146,160,50,80,170,145,150,
    0,7,131,126,19,125,112,62,129,140,20,43,9,186,139,19,29,97,205,164,164,172,219,137,
    223,167,202,139,120,2,109,223,51,25,163,43,82,44,80,12,209,72,81,82,66,136,33,23,
    71,69,99,246,156,184,84,121,91,239,47,2,160,56,195,118,219,237,189,77,71,158,10,82,
    122,152,129,98,208,125,168,112,142,10,0,146,10,49,212,161,156,170,128,102,42,161,64,141,
    102,16,40,61,220,188,197,64,128,195,102,34,85,245,42,80,43,78,79,4,245,191,38,138,
    213,229,196,116,144,83,8,21,169,91,8,144,56,167,202,61,97,208,160,10,168,54,3,167,
    30,139,233,238,35,121,126,111,197,70,111,254,25,96,30,234,189,250,0,155,183,21,13,255,
    21,211,129,98,36,213,28,17,15,46,10,180,142,73,57,77,65,69,153,58,130,104,196,56,
    15,180,28,119,65,193,191,36,153,238,26,154,84,209,93,137,102,181,174,25,123,207,193,82,
    88,35,16,235,3,8,76,49,155,130,218,113,73,142,110,137,137,205,182,79,90,63,13,33,
    50,166,182,142,164,244,24,125,70,126,170,215,87,93,72,148,110,123,122,69,223,248,84,202,
    42,153,86,131,58,144,37,160,226,42,18,170,48,78,200,65,232,67,9,33,118,61,135,61,
    60,10,74,192,251,50,77,148,72,61,213,93,177,219,3,165,223,200,112,149,228,207,243,197,
    16,119,65,83,49,174,96,243,27,156,117,138,48,43,229,68,80,94,195,104,183,155,105,245,
    29,228,164,132,240,247,214,145,163,55,109,198,213,183,123,47,253,6,223,214,123,126,141,59,
    122,17,122,239,79,155,246,239,11,104,236,89,72,53,165,189,91,197,106,82,213,57,179,4,
    121,188,170,70,81,59,225,200,215,115,32,51,0,133,52,49,8,170,128,115,209,154,99,84,
    119,195,138,23,82,144,41,147,225,188,198,117,85,222,203,218,185,156,141,34,141,25,180,92,
    39,86,92,82,213,155,194,234,150,59,238,104,47,52,163,60,241,216,98,58,30,51,200,112,
    203,209,155,22,44,24,26,122,164,72,63,91,90,242,70,24,191,193,255,240,126,42,209,43,
    191,28,250,251,247,49,204,105,40,43,218,27,176,127,8,160,25,149,80,197,45,72,92,90,
    116,243,27,180,59,171,12,2,69,167,139,79,128,237,149,81,3,21,72,140,16,61,171,197,
    98,212,72,81,100,172,38,83,164,66,35,241,10,25,206,221,161,112,115,126,226,118,150,80,
    164,97,194,72,137,200,71,118,138,187,10,70,118,203,142,7,30,218,177,99,114,199,142,5,
    61,108,13,225,41,243,118,116,240,164,47,238,98,170,123,228,39,8,10,17,229,188,11,208,
    209,242,162,62,131,223,162,160,1,79,7,255,21,84,116,151,166,26,38,146,35,9,110,219,
    25,93,74,41,29,21,209,233,114,76,96,0,74,51,36,56,2,144,172,114,176,62,102,168,
    98,68,147,42,197,167,68,70,116,226,239,75,4,72,221,14,61,71,152,72,61,229,149,84,
    126,36,165,22,193,88,140,140,19,39,182,101,17,122,66,188,190,131,84,33,125,49,69,85,
    154,191,250,34,0,253,78,40,244,225,101,7,0,211,56,131,135,166,154,27,36,117,68,195,
    2,93,52,197,203,56,146,150,226,44,124,66,2,32,216,14,106,46,83,52,122,184,101,44,
    216,220,78,159,160,197,154,81,164,162,82,41,96,192,206,119,70,178,113,151,52,78,170,58,
    43,30,40,138,52,230,42,47,148,49,234,19,134,216,142,73,150,153,161,148,238,73,171,198,
    148,164,226,27,212,25,25,243,155,190,67,85,247,208,213,144,183,139,103,9,70,85,164,96,
    160,34,177,164,40,36,45,54,231,20,57,226,123,19,194,165,37,72,242,224,160,136,11,234,
    112,74,220,21,56,71,192,127,106,245,64,106,248,149,39,166,60,5,61,82,176,42,164,92,
    53,39,93,21,145,66,202,171,170,55,238,84,51,89,56,159,162,72,18,126,9,63,149,211,
    92,48,66,65,197,141,135,66,115,106,174,12,221,222,52,95,230,245,240,217,65,3,25,110,
    70,213,18,144,225,42,98,150,112,142,192,170,22,140,132,112,122,11,38,235,244,249,160,68,
    33,150,219,178,5,79,44,42,205,245,160,188,10,141,146,172,113,43,20,194,122,221,125,86,
    165,5,202,77,161,196,243,4,94,140,5,83,76,4,69,117,68,23,11,108,158,84,112,213,
    254,116,129,42,155,221,36,176,239,22,0,13,61,223,187,13,57,103,65,234,119,166,180,146,
    90,111,39,192,213,138,198,101,156,221,207,226,29,22,34,156,5,134,40,81,80,66,232,179,
    156,96,97,167,171,114,208,49,17,92,47,232,110,176,155,146,169,211,245,174,114,194,55,212,
    41,156,12,20,68,234,249,35,83,21,107,71,196,172,162,98,76,131,236,97,122,80,115,225,
    81,166,185,227,189,55,99,82,127,123,159,44,27,225,89,172,122,230,113,155,109,229,14,73,
    4,129,154,32,158,176,45,164,73,69,160,113,78,73,43,8,170,19,6,138,251,14,105,44,
    177,252,45,94,2,202,123,80,20,245,32,40,105,140,232,250,206,132,83,17,42,87,133,74,
    176,64,192,202,39,233,106,55,224,213,53,26,25,149,45,105,122,205,85,250,92,129,238,185,
    29,74,239,154,208,253,7,100,53,60,171,133,237,138,230,246,59,146,201,12,104,108,218,196,
    212,12,204,38,237,9,20,15,80,17,50,12,148,73,20,207,51,73,163,164,253,131,182,53,
    195,80,234,177,244,10,128,100,35,145,72,93,59,177,156,56,231,246,248,171,73,57,170,187,
    158,72,57,203,228,13,181,43,146,145,24,168,72,152,227,155,34,208,5,55,81,129,198,110,
    132,218,5,64,127,121,252,187,21,129,198,220,165,235,1,77,214,105,198,17,171,15,167,146,
    203,119,44,207,74,225,30,53,69,28,41,71,36,154,209,22,24,40,205,100,64,132,25,161,
    2,202,220,161,131,50,243,69,10,165,179,33,54,215,67,234,20,0,77,226,164,66,151,37,
    216,190,84,61,47,76,136,15,106,192,65,120,157,82,188,112,170,81,247,68,202,49,7,86,
    13,234,9,212,128,210,5,138,149,154,11,143,183,201,46,85,44,60,50,178,119,231,8,172,
    108,54,149,114,49,83,169,84,54,57,178,101,249,142,29,59,30,124,232,17,240,168,42,40,
    46,177,152,64,99,224,123,92,99,148,232,193,161,95,194,46,130,110,251,7,8,34,167,150,
    234,235,41,122,203,100,125,80,164,164,64,71,7,71,113,210,193,114,28,43,78,166,154,170,
    64,117,215,23,41,65,145,214,133,51,134,47,82,97,42,168,185,249,168,39,208,111,96,211,
    168,38,116,124,92,118,153,110,250,204,139,144,63,61,50,4,11,210,12,199,93,152,114,56,
    233,116,129,36,8,152,190,33,102,135,196,44,151,192,118,44,205,86,24,40,36,236,216,218,
    172,128,142,250,155,99,130,141,231,155,107,247,143,217,214,65,229,109,76,251,171,65,69,189,
    192,98,144,101,89,108,155,60,184,247,68,207,107,101,59,131,7,63,212,152,205,242,140,212,
    164,34,13,26,169,185,230,48,75,232,141,62,54,88,7,206,232,83,12,84,79,189,177,36,
    40,121,186,252,96,167,112,54,166,34,240,103,82,76,129,66,229,240,131,225,120,185,36,97,
    17,155,237,251,77,1,21,152,151,196,195,76,217,62,87,198,64,202,172,230,101,62,28,87,
    8,3,168,33,118,185,126,151,76,241,186,12,148,182,156,82,238,75,68,16,105,164,209,211,
    93,26,99,170,189,209,238,20,19,232,9,38,80,0,189,165,207,83,221,174,23,241,39,189,
    60,202,197,244,78,186,109,98,201,105,168,153,34,24,166,141,110,6,55,23,8,84,223,180,
    199,233,237,111,122,160,205,190,230,130,238,226,219,132,147,158,153,130,242,26,49,77,240,204,
    143,122,23,4,5,87,60,90,73,133,166,128,42,46,168,251,198,156,4,37,22,95,183,197,
    244,72,221,240,226,11,116,49,243,68,186,222,116,59,155,148,172,9,157,223,59,223,115,70,
    135,139,210,20,153,122,199,198,161,218,226,229,103,230,16,21,40,246,222,177,28,38,245,10,
    221,251,19,226,126,36,80,112,3,95,79,86,164,66,91,246,92,22,146,31,47,120,218,49,
    131,208,55,245,136,226,232,118,65,95,92,19,116,69,26,148,43,28,9,107,151,166,220,55,
    21,85,48,208,59,139,174,238,130,153,6,5,106,14,121,138,187,253,248,133,161,235,221,6,
    246,241,3,34,75,23,98,123,23,152,202,84,82,247,211,220,235,86,213,161,93,160,117,84,
    54,49,234,114,98,120,180,1,78,6,170,71,130,160,113,58,127,1,217,41,209,124,229,117,
    93,50,243,205,92,60,67,85,87,172,117,77,144,38,199,83,65,57,10,234,158,11,108,108,
    128,72,239,112,175,255,147,170,227,232,216,110,215,189,62,210,123,81,136,237,131,215,204,9,
    157,183,71,117,243,34,125,179,114,154,72,105,208,39,76,69,32,11,217,34,106,72,67,232,
    249,13,219,205,6,71,226,130,228,79,32,76,7,202,97,128,225,56,93,212,219,93,153,10,
    68,11,170,54,230,175,186,129,160,162,158,97,102,109,75,83,68,234,131,186,121,160,34,97,
    254,199,255,74,2,77,51,209,124,170,42,151,221,44,201,213,195,158,226,82,208,159,55,237,
    139,185,39,96,239,2,73,153,226,143,216,36,129,103,10,91,164,176,6,231,59,206,44,38,
    101,103,37,204,95,227,149,28,199,5,133,140,34,16,27,104,108,233,6,12,38,83,13,119,
    217,188,30,16,173,100,193,176,105,134,46,130,67,159,94,164,232,141,99,1,221,197,118,161,
    97,200,117,24,171,108,86,239,86,64,251,88,100,9,199,250,60,197,69,213,157,19,122,223,
    126,207,72,245,23,57,105,26,80,34,186,156,221,25,35,203,209,236,142,158,222,102,4,13,
    112,194,1,82,81,71,116,49,28,148,40,77,23,136,129,222,198,1,76,252,195,226,42,137,
    51,53,192,87,177,171,218,244,172,192,222,103,138,149,162,102,177,237,184,176,235,141,76,19,
    164,41,119,193,51,40,111,33,32,81,215,17,133,99,11,104,78,84,1,189,173,233,70,79,
    164,35,155,3,34,245,2,12,145,60,129,14,73,224,222,227,240,31,161,158,129,216,221,16,
    65,227,1,39,153,70,175,43,234,83,64,5,212,93,82,15,78,59,124,151,67,52,141,201,
    140,243,71,169,8,196,76,221,189,126,47,229,234,114,80,164,132,238,139,187,222,40,198,132,
    46,152,81,5,116,55,210,5,5,2,241,64,171,57,31,105,249,170,207,137,206,104,78,232,
    185,61,158,72,99,155,31,169,144,122,18,245,21,183,59,131,159,2,152,174,26,217,0,42,
    197,3,165,9,231,48,80,248,83,5,26,79,160,59,66,208,176,158,161,168,172,162,33,30,
    168,197,197,220,171,79,35,244,52,224,240,67,133,20,127,202,7,245,252,46,84,195,64,42,
    55,198,20,169,10,116,137,199,153,109,249,112,96,6,159,130,222,182,226,68,204,203,108,251,
    132,169,18,37,94,176,18,135,20,240,121,4,37,72,232,185,37,36,108,86,13,184,81,183,
    67,65,35,217,170,90,171,76,219,179,162,161,135,13,61,85,128,18,85,161,91,158,46,40,
    62,84,188,75,50,195,254,25,243,179,64,124,177,132,142,59,168,187,2,238,204,136,50,255,
    4,105,38,30,39,144,46,241,236,51,60,240,88,40,116,125,213,214,62,136,244,106,47,137,
    143,141,44,153,146,28,113,82,64,160,26,1,73,16,247,51,195,182,34,249,82,97,171,68,
    227,78,152,130,6,227,189,85,134,127,130,155,210,117,200,88,245,108,161,128,249,191,69,168,
    172,4,170,169,156,238,138,84,167,34,77,219,83,68,42,112,174,219,53,194,26,123,6,106,
    120,136,122,242,214,118,42,14,70,90,236,27,241,56,59,175,186,48,192,233,130,254,85,175,
    239,143,98,71,55,123,193,148,73,212,243,68,162,184,69,169,39,28,237,22,16,234,23,234,
    5,69,227,56,18,84,221,50,158,251,72,216,23,76,229,27,224,143,192,29,193,57,192,34,
    36,18,46,36,44,66,85,214,7,77,122,151,18,119,83,25,67,85,80,9,49,113,55,219,
    21,105,50,18,118,181,217,4,157,21,121,126,57,134,22,150,22,13,245,101,125,206,150,247,
    6,57,89,3,123,78,232,251,77,91,61,229,141,237,119,99,140,219,84,195,152,73,27,241,
    221,203,235,147,205,130,101,249,231,22,84,119,10,40,153,9,84,136,175,199,232,201,64,225,
    191,136,134,153,59,177,16,23,206,64,2,234,207,130,39,82,250,74,40,33,136,34,249,175,
    102,69,60,54,155,192,48,194,174,178,72,16,196,99,170,172,103,92,143,43,44,222,92,239,
    235,237,20,78,183,83,63,39,244,244,110,191,4,141,109,102,164,254,220,18,248,127,27,80,
    123,52,13,252,64,2,13,70,99,91,244,54,142,220,84,129,174,199,88,78,65,245,41,201,
    170,179,158,70,152,48,11,134,224,41,9,65,80,236,228,186,22,217,45,6,117,215,161,25,
    23,231,118,150,226,30,40,172,152,7,202,65,166,0,217,189,156,4,129,106,138,178,110,201,
    136,23,60,178,77,87,77,225,244,65,255,234,248,230,74,249,13,164,1,80,44,93,56,112,
    226,203,149,20,48,59,116,227,192,5,213,132,41,160,121,60,50,61,236,123,79,191,65,32,
    112,37,7,29,175,238,129,178,248,10,222,56,39,81,80,248,166,43,210,8,5,1,80,66,
    219,80,244,229,241,120,16,84,113,11,114,65,194,1,36,57,210,12,71,153,89,112,83,216,
    15,146,45,213,246,25,24,214,152,19,170,105,58,90,77,234,213,49,52,146,66,104,216,66,
    168,64,157,28,128,210,249,98,136,149,212,45,84,129,226,231,83,80,177,187,186,204,18,172,
    172,195,250,33,21,80,156,192,224,36,23,84,16,186,69,150,30,33,8,62,69,232,247,220,
    57,81,4,149,24,168,46,185,70,202,73,26,102,225,114,150,203,236,216,156,117,183,189,99,
    177,163,45,143,253,110,42,167,191,201,4,41,111,211,136,31,99,98,55,45,86,2,18,197,
    205,132,98,143,118,23,164,185,235,51,236,3,241,96,83,20,212,74,84,112,236,60,126,126,
    44,92,9,19,126,155,64,32,221,17,56,248,250,152,120,26,40,193,60,3,207,133,46,70,
    232,9,196,143,76,219,26,29,137,12,130,154,46,104,204,5,133,40,197,68,250,226,230,173,
    238,188,6,24,233,18,200,19,66,51,207,2,254,62,244,211,171,183,86,72,71,150,20,165,
    0,40,177,39,51,40,80,107,61,157,43,165,37,56,3,21,210,78,165,78,177,55,225,231,
    43,20,84,15,130,98,241,158,21,245,66,156,24,213,160,240,102,240,48,195,126,164,59,2,
    169,61,120,27,48,17,206,122,85,11,144,18,38,81,181,26,148,163,219,167,162,202,143,28,
    172,15,167,112,179,41,22,27,121,160,245,34,186,107,54,227,208,227,117,161,247,5,73,83,
    155,135,2,185,145,214,220,67,45,180,92,150,192,98,220,188,148,86,76,130,83,5,74,245,
    152,130,178,166,144,59,93,140,160,26,148,214,89,66,55,44,12,67,112,199,202,33,238,43,
    66,198,235,118,119,211,235,249,35,8,154,198,81,122,90,234,50,35,13,130,234,30,40,58,
    67,206,0,145,222,133,205,45,16,103,44,147,191,228,115,161,107,206,56,198,10,39,225,233,
    101,217,10,169,126,116,71,209,45,190,225,35,123,50,90,24,156,199,250,56,206,0,137,65,
    208,114,0,212,233,161,160,41,195,51,210,192,32,163,192,233,244,53,132,6,9,119,126,30,
    247,169,36,169,32,20,112,130,37,103,10,216,105,136,24,160,187,74,186,157,129,114,82,206,
    117,187,96,64,83,65,89,165,175,170,124,23,128,30,172,79,45,239,225,165,225,255,115,182,
    193,100,36,13,200,20,132,122,211,130,12,219,219,86,184,73,146,130,136,0,154,155,155,2,
    202,173,15,130,102,104,233,153,52,60,221,13,142,108,114,245,248,100,44,70,115,86,118,144,
    32,81,201,148,148,140,148,177,113,151,81,149,178,80,151,70,12,189,27,140,111,57,187,80,
    128,105,175,16,4,53,34,21,80,244,190,144,241,234,169,250,131,201,155,86,246,203,50,159,
    255,227,208,191,157,101,48,25,73,3,30,9,52,97,100,193,130,130,82,175,113,74,218,73,
    166,224,252,150,19,168,73,46,168,74,65,201,250,74,203,86,40,21,209,173,112,54,11,20,
    90,53,168,160,81,80,81,53,36,116,34,120,93,86,194,98,27,193,88,213,130,22,170,124,
    174,27,73,245,176,164,144,66,42,72,74,165,234,131,86,66,23,130,98,46,190,51,249,98,
    231,100,62,45,203,114,255,199,67,255,235,108,19,216,115,66,161,159,54,221,20,211,3,189,
    249,212,242,29,25,120,179,73,224,228,226,4,52,151,230,172,12,84,71,40,171,100,85,162,
    101,73,73,211,171,232,52,213,243,187,149,129,77,248,26,113,227,135,106,40,108,99,204,73,
    99,102,146,230,164,162,141,91,221,188,220,64,205,84,79,1,91,127,50,25,32,141,251,160,
    70,189,17,232,9,199,29,48,84,213,144,183,238,232,176,136,117,204,148,101,115,229,249,161,
    255,119,182,81,243,155,175,12,189,118,247,139,225,88,112,27,34,165,109,121,232,33,39,165,
    104,113,206,42,163,111,16,124,80,108,19,56,9,171,210,4,112,164,12,253,23,241,79,68,
    149,238,134,25,40,164,171,204,233,38,58,210,146,100,42,233,156,185,165,93,83,178,91,186,
    187,35,82,49,11,174,183,17,158,237,23,124,80,14,219,227,158,234,98,22,25,72,70,176,
    181,47,26,252,43,73,98,219,54,21,233,196,87,167,243,70,83,39,199,174,12,213,92,53,
    176,197,23,106,253,65,88,225,212,142,59,193,76,227,152,153,155,216,17,15,74,180,100,7,
    64,211,146,144,166,160,6,21,105,146,11,140,97,8,52,201,115,65,89,209,9,201,18,128,
    102,50,38,15,82,85,184,162,40,182,237,202,110,105,212,245,186,152,41,100,114,69,58,203,
    201,222,60,71,251,243,212,155,235,85,123,113,14,36,197,166,104,200,97,4,117,142,241,178,
    60,244,216,108,64,65,125,255,239,251,90,150,176,238,47,110,43,29,60,152,10,39,39,147,
    232,1,49,101,55,43,170,43,234,73,56,203,101,98,87,182,13,210,57,6,202,116,151,26,
    113,80,164,154,168,210,22,24,171,180,0,52,141,62,55,205,69,229,104,63,134,68,30,175,
    186,148,184,174,58,93,55,179,217,168,144,205,250,50,165,65,202,84,225,36,197,170,64,5,
    48,157,184,36,234,106,173,29,183,109,114,73,81,150,199,32,159,255,230,44,174,146,0,67,
    125,254,170,38,72,28,99,225,131,245,74,253,65,136,79,91,182,0,39,17,44,7,65,3,
    217,119,18,63,199,111,206,10,66,186,40,48,80,218,32,18,213,218,209,106,43,213,34,106,
    132,10,133,53,9,156,116,90,49,205,98,191,196,243,0,42,17,129,221,87,48,199,165,244,
    176,153,221,37,155,28,156,223,102,173,114,145,12,77,54,112,172,69,241,187,44,2,7,182,
    201,1,40,159,180,0,180,220,33,203,13,3,223,152,198,72,167,153,238,156,3,130,255,234,
    21,75,110,74,214,199,234,129,50,149,74,45,87,56,13,10,97,39,46,84,129,70,146,104,
    34,90,0,148,203,153,253,212,9,199,169,212,213,148,191,79,207,70,229,117,85,15,128,150,
    50,105,8,46,105,1,239,68,26,0,229,77,33,155,50,179,81,153,151,138,168,73,21,82,
    31,84,8,244,205,202,22,103,153,221,162,28,78,16,27,220,81,84,150,215,78,103,164,211,
    128,94,143,207,253,229,131,15,172,49,187,65,117,129,115,103,82,227,154,9,246,73,4,170,
    186,89,23,212,96,218,92,113,186,142,96,242,2,221,128,112,155,93,145,81,218,146,245,149,
    55,44,6,64,173,52,216,52,181,80,88,25,34,17,201,5,229,163,66,210,204,66,73,173,
    230,178,89,165,185,80,113,61,20,20,94,31,104,10,67,102,6,94,170,59,203,215,218,22,
    144,230,65,119,7,31,155,13,232,156,127,13,133,190,248,254,137,124,81,53,185,158,201,155,
    70,82,225,174,81,141,67,177,197,113,11,45,0,170,198,8,103,57,65,80,48,57,222,236,
    231,114,56,46,157,197,123,173,169,141,25,127,106,158,208,250,68,119,117,30,86,71,177,8,
    111,151,206,169,200,86,4,137,154,30,40,47,101,204,108,74,68,53,78,38,129,148,248,65,
    51,70,65,37,58,218,237,130,166,203,0,106,222,105,240,186,101,19,226,56,188,188,240,170,
    208,23,207,10,10,202,125,254,199,30,236,73,11,197,142,149,3,3,157,77,3,15,60,132,
    30,161,144,102,165,6,77,200,60,137,130,165,148,2,53,154,66,250,133,40,175,10,253,216,
    108,158,220,17,166,19,6,79,226,150,162,164,184,87,98,105,17,6,218,204,138,1,16,40,
    120,34,138,134,227,141,13,62,104,67,209,44,70,241,238,186,170,146,132,143,207,20,108,175,
    65,172,80,19,37,201,202,134,34,7,165,161,16,93,222,8,167,199,130,220,173,44,202,107,
    90,254,225,108,94,247,154,247,132,126,254,216,155,61,69,226,172,28,94,183,107,254,83,60,
    255,200,170,117,146,166,20,30,244,186,231,152,165,120,221,29,194,154,97,21,208,34,74,7,
    142,29,175,195,47,102,49,61,82,195,119,209,89,70,156,251,195,236,135,129,82,167,11,153,
    99,127,20,188,27,187,177,41,134,15,213,7,229,139,74,6,190,162,241,74,232,122,185,194,
    100,218,19,43,148,78,156,22,11,244,182,177,195,104,100,46,5,5,192,86,120,73,145,23,
    181,254,252,116,183,91,61,175,27,10,93,222,58,97,154,229,77,43,215,177,235,12,218,38,
    50,34,145,184,135,38,89,203,25,15,154,19,146,17,207,68,173,96,99,76,65,205,133,8,
    145,238,1,9,18,46,151,141,224,6,92,99,150,99,211,157,138,96,19,69,167,47,197,234,
    196,41,230,114,130,34,184,118,41,99,34,82,225,228,149,44,231,62,66,59,213,184,177,59,
    108,199,97,42,108,217,74,92,9,236,34,43,144,50,16,83,218,121,153,42,71,81,164,28,
    223,214,250,141,211,141,52,8,250,157,208,127,253,216,155,67,114,195,250,85,75,230,179,201,
    228,40,216,54,232,225,242,145,130,63,86,96,250,160,96,162,137,170,190,80,26,52,151,143,
    22,51,67,40,210,140,36,80,210,8,146,186,93,69,155,11,71,168,137,194,67,56,43,38,
    60,239,73,113,17,23,48,81,80,139,162,239,153,84,72,150,180,98,65,33,237,169,112,54,
    75,146,153,140,77,130,219,229,74,193,66,35,237,190,75,226,101,5,172,148,136,102,235,71,
    79,143,47,53,193,164,232,252,171,134,23,201,234,166,55,23,120,215,12,244,164,193,3,114,
    147,141,59,169,39,240,64,51,110,206,30,39,73,214,93,118,247,128,33,80,200,102,70,226,
    115,105,83,202,97,18,213,141,164,97,108,24,176,93,75,161,61,89,75,131,14,228,236,153,
    92,148,139,70,61,78,121,140,35,1,205,133,172,161,33,64,205,101,147,163,156,141,177,57,
    22,131,84,152,120,115,74,244,115,149,76,2,210,93,51,27,110,54,100,94,0,145,74,226,
    192,69,167,123,163,154,32,103,203,132,42,203,29,173,11,189,171,6,134,28,85,20,226,153,
    112,99,59,155,18,230,232,156,60,71,194,20,148,227,50,41,119,8,136,133,185,254,156,186,
    107,23,186,146,151,185,40,164,225,170,137,50,85,197,176,222,237,141,27,115,59,89,102,168,
    16,40,205,64,209,43,108,99,36,168,185,124,54,40,94,62,154,43,110,209,50,10,132,81,
    220,139,138,217,144,71,74,130,127,215,130,12,234,85,52,171,143,18,83,86,5,28,44,131,
    64,58,51,232,149,161,63,187,106,2,12,179,191,194,185,46,31,5,79,182,73,170,221,9,
    138,227,137,45,7,78,37,165,82,205,37,89,220,161,32,130,59,186,5,1,116,44,74,111,
    155,108,98,106,23,55,49,242,235,170,26,9,71,124,82,218,31,76,74,164,0,53,110,80,
    130,16,94,130,104,81,169,234,155,124,67,191,211,76,192,11,41,88,210,106,4,65,185,202,
    237,25,40,104,183,158,180,136,42,171,156,21,87,215,126,248,12,160,53,161,67,195,192,105,
    190,190,221,227,220,182,73,0,197,205,119,136,122,73,11,12,22,152,2,73,226,116,138,194,
    17,93,39,116,24,151,102,129,196,201,140,121,7,150,75,75,160,242,112,128,57,37,28,81,
    245,176,17,86,124,80,208,92,173,152,41,104,153,42,153,245,87,161,69,171,57,33,113,42,
    20,48,1,108,70,129,18,129,130,10,62,40,222,59,199,84,116,72,141,192,113,131,76,165,
    23,14,205,28,94,174,12,93,212,10,217,147,124,201,128,127,137,237,4,39,139,66,62,47,
    168,201,18,90,68,193,77,70,64,105,104,124,33,241,20,70,126,236,129,208,129,38,107,50,
    19,245,15,44,199,73,81,18,165,233,92,183,78,183,17,21,211,147,104,146,203,130,128,147,
    182,41,251,32,242,216,216,20,180,41,160,233,2,246,37,52,133,250,122,193,149,40,59,245,
    154,98,209,146,53,108,100,226,28,170,137,36,12,63,54,163,215,189,62,244,103,45,235,128,
    110,81,171,127,37,218,4,228,24,82,126,165,196,215,182,39,176,175,94,240,166,78,36,90,
    110,198,226,54,164,41,89,10,138,70,42,164,169,211,245,93,37,232,106,142,106,157,41,225,
    30,128,217,157,85,160,200,4,239,164,55,23,4,41,202,139,90,177,194,166,142,169,103,6,
    237,73,10,146,161,96,87,78,99,160,177,41,160,138,148,50,186,45,44,112,100,94,157,215,
    114,219,105,164,46,232,53,161,15,15,96,228,92,59,232,113,110,132,106,157,143,175,106,131,
    35,115,8,14,85,20,188,237,30,73,32,16,36,148,56,94,219,145,114,175,63,195,190,45,
    205,206,3,102,38,185,255,110,64,214,176,41,101,50,2,130,66,16,117,168,48,165,172,58,
    147,170,78,93,209,229,18,23,131,202,130,166,157,184,43,39,86,58,158,154,98,83,80,205,
    48,18,88,155,71,33,148,119,180,92,28,250,215,233,85,247,194,175,45,148,121,185,173,117,
    190,167,184,43,193,11,74,155,218,100,67,205,38,176,29,105,101,252,13,60,41,30,6,13,
    194,107,52,140,48,161,155,176,14,118,35,211,66,181,20,26,114,66,229,113,20,52,55,87,
    28,129,106,93,193,43,105,37,252,229,9,226,217,248,42,154,13,138,175,112,118,189,74,211,
    249,106,80,69,67,80,77,2,135,140,25,133,41,66,144,227,59,32,57,170,153,6,244,154,
    208,69,45,136,184,250,5,79,160,131,101,240,253,235,139,50,132,135,66,9,65,211,190,68,
    33,148,38,193,70,176,6,142,233,180,123,199,149,240,174,53,233,220,212,3,52,165,32,56,
    176,118,171,98,178,144,41,114,90,177,56,22,157,21,34,189,1,146,73,36,220,124,72,138,
    108,84,95,114,91,140,94,15,155,129,106,134,88,68,37,86,163,28,132,153,142,85,31,168,
    110,6,122,160,63,185,7,53,119,96,220,243,184,171,36,81,22,250,101,89,229,195,175,150,
    208,28,74,105,206,155,58,16,114,182,238,22,193,250,168,128,38,154,79,67,105,18,151,166,
    49,174,170,127,137,58,175,11,92,177,88,20,166,149,165,170,54,52,52,4,12,23,212,32,
    151,51,205,113,147,245,1,109,214,43,154,30,148,147,178,49,177,59,129,129,158,143,198,5,
    144,233,112,77,21,169,7,250,216,66,21,52,183,197,187,244,119,137,35,154,106,6,207,167,
    154,124,117,18,155,138,249,180,63,52,140,183,189,144,104,191,61,22,203,210,139,69,243,69,
    33,55,45,104,245,138,68,68,193,140,210,53,213,249,168,38,189,22,196,52,43,223,81,241,
    231,204,117,3,235,64,111,53,142,148,178,94,25,106,48,80,183,181,161,216,20,84,73,198,
    12,157,166,164,160,73,171,134,100,217,1,210,247,156,102,163,191,187,255,164,200,203,107,188,
    216,178,109,101,212,140,154,244,183,38,212,22,146,203,177,42,216,20,0,197,18,216,5,85,
    232,22,125,7,117,81,103,5,213,35,166,57,131,183,201,9,82,67,181,26,224,113,52,140,
    127,162,101,16,47,157,39,182,99,199,188,70,134,11,202,166,97,20,37,193,64,33,8,68,
    208,72,137,36,139,131,159,134,28,47,189,234,163,1,82,23,244,194,150,147,209,168,188,240,
    30,79,160,61,170,41,209,223,165,195,135,157,48,142,30,199,143,57,21,80,40,11,35,236,
    150,61,245,138,130,85,77,7,94,214,203,157,13,84,13,139,51,24,38,186,202,224,130,167,
    230,47,90,115,100,113,231,192,11,253,146,82,76,231,59,172,184,223,103,16,196,138,68,53,
    78,105,102,160,26,70,187,2,149,50,207,247,183,116,174,146,228,226,166,63,174,144,122,170,
    123,213,186,168,36,175,118,65,231,191,46,69,93,129,170,201,246,176,131,78,119,147,7,26,
    167,176,58,3,213,36,186,5,238,8,232,85,207,6,170,139,51,71,203,182,147,191,94,184,
    113,227,198,151,94,122,9,190,46,220,190,184,239,234,166,107,91,154,58,23,15,46,233,28,
    216,208,218,186,225,227,107,119,120,131,177,180,29,165,179,41,66,141,139,217,9,119,10,13,
    194,64,6,231,161,33,229,53,91,199,79,94,210,207,143,229,255,75,232,3,83,64,15,189,
    96,74,234,144,11,186,48,47,154,192,9,25,62,248,220,84,10,65,157,245,101,31,52,206,
    28,2,170,174,34,101,155,221,2,77,144,204,51,71,139,51,228,4,209,254,75,90,91,216,
    106,106,234,236,219,191,249,106,92,77,77,215,94,219,210,114,237,6,92,19,25,127,128,76,
    98,251,62,116,76,88,147,36,80,106,252,134,150,132,64,154,97,27,225,60,63,177,90,158,
    255,96,158,95,84,33,245,64,31,109,1,55,177,206,181,209,181,253,162,25,149,163,244,82,
    181,118,93,195,113,2,167,180,158,171,136,212,51,82,140,225,97,218,45,80,33,45,158,198,
    254,100,234,82,26,206,102,188,170,186,184,229,90,88,77,87,95,221,9,107,79,159,11,218,
    212,52,128,242,92,59,220,47,40,62,168,81,1,85,52,67,179,75,20,52,3,101,26,130,
    162,72,163,114,113,21,47,171,195,121,121,44,255,1,151,212,3,253,69,75,79,212,28,99,
    249,66,219,202,156,41,96,130,97,170,70,248,50,131,130,150,158,205,147,42,80,220,185,86,
    112,56,21,135,225,28,1,163,29,167,158,238,99,198,138,112,108,230,217,50,3,249,215,173,
    27,40,40,35,133,255,225,111,148,104,75,75,235,134,193,177,151,59,4,111,202,94,96,190,
    200,5,213,20,195,182,243,84,117,147,122,50,22,75,186,151,44,200,13,171,198,192,2,91,
    127,36,103,242,23,50,82,63,5,188,232,7,156,41,173,92,205,52,215,132,234,80,134,194,
    88,21,179,163,6,193,185,137,114,98,189,229,205,237,113,110,122,162,208,17,79,108,99,59,
    52,87,16,170,51,134,6,115,209,24,69,148,207,154,23,252,209,199,91,61,197,69,86,38,
    205,129,206,129,181,139,127,189,122,237,37,47,119,252,136,229,181,180,33,198,198,35,179,4,
    204,81,209,76,206,182,55,209,218,41,171,167,40,168,77,183,126,228,60,230,178,107,90,138,
    178,243,151,204,33,121,121,210,249,161,199,86,246,155,197,85,69,204,138,210,2,167,130,128,
    4,213,16,71,92,208,245,137,146,187,15,234,77,138,25,146,22,115,199,220,5,71,96,177,
    48,40,184,6,166,177,24,35,214,45,58,75,26,180,104,108,209,216,248,201,141,11,127,189,
    125,251,226,197,75,150,44,89,188,120,245,234,23,54,172,94,20,109,80,133,248,216,88,7,
    155,201,22,42,160,70,18,137,160,246,75,16,135,170,26,9,235,225,122,10,202,34,204,216,
    235,152,18,220,243,186,169,230,153,235,173,241,247,70,255,219,135,135,29,33,189,106,112,108,
    221,176,5,177,197,164,91,29,226,72,115,140,224,140,87,57,145,152,98,164,28,157,1,195,
    145,181,140,11,202,171,211,152,232,248,246,13,45,173,159,141,158,45,211,243,195,10,228,67,
    96,212,13,42,102,140,144,92,152,96,249,57,135,238,236,187,173,92,58,131,99,36,81,160,
    138,153,177,185,201,229,136,150,129,10,73,3,80,14,79,0,129,8,179,10,127,115,197,182,
    150,50,63,182,234,102,76,240,107,2,29,192,154,63,190,36,159,31,30,62,150,54,85,106,
    251,216,130,30,133,226,0,65,75,207,150,104,202,32,184,151,201,121,160,72,122,23,113,138,
    167,103,125,24,133,183,125,162,19,244,241,218,214,197,209,179,171,47,155,142,111,104,136,250,
    11,82,64,19,190,52,244,199,9,231,115,106,84,115,35,205,40,80,179,8,169,208,29,20,
    52,9,160,89,58,157,141,243,225,160,187,67,52,109,239,28,230,248,137,143,85,129,134,206,
    167,221,135,191,236,40,115,160,131,116,54,2,27,38,163,138,148,248,45,110,224,247,76,172,
    47,11,108,206,203,75,50,25,105,44,22,182,173,151,153,252,162,57,137,249,29,252,245,83,
    227,219,129,18,93,204,181,27,90,63,190,104,54,41,188,15,10,162,204,225,87,250,197,116,
    8,241,118,88,65,69,217,254,22,10,212,196,205,198,204,19,237,0,106,55,226,181,72,245,
    108,12,29,47,233,128,80,138,186,251,82,107,66,26,107,189,152,94,155,86,149,14,254,207,
    188,3,39,63,74,77,130,110,187,182,43,217,73,72,172,200,37,11,38,18,101,8,161,216,
    28,19,220,81,228,152,11,106,220,101,165,209,66,215,13,21,209,87,243,234,252,182,241,133,
    24,242,113,93,139,177,112,195,11,39,171,53,122,102,80,202,105,226,159,28,251,18,141,91,
    244,142,14,236,58,134,81,214,106,13,219,56,243,232,64,209,50,249,36,94,58,212,222,8,
    213,125,234,224,168,59,208,109,43,188,60,240,18,222,176,170,165,108,69,233,166,83,0,244,
    223,66,53,127,81,30,131,131,144,233,173,176,56,186,153,219,158,129,140,199,225,172,158,71,
    242,150,85,194,121,31,220,79,11,90,41,128,234,237,101,21,2,230,186,13,247,44,94,188,
    120,207,61,16,27,118,111,222,223,215,137,1,2,35,68,231,198,182,89,120,222,128,64,1,
    49,215,128,95,41,114,154,222,167,65,162,214,84,232,114,231,180,9,114,98,119,227,142,174,
    2,176,61,65,65,83,238,213,136,144,230,171,110,154,215,57,108,75,67,143,133,190,25,176,
    209,247,132,46,31,46,131,56,105,195,155,163,3,211,80,224,254,170,40,74,120,79,133,244,
    142,146,109,151,48,43,146,76,193,19,169,175,188,201,99,38,111,190,60,184,1,35,4,4,
    67,64,236,235,99,145,176,229,218,237,139,78,163,84,207,12,106,82,153,122,134,42,36,232,
    125,14,20,168,26,56,114,39,211,92,81,35,192,137,82,117,186,186,208,251,92,218,21,137,
    132,83,201,36,11,67,9,52,210,177,22,244,113,71,86,18,82,108,57,63,116,77,77,197,
    23,125,116,56,13,81,31,80,45,28,178,38,10,110,47,135,11,170,40,25,25,129,203,223,
    71,108,219,178,112,86,13,178,90,193,221,138,240,175,78,124,240,101,57,186,186,117,195,192,
    128,23,7,105,204,191,182,105,241,201,168,44,243,179,92,83,124,145,187,230,197,217,4,38,
    26,232,232,101,212,21,169,10,49,53,7,39,122,165,77,141,59,9,103,103,27,27,69,61,
    220,53,202,174,107,96,215,129,139,173,120,87,155,31,182,130,129,55,61,234,131,2,231,135,
    135,115,178,234,8,178,12,20,232,185,98,42,56,212,203,56,94,148,196,66,90,41,97,179,
    223,182,113,204,31,239,219,226,6,83,233,96,61,94,191,26,51,70,126,116,114,67,43,164,
    107,27,48,187,105,194,236,6,254,186,7,84,118,70,241,205,4,58,133,180,161,223,242,238,
    31,13,174,245,206,48,221,73,135,66,41,131,193,93,49,156,75,186,182,112,164,16,70,208,
    198,198,59,169,199,165,87,165,136,34,191,118,35,128,126,164,21,132,220,121,145,7,10,156,
    143,173,138,202,66,222,129,252,214,162,215,103,16,172,79,52,45,35,2,168,84,82,80,155,
    17,212,162,34,53,189,75,64,164,203,234,49,194,116,47,104,133,10,3,22,21,41,168,236,
    181,157,139,55,46,106,160,17,131,127,139,160,85,164,13,57,203,191,161,136,149,12,95,230,
    114,42,5,11,142,81,51,149,135,238,219,9,69,100,74,15,55,138,225,46,0,229,88,178,
    139,125,74,158,26,105,91,11,252,115,237,237,30,40,230,69,209,6,167,127,30,168,181,98,
    209,235,3,238,3,208,24,169,47,72,188,33,74,15,18,188,191,5,206,125,144,15,162,149,
    34,41,189,183,135,162,216,41,148,233,174,71,150,44,94,187,182,115,67,203,134,13,157,157,
    173,3,171,79,154,13,232,65,27,222,42,232,20,210,134,104,220,231,180,147,225,39,241,102,
    21,162,194,117,143,210,196,64,50,123,186,238,91,110,67,246,23,214,187,34,225,174,174,59,
    81,237,216,117,86,240,131,253,27,80,162,45,112,148,19,30,232,53,161,175,182,154,99,19,
    170,35,193,183,240,71,45,171,99,65,68,140,104,90,125,115,134,23,85,103,75,129,75,82,
    208,4,177,202,212,74,161,232,151,240,39,5,237,213,187,232,109,167,146,81,62,186,22,178,
    54,115,209,224,175,77,55,76,96,130,115,38,210,169,230,235,145,122,176,13,120,47,46,198,
    73,236,187,244,39,239,162,155,192,154,17,105,198,32,162,136,233,59,26,239,43,17,200,254,
    116,189,11,132,58,50,90,112,65,137,98,138,170,208,250,138,44,143,183,192,57,122,224,195,
    161,223,215,176,140,190,117,172,216,33,143,57,120,19,17,84,10,242,236,162,54,93,141,52,
    215,199,154,211,170,170,36,113,95,35,5,31,150,72,32,169,69,73,185,184,132,138,162,104,
    175,94,150,2,45,143,36,163,11,215,69,225,64,23,153,170,47,144,25,64,225,83,176,13,
    22,109,91,215,38,79,147,27,121,172,152,183,228,112,215,70,225,236,2,168,231,147,112,70,
    37,137,14,10,163,41,26,197,188,221,117,7,28,24,94,61,217,213,216,216,152,108,102,160,
    196,178,21,209,84,77,244,70,159,106,173,87,200,218,91,92,137,254,195,85,11,138,67,50,
    159,111,0,80,220,31,39,165,162,120,99,88,173,111,142,25,138,99,138,156,161,235,5,171,
    57,75,44,0,181,128,180,140,215,75,153,196,194,157,53,36,253,113,87,4,202,181,53,81,
    42,72,26,32,26,76,12,18,140,244,116,206,63,250,245,96,71,199,188,142,109,252,201,213,
    242,233,89,32,188,11,235,7,130,121,176,251,175,196,11,119,1,204,93,245,236,119,175,98,
    189,4,199,104,74,29,137,246,39,11,92,138,94,38,26,9,55,118,53,55,223,65,141,20,
    20,146,118,33,90,127,8,105,253,132,30,38,3,204,235,130,226,118,246,175,147,229,98,63,
    166,213,28,128,150,198,100,179,91,23,53,188,70,192,238,145,84,35,22,78,58,22,105,119,
    158,69,145,198,173,124,158,115,73,17,149,211,108,231,161,70,93,213,99,84,229,76,70,105,
    78,15,138,247,136,93,253,155,242,188,15,194,127,67,31,26,250,205,52,34,245,104,133,56,
    189,159,158,16,79,39,195,104,133,110,8,53,240,170,46,98,75,102,71,233,217,246,44,215,
    85,135,87,21,70,196,198,186,187,72,243,114,218,199,35,182,165,224,189,111,7,54,202,175,
    180,102,244,88,186,229,175,113,212,6,178,163,150,65,100,236,193,123,203,241,80,171,56,216,
    231,148,194,34,189,0,45,86,50,212,152,81,159,114,18,196,118,74,76,164,78,249,88,71,
    70,2,81,39,4,116,19,156,166,117,148,20,208,173,136,26,245,51,55,51,58,29,104,195,
    186,117,171,63,254,155,196,60,186,202,29,229,245,39,167,21,169,138,233,54,151,131,5,210,
    76,162,192,140,84,202,227,196,81,15,91,49,75,9,162,17,210,78,239,111,163,139,145,39,
    235,146,0,186,179,0,117,26,128,66,220,21,163,171,182,203,47,221,29,214,99,19,143,209,
    20,16,4,58,208,1,140,209,52,189,155,17,68,144,2,189,117,101,42,102,211,203,120,98,
    25,145,94,251,91,178,168,234,2,41,23,255,96,62,255,108,153,110,130,18,5,71,78,180,
    204,100,78,208,36,61,44,152,185,168,71,137,156,83,65,229,147,173,175,119,88,243,252,85,
    30,146,167,173,97,76,86,101,91,233,76,56,130,211,231,98,42,197,46,50,162,23,144,162,
    179,41,216,160,73,165,18,5,197,95,178,113,233,157,118,115,115,251,214,173,22,5,197,210,
    70,77,183,110,187,118,135,30,238,30,56,159,130,126,49,244,216,218,28,144,141,229,232,205,
    169,160,146,21,89,135,172,153,130,138,106,119,50,162,71,224,228,57,36,193,150,45,196,203,
    195,240,47,167,228,20,227,22,71,179,22,167,159,7,73,230,32,238,4,163,224,105,206,72,
    94,244,250,176,203,104,193,74,151,215,201,211,244,62,115,120,87,105,203,138,207,115,116,118,
    37,178,190,243,87,221,221,221,187,48,161,135,162,16,56,139,249,132,253,170,253,219,210,171,
    163,80,182,64,49,19,121,229,201,191,109,111,110,182,179,70,30,66,32,22,150,146,36,138,
    131,45,195,32,236,161,143,97,203,190,230,139,161,111,182,230,17,140,222,124,152,55,237,68,
    134,213,192,15,100,95,205,210,107,4,196,76,42,18,139,53,19,171,244,44,227,44,149,5,
    167,132,157,26,130,55,242,182,232,165,230,10,110,194,54,152,213,209,126,26,175,27,253,248,
    111,62,136,148,110,194,145,126,121,154,4,17,4,154,195,155,33,22,147,240,234,72,247,35,
    11,7,23,175,94,8,107,245,234,193,193,5,203,177,88,153,183,169,125,244,206,201,114,201,
    78,140,54,226,64,79,68,84,239,44,221,71,154,155,183,168,121,7,115,85,4,133,4,110,
    56,25,17,139,195,53,180,30,253,98,232,163,173,104,161,114,14,114,111,4,45,69,153,64,
    87,38,219,239,208,85,204,3,141,66,183,166,100,71,109,138,149,176,18,171,202,54,252,229,
    54,203,236,82,190,99,16,86,79,71,190,63,202,250,3,103,224,228,249,75,214,3,168,139,
    249,61,46,221,54,125,230,128,47,238,6,255,246,200,130,213,11,199,231,123,247,147,133,2,
    112,124,225,224,68,71,254,190,246,246,31,223,55,121,211,100,201,78,225,111,184,68,111,124,
    71,185,228,104,205,203,197,158,97,27,195,61,189,109,153,164,212,71,182,228,47,167,149,11,
    128,66,178,128,239,194,97,225,194,155,137,73,250,158,39,91,1,36,177,124,23,155,176,205,
    36,4,238,178,87,153,60,19,171,86,129,80,19,137,252,38,75,35,61,131,131,11,215,108,
    107,107,107,219,53,190,102,35,60,46,66,70,228,230,10,46,167,58,117,7,240,55,214,60,
    255,250,196,248,203,51,39,19,162,190,102,193,194,53,243,121,57,152,86,224,63,218,214,45,
    249,204,175,126,92,90,254,100,87,215,19,147,97,6,170,242,15,61,155,248,45,105,254,177,
    145,25,44,129,24,112,12,6,47,51,86,38,135,223,227,54,199,0,116,37,189,167,117,63,
    110,64,240,209,223,238,162,243,40,63,216,4,198,158,50,146,133,172,110,184,45,26,226,32,
    168,243,224,3,15,194,59,61,180,227,87,247,173,90,187,122,124,126,112,47,65,109,91,183,
    122,245,58,147,9,100,90,78,88,159,93,95,225,236,159,153,51,124,19,80,202,211,149,62,
    240,57,27,59,159,104,236,234,234,122,50,149,12,215,65,6,135,131,91,203,65,217,126,219,
    92,232,46,62,0,133,115,66,96,160,113,209,185,36,244,63,66,62,40,38,10,81,6,170,
    62,0,160,187,150,172,132,226,192,78,225,125,243,146,201,36,176,170,250,40,17,18,104,163,
    171,214,45,201,191,90,122,118,103,227,147,15,108,111,155,122,32,216,65,89,179,125,245,34,
    94,109,240,226,196,233,122,249,217,15,126,143,251,30,93,51,247,87,228,238,137,113,126,230,
    2,79,158,191,248,69,0,237,170,143,53,118,233,244,166,70,181,163,224,139,119,148,180,145,
    244,218,181,9,203,193,2,22,114,138,31,117,136,29,127,225,75,244,162,86,168,205,100,211,
    145,233,129,245,172,225,7,223,236,32,2,177,119,242,42,222,84,75,215,83,93,201,108,10,
    202,90,136,46,214,240,224,194,97,235,206,209,132,125,211,146,109,211,30,8,60,185,107,245,
    234,54,126,38,78,108,110,190,252,61,204,120,206,212,213,30,90,119,230,158,132,204,111,223,
    221,213,149,170,111,236,122,66,7,79,36,242,181,237,68,33,163,237,137,187,238,88,189,96,
    189,229,176,251,39,74,187,94,31,146,54,249,54,250,215,173,14,28,94,219,48,253,37,213,
    124,166,103,229,4,142,103,2,167,76,7,224,241,198,18,163,150,109,39,71,151,59,118,126,
    229,248,202,252,72,227,93,163,15,46,86,103,62,223,160,18,235,206,176,215,114,246,74,124,
    245,154,179,254,140,188,125,119,10,42,179,198,95,53,162,47,146,35,182,6,9,90,243,157,
    237,237,75,54,110,74,16,118,99,117,69,157,88,215,159,121,243,124,156,58,66,216,171,58,
    160,106,137,190,158,67,145,242,197,188,36,2,103,233,161,87,100,213,3,13,183,179,187,210,
    36,18,107,23,0,103,87,42,245,208,246,51,30,174,204,47,92,253,86,74,238,41,107,112,
    209,44,186,18,242,226,23,83,0,122,223,19,58,254,86,160,186,102,205,38,142,221,213,56,
    122,95,231,202,178,228,206,91,201,147,107,218,210,160,188,24,71,129,246,163,107,133,156,204,
    15,119,240,236,110,28,38,110,181,92,177,15,146,35,122,145,10,142,100,176,123,244,216,207,
    110,90,56,248,128,190,115,52,245,171,237,103,59,18,121,205,146,183,139,41,15,142,207,170,
    251,162,130,71,106,236,250,237,114,189,22,64,71,176,71,239,20,70,244,200,200,37,43,51,
    236,186,83,162,200,99,29,114,49,157,71,199,139,160,53,173,67,28,39,15,173,162,119,88,
    225,113,166,168,180,106,11,102,129,236,98,59,108,220,178,187,106,57,195,47,60,160,70,158,
    24,93,190,120,22,135,187,102,241,219,228,220,184,113,150,93,166,125,187,27,27,239,252,237,
    114,240,187,252,43,151,65,146,0,185,90,35,148,166,79,102,188,129,109,83,54,39,100,249,
    229,142,203,177,195,137,66,189,252,205,12,168,230,240,74,129,254,42,121,155,148,122,30,170,
    173,128,26,177,148,205,174,23,39,171,126,16,225,231,62,209,222,57,127,54,170,181,112,163,
    252,118,56,219,22,207,250,101,219,223,104,188,175,125,103,157,30,145,231,222,247,96,30,64,
    219,233,47,124,93,238,221,67,71,149,213,65,94,142,150,175,8,253,111,106,163,95,12,125,
    108,101,209,20,202,195,113,19,73,165,82,102,108,7,205,235,221,95,65,110,132,109,11,111,
    75,46,40,61,107,100,93,223,185,242,165,217,29,202,146,249,111,71,162,75,230,207,26,180,
    109,247,19,235,187,46,173,211,117,249,232,21,11,55,37,172,82,10,127,229,253,86,239,170,
    64,5,34,192,16,112,56,173,215,135,254,159,219,5,124,255,202,30,65,17,204,56,189,23,
    65,247,150,7,182,178,123,95,187,160,16,68,9,222,220,89,72,242,124,157,190,119,247,44,
    101,51,190,253,173,139,84,62,185,96,246,237,81,190,239,193,31,95,122,105,88,143,212,221,
    55,185,227,190,118,210,93,139,87,136,79,186,2,197,228,157,199,146,108,172,181,38,244,77,
    10,138,57,239,15,86,226,54,132,77,239,108,160,239,165,191,177,142,55,188,235,62,82,96,
    182,132,93,161,83,23,94,50,46,207,90,56,111,145,18,242,187,37,109,179,111,166,241,27,
    87,238,188,244,82,61,18,126,226,206,246,59,238,104,31,201,102,179,201,76,158,141,133,19,
    28,54,145,213,31,97,138,0,160,174,68,191,25,250,121,107,135,72,127,145,9,217,52,89,
    4,64,246,235,20,12,239,6,38,73,146,88,111,129,204,179,122,164,177,79,157,245,113,188,
    5,43,197,163,90,180,238,133,214,133,44,254,214,214,206,230,3,198,7,26,17,180,209,182,
    181,31,183,39,74,165,157,201,212,22,199,29,221,192,211,38,103,138,84,162,127,227,238,166,
    125,51,244,187,171,38,240,46,152,56,172,176,166,56,49,248,208,141,50,205,97,35,158,76,
    195,5,136,162,14,228,133,169,200,230,141,103,61,128,110,86,64,243,109,157,139,26,120,249,
    172,173,122,252,137,134,63,58,57,184,182,245,238,166,166,54,150,80,173,17,103,209,29,229,
    119,13,116,93,218,165,55,66,86,100,20,28,139,216,141,93,203,211,116,155,73,161,187,93,
    178,137,35,200,114,254,99,24,90,152,141,126,116,37,164,226,116,212,55,191,77,222,54,127,
    96,135,155,166,27,222,157,140,34,138,157,120,118,229,240,7,173,228,238,93,211,74,84,141,
    84,78,244,154,90,55,3,236,108,89,251,217,207,190,188,8,247,190,229,233,23,238,140,47,
    58,249,235,79,116,222,125,247,221,23,60,188,236,130,62,150,57,182,173,241,19,171,182,218,
    153,219,192,109,3,79,118,69,194,141,182,166,170,201,245,54,185,108,57,94,85,198,9,18,
    83,72,89,88,133,35,127,233,215,191,233,110,50,189,247,155,53,173,131,230,174,93,153,52,
    172,7,87,174,110,29,127,225,205,5,109,116,237,91,243,200,154,241,53,107,224,235,137,29,
    15,14,15,12,76,76,172,237,244,83,59,89,13,166,71,55,6,188,208,46,23,180,175,233,
    97,0,184,123,96,96,237,226,237,171,23,110,60,185,102,124,124,108,145,187,198,198,79,174,
    91,184,122,241,11,157,3,240,19,77,15,227,106,122,120,197,66,214,221,31,111,243,243,170,
    53,149,243,42,215,250,121,167,74,37,58,222,116,248,241,83,111,28,157,88,112,226,145,161,
    142,142,137,225,142,116,218,73,23,119,209,67,159,191,168,39,95,156,63,127,151,115,236,114,
    111,35,248,3,151,127,235,95,96,253,211,191,124,235,91,223,130,255,255,233,91,240,232,159,
    174,154,102,125,250,134,171,142,223,127,197,23,190,240,133,207,179,245,143,31,250,188,183,190,
    240,249,143,124,232,243,95,96,235,243,127,247,17,246,240,67,87,220,192,214,5,23,220,10,
    235,180,247,195,39,111,189,224,130,27,42,235,170,63,97,175,252,147,127,244,222,235,67,244,
    25,246,33,31,250,136,255,129,31,162,223,252,147,227,159,62,126,60,112,124,159,134,47,0,
    226,174,111,93,241,45,248,250,254,15,208,209,221,255,15,5,236,54,94,3,48,237,224,0,
    0,0,0,73,69,78,68,174,66,96,130,
};

struct State {
    float t = 0.f;
    float bounce = -1.f;
    float heart = -1.f;
};

inline void Step(State& s, float dt) {
    s.t += dt;
    if (s.bounce >= 0.f) { s.bounce += dt / 0.45f; if (s.bounce >= 1.f) s.bounce = -1.f; }
    if (s.heart >= 0.f)  { s.heart += dt / 1.1f;  if (s.heart >= 1.f)  s.heart = -1.f; }
}

// Bitmap hasil IStream hanya valid selama stream hidup, jadi pikselnya disalin.
inline G::Bitmap* Load() {
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put()))) return nullptr;
    ULONG written = 0;
    if (FAILED(stream->Write(kPng, static_cast<ULONG>(sizeof(kPng)), &written)) || written != sizeof(kPng))
        return nullptr;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    G::Bitmap src(stream.Get());
    if (src.GetLastStatus() != G::Ok) return nullptr;
    const INT w = static_cast<INT>(src.GetWidth()), h = static_cast<INT>(src.GetHeight());
    auto* copy = new G::Bitmap(w, h, PixelFormat32bppPARGB);
    {
        G::Graphics gg(copy);
        gg.SetCompositingMode(G::CompositingModeSourceCopy);
        gg.DrawImage(&src, G::Rect(0, 0, w, h));
    }
    return copy;
}

inline void DrawZ(G::Graphics& g, float x, float y, float size, int alpha) {
    G::Pen pen(G::Color(static_cast<BYTE>(alpha), 160, 98, 236), size * 0.26f);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    pen.SetLineJoin(G::LineJoinRound);
    const G::PointF pts[] = {{x, y}, {x + size, y}, {x, y + size}, {x + size, y + size}};
    g.DrawLines(&pen, pts, 4);
}

inline void DrawHeart(G::Graphics& g, float cx, float cy, float s, int alpha) {
    G::GraphicsPath p;
    p.AddBezier(cx, cy + 0.9f * s, cx - 1.3f * s, cy + 0.1f * s, cx - 0.9f * s, cy - 0.85f * s, cx, cy - 0.35f * s);
    p.AddBezier(cx, cy - 0.35f * s, cx + 0.9f * s, cy - 0.85f * s, cx + 1.3f * s, cy + 0.1f * s, cx, cy + 0.9f * s);
    p.CloseFigure();
    G::SolidBrush b(G::Color(static_cast<BYTE>(alpha), 255, 120, 165));
    g.FillPath(&b, &p);
}

inline void Draw(G::Graphics& g, G::Image* img, const State& s, const G::RectF& r, const G::RectF& clip) {
    if (!img) return;
    const float breath = std::sin(s.t * 2.f * kPi / 3.4f);
    const float sway = 1.2f * std::sin(s.t * 2.f * kPi / 7.f);
    const float lift = s.bounce >= 0.f ? std::sin(s.bounce * kPi) : 0.f;
    const float sy = 1.f + 0.02f * breath + 0.04f * lift;
    const float sx = 1.f - 0.006f * breath;

    G::GraphicsState st = g.Save();
    g.SetClip(clip, G::CombineModeIntersect);
    g.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);

    g.TranslateTransform(r.X + r.Width / 2.f, r.Y + r.Height - lift * r.Height * 0.10f);
    g.RotateTransform(sway);
    g.ScaleTransform(sx, sy);
    G::ImageAttributes ia;
    ia.SetWrapMode(G::WrapModeTileFlipXY);
    g.DrawImage(img, G::RectF(-r.Width / 2.f, -r.Height, r.Width, r.Height), 0.f, 0.f,
                static_cast<float>(img->GetWidth()), static_cast<float>(img->GetHeight()), G::UnitPixel, &ia);
    g.Restore(st);

    G::GraphicsState st2 = g.Save();
    g.SetClip(clip, G::CombineModeIntersect);
    for (int i = 0; i < 3; ++i) {
        const float p = std::fmod(s.t / 3.6f + static_cast<float>(i) / 3.f, 1.f);
        const int a = static_cast<int>(235.f * std::sin(p * kPi));
        if (a < 6) continue;
        DrawZ(g, r.X + r.Width * 0.74f + p * r.Width * 0.20f + std::sin(p * 6.f) * 1.5f,
              r.Y + r.Height * 0.10f - p * r.Height * 0.30f, r.Height * (0.10f + 0.07f * p), a);
    }
    if (s.heart >= 0.f)
        DrawHeart(g, r.X + r.Width * 0.12f + std::sin(s.heart * 8.f) * 2.f,
                  r.Y + r.Height * 0.30f - s.heart * r.Height * 0.30f, r.Height * 0.13f,
                  static_cast<int>(255.f * (1.f - s.heart)));
    g.Restore(st2);
}

}

class TrayIcons {
public:
    ~TrayIcons() { Destroy(); }

    void Build(bool lightTaskbar) {
        Destroy();
        GdiplusSession gdip;
        if (!gdip.ok()) return;
        const int size = std::max(16, GetSystemMetrics(SM_CXSMICON));
        normal_ = Make(size, [&](G::Graphics& g) {
            const G::Color c = lightTaskbar ? G::Color(255, 32, 32, 36) : G::Color(255, 245, 246, 250);
            glyph::Speaker(g, 0.f, 0.f, static_cast<float>(size), c, false);
        });
        locked_ = Make(size, [&](G::Graphics& g) {
            const G::Color c = theme::Amber();
            const float k = static_cast<float>(size) / 16.f;
            G::SolidBrush b(c);
            const G::PointF body[] = {{0.5f * k, 6.5f * k}, {3.4f * k, 6.5f * k}, {6.8f * k, 3.6f * k},
                                      {6.8f * k, 12.4f * k}, {3.4f * k, 9.5f * k}, {0.5f * k, 9.5f * k}};
            g.FillPolygon(&b, body, 6);
            glyph::Lock(g, 7.f * k, 3.f * k, 9.f * k, c, true);
        });
    }

    HICON normal() const { return normal_; }
    HICON locked() const { return locked_; }

private:
    template <class Draw>
    static HICON Make(int size, Draw draw) {
        G::Bitmap bmp(size, size, PixelFormat32bppARGB);
        {
            G::Graphics g(&bmp);
            g.SetSmoothingMode(G::SmoothingModeAntiAlias);
            g.Clear(G::Color(0, 0, 0, 0));
            draw(g);
        }
        HICON icon = nullptr;
        bmp.GetHICON(&icon);
        return icon;
    }
    void Destroy() {
        if (normal_) DestroyIcon(normal_);
        if (locked_) DestroyIcon(locked_);
        normal_ = locked_ = nullptr;
    }
    HICON normal_ = nullptr, locked_ = nullptr;
};

bool TaskbarIsLight() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

class FlyoutHost {
public:
    virtual ~FlyoutHost() = default;
    virtual bool StartupEnabled() = 0;
    virtual void SetStartup(bool on) = 0;
    virtual void RequestQuit() = 0;
    virtual void RequestHide() = 0;
};

class Flyout {
public:
    Flyout(Engine& engine, FlyoutHost& host, HINSTANCE inst) : engine_(engine), host_(host), inst_(inst) {}
    ~Flyout() { Destroy(); }
    Flyout(const Flyout&) = delete;
    Flyout& operator=(const Flyout&) = delete;

    HWND hwnd() const { return hwnd_; }
    void Destroy() { if (hwnd_) DestroyWindow(hwnd_); }

    bool Create(POINT anchor) {
        if (!gdip_.ok()) return false;
        RegisterClassOnce();

        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kFlyoutClass, L"Hub Volume", WS_POPUP,
                                anchor.x, anchor.y, 1, 1, nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;

        auto getDpi = util::Proc<UINT(WINAPI*)(HWND)>(L"user32.dll", "GetDpiForWindow");
        const UINT dpi = getDpi ? getDpi(hwnd_) : 96;
        scale_ = static_cast<float>(dpi) / 96.f;

        HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        const RECT work = mi.rcWork;

        BuildFonts();
        startup_ = host_.StartupEnabled();
        W_ = S(kWidth);
        hdrH_ = S(kHeaderH);
        ftrH_ = S(kFooterH);
        Relayout();

        const float maxH = std::min(S(kMaxHeight), static_cast<float>(work.bottom - work.top) - S(24));
        H_ = std::clamp(hdrH_ + contentH_ + ftrH_, S(220), maxH);
        viewportH_ = H_ - hdrH_ - ftrH_;
        ClampScroll();
        ComputeFooterRects();

        const int w = static_cast<int>(std::lround(W_)), h = static_cast<int>(std::lround(H_));
        const int margin = static_cast<int>(S(12));

        const bool right = anchor.x > (work.left + work.right) / 2;
        const bool bottom = anchor.y > (work.top + work.bottom) / 2;
        const int x = right ? work.right - w - margin : work.left + margin;
        const int y = bottom ? work.bottom - h - margin : work.top + margin;

        DWORD round = 2;
        DwmSetWindowAttribute(hwnd_, 33 , &round, sizeof(round));

        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        StartMascot();
        return true;
    }

    void Refresh() {
        if (!hwnd_) return;
        Relayout();
        ClampScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

private:

    static constexpr float kWidth = 392.f, kHeaderH = 84.f, kFooterH = 56.f, kMaxHeight = 640.f;
    static constexpr UINT_PTR kAnimTimer = 1;
    static constexpr float kSectionH = 32.f, kRowH = 82.f, kNoteH = 56.f, kCardMargin = 10.f;
    static constexpr float kLockW = 88.f;

    float S(float v) const { return v * scale_; }

    enum class Part { None, Lock, Mute, Slider, Startup, Quit, Mascot };
    struct Hit { Part part = Part::None; int row = -1; };
    struct Item {
        enum Type { Section, RowItem, Note } type = Section;
        std::wstring text;
        int row = -1;
        float y = 0.f, h = 0.f;
    };
    struct RowRects { G::RectF card, icon, title, lock, status, mute, track, pct, sliderHit; };

    G::RectF MascotRect() const {
        const float w = S(mascot::kDisplayW), h = w / mascot::kAspect;
        return G::RectF(W_ - S(12) - w, hdrH_ - h, w, h);
    }

    void StartMascot() {
        mascotImg_.reset(mascot::Load());
        mascot_.t = static_cast<float>(GetTickCount64() % 7000) / 1000.f;

        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        animate_ = (anim != FALSE);
        if (animate_) {
            lastTick_ = GetTickCount64();
            SetTimer(hwnd_, kAnimTimer, 50, nullptr);
        }
    }

    void TickMascot() {
        const ULONGLONG now = GetTickCount64();

        const float dt = std::min(0.1f, static_cast<float>(now - lastTick_) / 1000.f);
        lastTick_ = now;
        mascot::Step(mascot_, dt);

        RECT area{static_cast<LONG>(W_ - S(mascot::kDisplayW + 40.f)), 0, static_cast<LONG>(std::ceil(W_)),
                  static_cast<LONG>(std::ceil(hdrH_))};
        InvalidateRect(hwnd_, &area, FALSE);
    }

    void RegisterClassOnce() {
        static bool done = false;
        if (done) return;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = &Flyout::WndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kFlyoutClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) done = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Flyout* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<Flyout*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<Flyout*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->OnMessage(hwnd, msg, wp, lp);
    }

    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: OnPaint(); return 0;
            case WM_ACTIVATE:

                if (LOWORD(wp) == WA_INACTIVE) host_.RequestHide();
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) host_.RequestHide();
                return 0;
            case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
            case WM_MOUSELEAVE: tracking_ = false; SetHot(Hit{}); return 0;
            case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
            case WM_LBUTTONUP: OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
            case WM_CAPTURECHANGED: dragging_ = false; return 0;
            case WM_TIMER:
                if (wp == kAnimTimer) TickMascot();
                return 0;
            case WM_MOUSEWHEEL: {
                scroll_ -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.f * S(48);
                ClampScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    POINT p;
                    GetCursorPos(&p);
                    ScreenToClient(hwnd_, &p);
                    Hit h = HitTest(static_cast<float>(p.x), static_cast<float>(p.y));
                    bool hand = h.part != Part::None;
                    if (h.part == Part::Slider && h.row >= 0 && (rows_[h.row].locked || !rows_[h.row].live)) hand = false;
                    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_DESTROY:

                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                hwnd_ = nullptr;
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void BuildFonts() {
        family_ = std::make_unique<G::FontFamily>(L"Segoe UI");
        if (!family_->IsAvailable()) family_.reset(G::FontFamily::GenericSansSerif()->Clone());
        auto mk = [&](float px, int style) {
            return std::make_unique<G::Font>(family_.get(), px * scale_, style, G::UnitPixel);
        };
        fTitle_ = mk(18, G::FontStyleBold);
        fRow_ = mk(13, G::FontStyleRegular);
        fBtn_ = mk(12, G::FontStyleBold);
        fSmall_ = mk(11, G::FontStyleRegular);
        fPct_ = mk(12, G::FontStyleRegular);
        fSection_ = mk(11, G::FontStyleBold);
        auto fmt = [&](G::StringAlignment align) {
            auto f = std::make_unique<G::StringFormat>(G::StringFormatFlagsNoWrap);
            f->SetAlignment(align);
            f->SetLineAlignment(G::StringAlignmentCenter);
            f->SetTrimming(G::StringTrimmingEllipsisCharacter);
            return f;
        };
        fmtL_ = fmt(G::StringAlignmentNear);
        fmtR_ = fmt(G::StringAlignmentFar);
        fmtC_ = fmt(G::StringAlignmentCenter);
    }

    void Relayout() {
        rows_ = engine_.Snapshot();
        items_.clear();
        if (!engine_.Ready()) {
            items_.push_back(Item{Item::Note, L"Layanan audio Windows belum tersedia. Mencoba lagi\u2026", -1, S(4), S(kNoteH)});
            contentH_ = S(kNoteH) + S(12);
            return;
        }
        float y = S(4);
        struct Sec { Kind kind; const wchar_t* title; };
        const Sec secs[] = {{Kind::Output, L"KELUARAN"}, {Kind::App, L"APLIKASI"}, {Kind::Input, L"MIKROFON"}};
        for (const Sec& sec : secs) {
            std::vector<int> idx;
            for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i].kind == sec.kind) idx.push_back(static_cast<int>(i));
            if (idx.empty() && sec.kind != Kind::App) continue;
            items_.push_back(Item{Item::Section, sec.title, -1, y, S(kSectionH)});
            y += S(kSectionH);
            if (idx.empty()) {
                items_.push_back(Item{Item::Note, L"Belum ada aplikasi yang sedang memutar suara.", -1, y, S(kNoteH)});
                y += S(kNoteH);
            }
            for (int i : idx) {
                items_.push_back(Item{Item::RowItem, L"", i, y, S(kRowH)});
                y += S(kRowH);
            }
        }
        contentH_ = y + S(8);
    }

    void ClampScroll() {
        const float maxScroll = std::max(0.f, contentH_ - viewportH_);
        scroll_ = std::clamp(scroll_, 0.f, maxScroll);
    }

    void ComputeFooterRects() {
        const float top = H_ - ftrH_;
        startupRect_ = G::RectF(S(12), top + S(10), S(250), S(36));
        quitRect_ = G::RectF(W_ - S(12) - S(76), top + S(10), S(76), S(36));
    }

    RowRects RectsFor(float iy) const {
        RowRects r;
        const float cx = S(kCardMargin), cw = W_ - 2.f * cx;
        const float cy = iy + S(3), ch = S(kRowH - 6.f);
        r.card = G::RectF(cx, cy, cw, ch);
        r.icon = G::RectF(cx + S(12), cy + S(10), S(24), S(24));
        r.lock = G::RectF(cx + cw - S(12) - S(kLockW), cy + S(9), S(kLockW), S(26));
        r.title = G::RectF(cx + S(46), cy + S(8), r.lock.X - S(8) - (cx + S(46)), S(26));
        r.status = G::RectF(cx + S(46), cy + S(34), cw - S(46) - S(12), S(16));
        r.mute = G::RectF(cx + S(10), cy + S(50), S(24), S(24));
        const float trackL = cx + S(44), trackR = cx + cw - S(12) - S(42);
        r.track = G::RectF(trackL, cy + S(59), trackR - trackL, S(6));
        r.pct = G::RectF(trackR + S(6), cy + S(50), S(36), S(24));
        r.sliderHit = G::RectF(trackL - S(10), cy + S(50), (trackR - trackL) + S(20), S(24));
        return r;
    }

    Hit HitTest(float x, float y) const {
        if (y >= H_ - ftrH_) {
            if (startupRect_.Contains(x, y)) return Hit{Part::Startup, -1};
            if (quitRect_.Contains(x, y)) return Hit{Part::Quit, -1};
            return Hit{};
        }
        if (y < hdrH_) return MascotRect().Contains(x, y) ? Hit{Part::Mascot, -1} : Hit{};
        for (const Item& it : items_) {
            if (it.type != Item::RowItem) continue;
            const float iy = hdrH_ + it.y - scroll_;
            if (y < iy || y >= iy + it.h) continue;
            const RowRects rc = RectsFor(iy);
            if (rc.lock.Contains(x, y)) return Hit{Part::Lock, it.row};
            if (rc.mute.Contains(x, y)) return Hit{Part::Mute, it.row};
            if (rc.sliderHit.Contains(x, y)) return Hit{Part::Slider, it.row};
            return Hit{Part::None, it.row};
        }
        return Hit{};
    }

    void SetHot(const Hit& h) {
        if (h.part == hot_.part && h.row == hot_.row) return;
        hot_ = h;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnMouseMove(int x, int y) {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            tracking_ = true;
        }
        if (dragging_) { ApplyDrag(static_cast<float>(x)); return; }
        SetHot(HitTest(static_cast<float>(x), static_cast<float>(y)));
    }

    void OnLButtonDown(int x, int y) {
        const Hit h = HitTest(static_cast<float>(x), static_cast<float>(y));
        pressed_ = h;
        if (h.part == Part::Slider && h.row >= 0) {
            const Row& r = rows_[h.row];
            if (r.locked || !r.live) return;
            dragging_ = true;
            dragKind_ = r.kind;
            dragKey_ = r.key;
            SetCapture(hwnd_);
            ApplyDrag(static_cast<float>(x));
        }
    }

    void OnLButtonUp(int x, int y) {
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            return;
        }
        const Hit h = HitTest(static_cast<float>(x), static_cast<float>(y));
        const Hit down = pressed_;
        pressed_ = Hit{};
        if (h.part == Part::None || h.part != down.part || h.row != down.row) return;

        switch (h.part) {
            case Part::Lock: {

                const Kind kind = rows_[h.row].kind;
                const std::wstring key = rows_[h.row].key;
                engine_.ToggleLock(kind, key);
                Refresh();
                break;
            }
            case Part::Mute: {
                const Kind kind = rows_[h.row].kind;
                const std::wstring key = rows_[h.row].key;
                const bool muted = rows_[h.row].now.muted;
                if (rows_[h.row].locked || !rows_[h.row].live) break;
                engine_.SetMuted(kind, key, !muted);
                Refresh();
                break;
            }
            case Part::Startup:
                host_.SetStartup(!startup_);
                startup_ = host_.StartupEnabled();
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            case Part::Quit: host_.RequestQuit(); break;
            case Part::Mascot:

                if (mascot_.bounce < 0.f) { mascot_.bounce = 0.f; mascot_.heart = 0.f; }
                break;
            default: break;
        }
    }

    void ApplyDrag(float x) {

        for (const Item& it : items_) {
            if (it.type != Item::RowItem) continue;
            Row& r = rows_[it.row];
            if (r.kind != dragKind_ || r.key != dragKey_) continue;
            const RowRects rc = RectsFor(hdrH_ + it.y - scroll_);
            float level = (x - rc.track.X) / rc.track.Width;
            level = std::clamp(std::round(level * 100.f) / 100.f, 0.f, 1.f);
            engine_.SetLevel(r.kind, r.key, level);
            r.now.level = level;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        dragging_ = false;
        ReleaseCapture();
    }

    void OnPaint() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;

        RECT d = ps.rcPaint;
        IntersectRect(&d, &d, &rc);
        if (w > 0 && h > 0 && d.right > d.left && d.bottom > d.top) {

            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            IntersectClipRect(mem, d.left, d.top, d.right, d.bottom);
            {
                G::Graphics g(mem);
                g.SetClip(G::Rect(d.left, d.top, d.right - d.left, d.bottom - d.top));
                Paint(g, mem, w, h);
                g.Flush(G::FlushIntentionSync);
            }
            BitBlt(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top, mem, d.left, d.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd_, &ps);
    }

    void Text(G::Graphics& g, const std::wstring& s, const G::Font& f, const G::Color& c, const G::RectF& r,
              const G::StringFormat& fmt) {
        G::SolidBrush b(c);
        g.DrawString(s.c_str(), static_cast<INT>(s.size()), &f, r, &fmt, &b);
    }

    static std::wstring Percent(float level) { return std::to_wstring(static_cast<int>(std::lround(level * 100.f))) + L"%"; }

    static std::wstring StatusText(const Row& r) {
        std::wstring s;
        if (r.locked) {
            s = r.lockedTo.muted ? L"Terkunci (bisu)" : L"Terkunci di " + Percent(r.lockedTo.level);
            if (r.restores) s += L"  \u00B7  dikembalikan " + std::to_wstring(r.restores) + L"\u00D7";
            if (!r.live) s += L"  \u00B7  " + std::wstring(r.kind == Kind::App ? L"tidak berjalan" : L"tidak terhubung");
        } else if (r.now.muted) {
            s = L"Bisu";
        } else {
            s = L"Tidak terkunci";
        }
        return s;
    }

    void Paint(G::Graphics& g, HDC hdc, int W, int H) {
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
        {
            G::SolidBrush bg(theme::Bg());
            g.FillRectangle(&bg, 0.f, 0.f, static_cast<float>(W), static_cast<float>(H));
        }

        for (const Item& it : items_) {
            const float iy = hdrH_ + it.y - scroll_;
            if (iy + it.h < hdrH_ || iy > H_ - ftrH_) continue;
            switch (it.type) {
                case Item::Section:
                    Text(g, it.text, *fSection_, theme::Muted(), G::RectF(S(20), iy, W_ - S(40), it.h), *fmtL_);
                    break;
                case Item::Note:
                    Text(g, it.text, *fSmall_, theme::Muted(), G::RectF(S(20), iy, W_ - S(40), it.h), *fmtC_);
                    break;
                case Item::RowItem: PaintRow(g, hdc, rows_[it.row], it.row, iy); break;
            }
        }
        PaintScrollbar(g);
        PaintHeader(g);
        PaintFooter(g);

        G::Pen border(theme::Border(), 1.f);
        g.DrawRectangle(&border, 0.5f, 0.5f, static_cast<float>(W) - 1.f, static_cast<float>(H) - 1.f);
    }

    void PaintHeader(G::Graphics& g) {
        G::SolidBrush bg(theme::Bg());
        g.FillRectangle(&bg, 0.f, 0.f, W_, hdrH_);

        G::Pen line(theme::Border(), 1.f);
        g.DrawLine(&line, 0.f, hdrH_ - 0.5f, W_, hdrH_ - 0.5f);

        mascot::Draw(g, mascotImg_.get(), mascot_, MascotRect(), G::RectF(0.f, 0.f, W_, hdrH_));

        Text(g, L"Hub Volume", *fTitle_, theme::Text(), G::RectF(S(20), S(14), W_ - S(40), S(26)), *fmtL_);
        const size_t n = engine_.LockCount();
        const std::wstring sub = n == 0 ? L"Belum ada volume yang dikunci"
                                        : std::to_wstring(n) + L" volume terkunci";
        Text(g, sub, *fSmall_, n ? theme::Amber() : theme::Muted(), G::RectF(S(20), S(42), W_ - S(40), S(18)), *fmtL_);
    }

    void PaintFooter(G::Graphics& g) {
        const float top = H_ - ftrH_;
        G::SolidBrush bg(theme::Bg());
        g.FillRectangle(&bg, 0.f, top, W_, ftrH_);
        G::Pen line(theme::Border(), 1.f);
        g.DrawLine(&line, 0.f, top + 0.5f, W_, top + 0.5f);

        const bool hotS = hot_.part == Part::Startup;
        if (hotS) FillRound(g, theme::Card(), startupRect_, S(8));
        const G::RectF tr(startupRect_.X + S(8), top + S(19), S(34), S(18));
        FillRound(g, startup_ ? theme::Accent() : theme::Track(), tr, S(9));
        const float knob = S(12);
        const float kx = startup_ ? tr.X + tr.Width - S(3) - knob : tr.X + S(3);
        G::SolidBrush kb(G::Color(255, 255, 255, 255));
        g.FillEllipse(&kb, kx, tr.Y + S(3), knob, knob);
        Text(g, L"Mulai bersama Windows", *fRow_, theme::Text(),
             G::RectF(startupRect_.X + S(50), startupRect_.Y, startupRect_.Width - S(50), startupRect_.Height), *fmtL_);

        if (hot_.part == Part::Quit) FillRound(g, theme::Card(), quitRect_, S(8));
        Text(g, L"Keluar", *fRow_, theme::Muted(), quitRect_, *fmtC_);
    }

    void PaintScrollbar(G::Graphics& g) {
        if (contentH_ <= viewportH_ + 0.5f) return;
        const float trackH = viewportH_ - S(8);
        const float thumbH = std::max(S(24), trackH * viewportH_ / contentH_);
        const float maxScroll = contentH_ - viewportH_;
        const float ty = hdrH_ + S(4) + (trackH - thumbH) * (scroll_ / maxScroll);
        FillRound(g, G::Color(120, 150, 156, 168), G::RectF(W_ - S(6), ty, S(3), thumbH), S(1.5f));
    }

    void PaintRow(G::Graphics& g, HDC hdc, const Row& r, int idx, float iy) {
        const RowRects rc = RectsFor(iy);
        const bool dim = !r.live;
        FillRound(g, theme::Card(), rc.card, S(10));

        if (r.kind == Kind::App && r.icon) {
            g.Flush(G::FlushIntentionSync);
            DrawIconEx(hdc, static_cast<int>(rc.icon.X), static_cast<int>(rc.icon.Y), r.icon,
                       static_cast<int>(rc.icon.Width), static_cast<int>(rc.icon.Height), 0, nullptr, DI_NORMAL);
        } else {
            FillRound(g, theme::Tile(), rc.icon, S(6));
            const G::Color gc = dim ? theme::Muted() : theme::Text();
            const float gs = S(16), gx = rc.icon.X + S(4), gy = rc.icon.Y + S(4);
            if (r.kind == Kind::Input) {
                glyph::Mic(g, gx, gy, gs, gc, false);
            } else if (r.kind == Kind::Output) {
                glyph::Speaker(g, gx, gy, gs, gc, false);
            } else {
                const std::wstring letter = r.title.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(towupper(r.title[0])));
                Text(g, letter, *fBtn_, gc, rc.icon, *fmtC_);
            }
        }

        Text(g, r.title, *fRow_, dim ? theme::Muted() : theme::Text(), rc.title, *fmtL_);
        Text(g, StatusText(r), *fSmall_, r.locked ? theme::Amber() : theme::Muted(), rc.status, *fmtL_);

        const bool hotL = hot_.part == Part::Lock && hot_.row == idx;
        FillRound(g, r.locked ? (hotL ? theme::AmberBgHot() : theme::AmberBg())
                              : (hotL ? theme::ButtonHot() : theme::Button()), rc.lock, S(13));
        const G::Color lc = r.locked ? theme::Amber() : theme::Text();
        glyph::Lock(g, rc.lock.X + S(10), rc.lock.Y + S(5), S(16), lc, r.locked);
        Text(g, r.locked ? L"Terkunci" : L"Kunci", *fBtn_, lc,
             G::RectF(rc.lock.X + S(28), rc.lock.Y, rc.lock.Width - S(32), rc.lock.Height), *fmtL_);

        const bool mutedNow = r.locked ? r.lockedTo.muted : r.now.muted;
        const bool hotM = hot_.part == Part::Mute && hot_.row == idx;
        if (hotM && !r.locked && r.live) FillRound(g, theme::Button(), rc.mute, S(6));
        const G::Color mc = (r.locked || !r.live) ? theme::Muted() : theme::Text();
        if (r.kind == Kind::Input) glyph::Mic(g, rc.mute.X + S(4), rc.mute.Y + S(4), S(16), mc, mutedNow);
        else glyph::Speaker(g, rc.mute.X + S(4), rc.mute.Y + S(4), S(16), mc, mutedNow);

        const float level = r.locked ? r.lockedTo.level : r.now.level;
        G::Color fill = r.locked ? theme::Amber() : theme::Accent();
        if (!r.locked && (mutedNow || !r.live)) fill = theme::Muted();
        FillRound(g, theme::Track(), rc.track, rc.track.Height / 2.f);
        const float tx = rc.track.X + rc.track.Width * std::clamp(level, 0.f, 1.f);
        if (tx - rc.track.X > 1.f)
            FillRound(g, fill, G::RectF(rc.track.X, rc.track.Y, tx - rc.track.X, rc.track.Height), rc.track.Height / 2.f);
        const bool active = dragging_ && dragKind_ == r.kind && dragKey_ == r.key;
        const float tr = S(active ? 8.5f : 7.f);
        const float cy = rc.track.Y + rc.track.Height / 2.f;
        G::SolidBrush knob(r.locked ? G::Color(255, 255, 226, 160) : G::Color(255, 255, 255, 255));
        g.FillEllipse(&knob, tx - tr, cy - tr, tr * 2.f, tr * 2.f);

        Text(g, mutedNow ? L"Bisu" : Percent(level), *fPct_, theme::Muted(), rc.pct, *fmtR_);
    }

    Engine& engine_;
    FlyoutHost& host_;
    HINSTANCE inst_;
    HWND hwnd_ = nullptr;

    // GDI+ harus dibuat lebih dulu dan dihancurkan paling akhir.
    GdiplusSession gdip_;
    std::unique_ptr<G::FontFamily> family_;
    std::unique_ptr<G::Font> fTitle_, fRow_, fBtn_, fSmall_, fPct_, fSection_;
    std::unique_ptr<G::StringFormat> fmtL_, fmtR_, fmtC_;

    float scale_ = 1.f, W_ = 0.f, H_ = 0.f, hdrH_ = 0.f, ftrH_ = 0.f;
    float contentH_ = 0.f, viewportH_ = 0.f, scroll_ = 0.f;
    G::RectF startupRect_, quitRect_;
    std::vector<Row> rows_;
    std::vector<Item> items_;
    bool startup_ = false;
    bool tracking_ = false;
    bool dragging_ = false;

    std::unique_ptr<G::Bitmap> mascotImg_;
    mascot::State mascot_;
    bool animate_ = false;
    ULONGLONG lastTick_ = 0;
    Kind dragKind_ = Kind::Output;
    std::wstring dragKey_;
    Hit hot_, pressed_;
};

class App final : public FlyoutHost {
public:
    explicit App(HINSTANCE inst) : inst_(inst), engine_(cfg_) {}

    int Run(bool showUi) {
        cfg_.Load();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = inst_;
        wc.lpszClassName = kMainClass;
        if (!RegisterClassExW(&wc)) return 1;

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"Hub Volume", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                inst_, this);
        if (!hwnd_) return 1;

        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        icons_.Build(TaskbarIsLight());

        engine_.onLocksChanged = [this] { UpdateTray(); };
        engine_.onStateChanged = [this] { if (flyout_) flyout_->Refresh(); };

        Startup::SelfHeal();
        AddTray();
        engine_.Start(hwnd_);
        UpdateTray();
        if (showUi) ShowFlyout(CursorPoint());
        TrimWorkingSet();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }

    bool StartupEnabled() override { return Startup::Enabled(); }
    void SetStartup(bool on) override { Startup::Set(on); }

    void RequestQuit() override { PostMessageW(hwnd_, WM_APP_QUIT, 0, 0); }
    void RequestHide() override { PostMessageW(hwnd_, WM_APP_HIDE, 0, 0); }

private:
    static constexpr UINT kTrayId = 1;

    static POINT CursorPoint() {
        POINT p{};
        GetCursorPos(&p);
        return p;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->OnMessage(hwnd, msg, wp, lp);
    }

    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (taskbarCreated_ && msg == taskbarCreated_) { AddTray(); return 0; }
        switch (msg) {
            case WM_APP_TRAY:
                switch (LOWORD(lp)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT: ToggleFlyout(AnchorFrom(wp)); break;
                    case WM_CONTEXTMENU: ShowTrayMenu(AnchorFrom(wp)); break;
                }
                return 0;
            case WM_APP_VOLUME: engine_.OnVolumeMessage(static_cast<uint32_t>(wp)); return 0;
            case WM_APP_SESSIONS: engine_.OnSessionsMessage(static_cast<uint32_t>(wp)); return 0;
            case WM_APP_DEVICES: engine_.ScheduleDeviceResync(); return 0;
            case WM_APP_SHOW: ShowFlyout(CursorPoint()); return 0;
            case WM_APP_HIDE: HideFlyout(); return 0;
            case WM_APP_QUIT: Quit(); return 0;
            case WM_CLOSE:

                DestroyWindow(hwnd);
                return 0;
            case WM_TIMER:
                if (wp == kTimerTray) RetryTray();
                else engine_.OnTimer(wp);
                return 0;
            case WM_SETTINGCHANGE:

                if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                    icons_.Build(TaskbarIsLight());
                    UpdateTray();
                }
                return 0;
            case WM_DESTROY:
                flyout_.reset();
                engine_.Stop();
                RemoveTray();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    POINT AnchorFrom(WPARAM wp) const {
        POINT p{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
        if (p.x == 0 && p.y == 0) p = CursorPoint();
        return p;
    }

    NOTIFYICONDATAW BaseNid() const {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        return nid;
    }

    std::wstring TooltipText() const {
        if (!engine_.Ready()) return L"Hub Volume \u2014 layanan audio belum tersedia";
        const size_t n = engine_.LockCount();
        return n == 0 ? L"Hub Volume \u2014 tidak ada yang terkunci"
                      : L"Hub Volume \u2014 " + std::to_wstring(n) + L" terkunci";
    }

    void FillIconAndTip(NOTIFYICONDATAW& nid) const {
        nid.uFlags |= NIF_ICON | NIF_TIP;
        nid.hIcon = engine_.LockCount() > 0 ? icons_.locked() : icons_.normal();
        wcsncpy_s(nid.szTip, TooltipText().c_str(), _TRUNCATE);
    }

    void AddTray() {
        NOTIFYICONDATAW nid = BaseNid();
        nid.uFlags = NIF_MESSAGE;
        nid.uCallbackMessage = WM_APP_TRAY;
        FillIconAndTip(nid);
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (trayAdded_) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            KillTimer(hwnd_, kTimerTray);
        } else if (trayRetries_ < 60) {

            ++trayRetries_;
            SetTimer(hwnd_, kTimerTray, 2000, nullptr);
        }
    }

    void RetryTray() {
        if (trayAdded_) { KillTimer(hwnd_, kTimerTray); return; }
        AddTray();
    }

    void UpdateTray() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid = BaseNid();
        nid.uFlags = 0;
        FillIconAndTip(nid);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void RemoveTray() {
        if (!trayAdded_) return;
        NOTIFYICONDATAW nid = BaseNid();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }

    void MaybeShowTrayHint() {
        if (cfg_.trayHintShown || !trayAdded_) return;
        cfg_.trayHintShown = true;
        cfg_.Save();
        NOTIFYICONDATAW nid = BaseNid();
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
        wcsncpy_s(nid.szInfoTitle, L"Hub Volume tetap berjalan", _TRUNCATE);
        wcsncpy_s(nid.szInfo, L"Klik ikon speaker di dekat jam untuk membuka mixer lagi.", _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void ToggleFlyout(POINT anchor) {

        if (!flyout_ && GetTickCount64() - lastHide_ < 250) return;
        if (flyout_) HideFlyout();
        else ShowFlyout(anchor);
    }

    void ShowFlyout(POINT anchor) {
        if (flyout_ && flyout_->hwnd()) {
            SetForegroundWindow(flyout_->hwnd());
            return;
        }
        engine_.SetObserved(true);
        flyout_ = std::make_unique<Flyout>(engine_, *this, inst_);
        if (!flyout_->Create(anchor)) {
            flyout_.reset();
            engine_.SetObserved(false);
        }
    }

    void HideFlyout() {
        if (!flyout_) return;
        flyout_.reset();
        engine_.SetObserved(false);
        lastHide_ = GetTickCount64();

        TrimWorkingSet();
        MaybeShowTrayHint();
    }

    static void TrimWorkingSet() { SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)); }

    void ShowTrayMenu(POINT anchor) {
        enum { ID_OPEN = 1, ID_STARTUP, ID_UNLOCK_ALL, ID_QUIT };
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, ID_OPEN, L"Buka mixer");
        AppendMenuW(menu, MF_STRING | (Startup::Enabled() ? MF_CHECKED : 0), ID_STARTUP, L"Mulai bersama Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (engine_.LockCount() ? 0 : MF_GRAYED), ID_UNLOCK_ALL, L"Buka kunci semua");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_QUIT, L"Keluar");

        SetForegroundWindow(hwnd_);
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, anchor.x, anchor.y, 0,
                                       hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (cmd) {
            case ID_OPEN: ShowFlyout(anchor); break;
            case ID_STARTUP: Startup::Set(!Startup::Enabled()); break;
            case ID_UNLOCK_ALL: engine_.UnlockAll(); if (flyout_) flyout_->Refresh(); break;
            case ID_QUIT: Quit(); break;
        }
    }

    void Quit() {

        if (engine_.LockCount() > 0) {
            const int r = MessageBoxW(hwnd_,
                                      L"Masih ada volume yang terkunci. Jika Hub Volume ditutup, penguncian berhenti "
                                      L"sampai program dijalankan lagi.\n\nTetap keluar?",
                                      L"Hub Volume", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (r != IDYES) return;
        }
        DestroyWindow(hwnd_);
    }

    HINSTANCE inst_;
    HWND hwnd_ = nullptr;
    Config cfg_;
    Engine engine_;
    TrayIcons icons_;
    std::unique_ptr<Flyout> flyout_;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    int trayRetries_ = 0;
    ULONGLONG lastHide_ = 0;
};

bool HasArg(const wchar_t* arg) {
    const wchar_t* cmd = GetCommandLineW();
    return cmd && wcsstr(cmd, arg) != nullptr;
}

}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {

    auto setDpi = util::Proc<BOOL(WINAPI*)(HANDLE)>(L"user32.dll", "SetProcessDpiAwarenessContext");
    if (!setDpi || !setDpi(reinterpret_cast<HANDLE>(-4))) SetProcessDPIAware();

    const bool background = HasArg(L"--background");

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!background) {
            for (int i = 0; i < 20; ++i) {
                HWND other = FindWindowW(kMainClass, nullptr);
                if (other) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(other, &pid);
                    AllowSetForegroundWindow(pid);
                    PostMessageW(other, WM_APP_SHOW, 0, 0);
                    break;
                }
                Sleep(100);
            }
        }
        CloseHandle(mutex);
        return 0;
    }

    // MTA: callback Core Audio datang dari thread pool miliknya sendiri.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    int rc = 0;
    {
        App app(inst);
        rc = app.Run(!background);
    }

    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
