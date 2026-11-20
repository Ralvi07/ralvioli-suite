// Discovers the plotter's Windows device interface and implements the WinUSB transport.
#pragma once

#include <setupapi.h>
#include <winusb.h>
#include <vector>

enum class UsbPresence { Present, Missing, Unavailable };

inline UsbPresence plotterUsbPresence(UINT vid, UINT pid) {
    wchar_t target[32];
    swprintf(target, sizeof(target) / sizeof(target[0]), L"USB\\VID_%04X&PID_%04X", vid, pid);
    const size_t length = wcslen(target);
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) {
        return UsbPresence::Unavailable;
    }
    UsbPresence result = UsbPresence::Missing;
    for (DWORD index = 0; ; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) result = UsbPresence::Unavailable;
            break;
        }
        wchar_t id[256];
        if (SetupDiGetDeviceInstanceIdW(devices, &device, id, 256, nullptr) &&
            _wcsnicmp(id, target, length) == 0 && (id[length] == L'\\' || id[length] == L'&')) {
            result = UsbPresence::Present;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

struct WinUsbTransport : Transport {
    UINT vid;
    UINT pid;
    int forcePipe;
    ULONG timeoutMs;
    HANDLE device = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    UCHAR pipe = 0;
    std::string info;

    WinUsbTransport(UINT v, UINT p, int endpoint, int timeout)
        : vid(v), pid(p), forcePipe(endpoint), timeoutMs(static_cast<ULONG>(timeout)) {}
    ~WinUsbTransport() override { close(); }
    std::string description() const override { return info; }

    bool open(std::string& err) override {
        // DeviceInterfaceGUIDs in Algotex-Printer-WinUSB.inf.
        const GUID plotterGuid = {0x1f2af5bc, 0xecf9, 0xc67c,
                                  {0x68, 0xb2, 0x2f, 0xd9, 0xeb, 0x31, 0xda, 0xa2}};
        HDEVINFO devices = SetupDiGetClassDevsW(&plotterGuid, nullptr, nullptr,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) {
            err = "WinUSB device enumeration failed: " + winErrorText(GetLastError());
            return false;
        }

        wchar_t id[32];
        swprintf(id, sizeof(id) / sizeof(id[0]), L"usb#vid_%04x&pid_%04x", vid, pid);
        err = "No matching WinUSB plotter interface found. Check the cable and Windows driver.";
        bool opened = false;
        for (DWORD index = 0; !opened; ++index) {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &plotterGuid, index, &iface)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_ITEMS) {
                    err = "WinUSB interface enumeration failed: " + winErrorText(error);
                }
                break;
            }
            DWORD bytes = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &bytes, nullptr);
            if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                continue;
            }
            std::vector<BYTE> buffer(bytes);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(*detail);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, bytes, nullptr, nullptr) ||
                _wcsnicmp(detail->DevicePath, L"\\\\?\\", 4) != 0 ||
                _wcsnicmp(detail->DevicePath + 4, id, wcslen(id)) != 0) {
                continue;
            }
            wchar_t suffix = detail->DevicePath[4 + wcslen(id)];
            if (suffix != L'#' && suffix != L'&') {
                continue;
            }
            device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            if (device == INVALID_HANDLE_VALUE || !WinUsb_Initialize(device, &usb)) {
                err = "WinUSB could not open the plotter: " + winErrorText(GetLastError());
                close();
                continue;
            }
            opened = selectPipe(err);
            if (!opened) {
                close();
            }
        }
        SetupDiDestroyDeviceInfoList(devices);
        return opened;
    }

    bool selectPipe(std::string& err) {
        USB_INTERFACE_DESCRIPTOR iface{};
        if (!WinUsb_QueryInterfaceSettings(usb, 0, &iface)) {
            err = "WinUSB could not read the plotter interface: " + winErrorText(GetLastError());
            return false;
        }
        char line[96];
        snprintf(line, sizeof(line), "WinUSB interface %u", iface.bInterfaceNumber);
        info = line;
        pipe = 0;
        for (UCHAR index = 0; index < iface.bNumEndpoints; ++index) {
            WINUSB_PIPE_INFORMATION endpoint{};
            if (!WinUsb_QueryPipe(usb, 0, index, &endpoint) || endpoint.PipeType != UsbdPipeTypeBulk) {
                continue;
            }
            snprintf(line, sizeof(line), "; bulk %s 0x%02X (%u bytes)",
                     endpoint.PipeId & 0x80 ? "IN" : "OUT", endpoint.PipeId, endpoint.MaximumPacketSize);
            info += line;
            if (!(endpoint.PipeId & 0x80) && (forcePipe < 0 || endpoint.PipeId == forcePipe) && !pipe) {
                pipe = endpoint.PipeId;
            }
        }
        if (!pipe) {
            err = "No matching bulk OUT endpoint (" + info + ")";
            return false;
        }
        if (!WinUsb_SetPipePolicy(usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs)) {
            err = "WinUSB could not set the transfer timeout: " + winErrorText(GetLastError());
            return false;
        }
        snprintf(line, sizeof(line), "; selected OUT 0x%02X", pipe);
        info += line;
        return true;
    }

    int write(const unsigned char* data, size_t size, size_t& done, std::string& err) override {
        ULONG transferred = 0;
        BOOL ok = WinUsb_WritePipe(usb, pipe, const_cast<PUCHAR>(data),
                                   static_cast<ULONG>(size), &transferred, nullptr);
        done = transferred;
        if (ok && transferred) {
            return 0;
        }
        if (ok) {
            err = "WinUSB transferred zero bytes.";
        } else {
            err = "WinUSB transfer failed: " + winErrorText(GetLastError());
        }
        // Without a device acknowledgment, retrying a failed transfer could duplicate data.
        return -1;
    }

    void close() override {
        if (usb) {
            WinUsb_Free(usb);
            usb = nullptr;
        }
        if (device != INVALID_HANDLE_VALUE) {
            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
        }
    }
};
