#include "hid.h"

#include <windows.h>
// After windows.h.
#include <hidsdi.h>
#include <setupapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>

namespace cooler {
namespace {

struct Model {
    uint16_t pid;
    bool pump;
    const char* name;
};
const Model kModels[] = {
    {0x0C1C, true, "Commander Core"},
    {0x0C32, true, "Commander ST"},
    {0x0C2A, false, "Commander Core XT"},
};

std::string hexText(const char* what, DWORD err) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s (error %lu)", what, (unsigned long)err);
    return buf;
}

}  // namespace

std::vector<HidInfo> FindCoolers() {
    std::vector<HidInfo> out;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &data); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(L"vid_1b1c") == std::wstring::npos || lower.find(L"mi_00") == std::wstring::npos) continue;
        for (const auto& m : kModels) {
            wchar_t pid[16];
            std::swprintf(pid, 16, L"pid_%04x", m.pid);
            if (lower.find(pid) == std::wstring::npos) continue;
            out.push_back({path, m.pid, m.pump, m.name});
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

Link::Link(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error_ = hexText("Couldn't open the controller", GetLastError());
        return;
    }
    PHIDP_PREPARSED_DATA pre = nullptr;
    HIDP_CAPS caps{};
    if (HidD_GetPreparsedData(handle_, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
        inLen_ = caps.InputReportByteLength;
        outLen_ = caps.OutputReportByteLength;
    }
    if (pre) HidD_FreePreparsedData(pre);
    if (outLen_ < kPacket + 1 || inLen_ < kPacket + 1) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Unexpected report sizes (in %zu, out %zu)", inLen_, outLen_);
        error_ = buf;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

Link::~Link() {
    if (Open()) CloseHandle(handle_);
    if (event_) CloseHandle(event_);
}

bool Link::Open() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

bool Link::writePacket(const Packet& p) {
    std::vector<uint8_t> buf(outLen_, 0);
    std::copy(p.begin(), p.end(), buf.begin() + 1);  // [0] is the report ID
    OVERLAPPED ov{};
    ov.hEvent = event_;
    ResetEvent(event_);
    DWORD done = 0;
    if (!WriteFile(handle_, buf.data(), DWORD(buf.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
        error_ = hexText("Write failed", GetLastError());
        return false;
    }
    if (WaitForSingleObject(event_, 1000) != WAIT_OBJECT_0) {
        CancelIoEx(handle_, &ov);
        GetOverlappedResult(handle_, &ov, &done, TRUE);
        error_ = "Write timed out";
        return false;
    }
    return GetOverlappedResult(handle_, &ov, &done, FALSE) != 0;
}

bool Link::readPacket(Packet& p, unsigned ms) {
    std::vector<uint8_t> buf(inLen_, 0);
    OVERLAPPED ov{};
    ov.hEvent = event_;
    ResetEvent(event_);
    DWORD done = 0;
    if (!ReadFile(handle_, buf.data(), DWORD(buf.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
        error_ = hexText("Read failed", GetLastError());
        return false;
    }
    if (WaitForSingleObject(event_, ms) != WAIT_OBJECT_0) {
        CancelIoEx(handle_, &ov);
        GetOverlappedResult(handle_, &ov, &done, TRUE);
        error_ = "No answer from the controller";
        return false;
    }
    if (!GetOverlappedResult(handle_, &ov, &done, FALSE) || done < kPacket + 1) {
        error_ = "Short read";
        return false;
    }
    std::copy(buf.begin() + 1, buf.begin() + 1 + kPacket, p.begin());
    return true;
}

bool Link::Send(const Bytes& command, const Bytes& data, Packet& reply) {
    if (!Open()) return false;
    HidD_FlushQueue(handle_);  // drop anything left over
    if (!writePacket(Request(command, data))) return false;
    // Skip reports that answer something else.
    for (int i = 0; i < 8; i++) {
        if (!readPacket(reply, 1000)) return false;
        if (Answers(reply, command)) return true;
    }
    error_ = "The controller answered something else";
    return false;
}

std::optional<Bytes> Link::ReadEndpoint(const Endpoint& e) {
    Packet a, b, c;
    // Close first in case something left it open; that may be refused.
    Send(cmd::kClose, e.id);
    if (!Send(cmd::kOpen, e.id)) return std::nullopt;
    bool ok = Send(cmd::kRead1, {}, a) && Send(cmd::kRead2, {}, b) && Send(cmd::kRead3, {}, c);
    Send(cmd::kClose, e.id);
    if (!ok) return std::nullopt;
    auto p = Payload(a, b, c, e.type);
    if (!p) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Endpoint %02x answered with type %02x %02x", e.id[0], a[3], a[4]);
        error_ = buf;
    }
    return p;
}

bool Link::WriteEndpoint(const Endpoint& e, const Bytes& data) {
    // Read it first: makes sure it's the kind of data expected here.
    if (!ReadEndpoint(e)) return false;
    Send(cmd::kClose, e.id);
    if (!Send(cmd::kOpen, e.id)) return false;
    bool ok = true;
    for (const auto& p : WritePackets(e.type, data)) {
        Bytes command(p.begin() + 1, p.begin() + 3);
        Bytes rest(p.begin() + 3, p.end());
        ok = ok && Send(command, rest);
    }
    Send(cmd::kClose, e.id);
    return ok;
}

bool IcueRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"iCUE.exe") == 0 || _wcsicmp(pe.szExeFile, L"Corsair.Service.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"CorsairDeviceControlService.exe") == 0;
    CloseHandle(snap);
    return found;
}

}  // namespace cooler
