// Defines the plotter transport contract and implements file and legacy libusbK backends.
#pragma once
// Transport interface and legacy libusbK and dry-run backends.
// Include after <windows.h>.

#include <cstdio>
#include <string>

struct Transport {
    virtual ~Transport() {}
    virtual bool open(std::string& err) = 0;
    virtual std::string description() const { return {}; }
    // 0 = ok, 1 = timeout (done may be partial), -1 = error
    virtual int write(const unsigned char* p, size_t n, size_t& done, std::string& err) = 0;
    virtual void close() {}
};

inline std::string winErrorText(DWORD e) {
    wchar_t* msg = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string s;
    if (len > 0 && msg != nullptr) {
        int n = WideCharToMultiByte(CP_UTF8, 0, msg, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        s.assign(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg, static_cast<int>(len), &s[0], n, nullptr, nullptr);
    }
    if (msg != nullptr) {
        LocalFree(msg);
    }
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    char num[40];
    snprintf(num, sizeof(num), " (Win32 %lu)", static_cast<unsigned long>(e));
    return (s.empty() ? std::string("error") : s) + num;
}

struct FileTransport : Transport {
    std::wstring path;
    FILE* f = nullptr;
    explicit FileTransport(const std::wstring& p) : path(p) {}
    ~FileTransport() override { close(); }
    bool open(std::string& err) override {
        f = _wfopen(path.c_str(), L"wb");
        if (f == nullptr) {
            err = "The test file could not be written";
        }
        return f != nullptr;
    }
    int write(const unsigned char* p, size_t n, size_t& done, std::string& err) override {
        done = fwrite(p, 1, n, f);
        if (done != n || fflush(f) != 0) {
            err = "Could not write the dry-run output file.";
            return -1;
        }
        Sleep(5);
        return 0;
    }
    void close() override {
        if (f != nullptr) {
            fclose(f);
        }
        f = nullptr;
    }
};

#pragma pack(push, 1)
struct UsbIfDesc {
    BYTE bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass,
        bInterfaceSubClass, bInterfaceProtocol, iInterface;
};
#pragma pack(pop)
struct UsbPipeInfo {      // same layout as WINUSB_PIPE_INFORMATION
    int type;
    UCHAR id;
    USHORT maxPacket;
    UCHAR interval;
};

struct LibUsbKTransport : Transport {
    typedef BOOL(WINAPI* LstInitFn)(void**, UINT);
    typedef BOOL(WINAPI* LstFreeFn)(void*);
    typedef BOOL(WINAPI* LstFindFn)(void*, UINT, UINT, void**);
    typedef BOOL(WINAPI* InitFn)(void**, void*);
    typedef BOOL(WINAPI* FreeFn)(void*);
    typedef BOOL(WINAPI* QueryIfFn)(void*, UCHAR, UsbIfDesc*);
    typedef BOOL(WINAPI* QueryPipeFn)(void*, UCHAR, UCHAR, UsbPipeInfo*);
    typedef BOOL(WINAPI* WriteFn)(void*, UCHAR, PUCHAR, UINT, UINT*, LPOVERLAPPED);
    typedef BOOL(WINAPI* PolicyFn)(void*, UCHAR, UINT, UINT, PVOID);
    typedef BOOL(WINAPI* ResetFn)(void*, UCHAR);

    UINT vid;
    UINT pid;
    int forcePipe;
    int timeoutMs;
    HMODULE dll = nullptr;
    void* list = nullptr;
    void* dev = nullptr;
    void* h = nullptr;
    UCHAR pipe = 0;
    LstInitFn lstInit = nullptr;
    LstFreeFn lstFree = nullptr;
    LstFindFn lstFind = nullptr;
    InitFn usbInit = nullptr;
    FreeFn usbFree = nullptr;
    QueryIfFn queryIf = nullptr;
    QueryPipeFn queryPipe = nullptr;
    WriteFn writePipe = nullptr;
    PolicyFn setPolicy = nullptr;
    ResetFn resetPipe = nullptr;
    std::string info;

    LibUsbKTransport(UINT v, UINT p, int pipeId, int tmo) : vid(v), pid(p), forcePipe(pipeId), timeoutMs(tmo) {}
    ~LibUsbKTransport() override { close(); }
    std::string description() const override { return "libusbK " + info; }

    template <class T>
    bool load(T& fn, const char* name, std::string& err) {
        fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
        if (fn == nullptr) {
            err = std::string("libusbK.dll has no ") + name;
        }
        return fn != nullptr;
    }

    bool open(std::string& err) override {
        dll = LoadLibraryW(L"libusbK.dll");
        if (dll == nullptr) {
            err = "libusbK.dll was not found next to the program";
            return false;
        }
        if (!load(lstInit, "LstK_Init", err) || !load(lstFree, "LstK_Free", err) ||
            !load(lstFind, "LstK_FindByVidPid", err) || !load(usbInit, "UsbK_Init", err) ||
            !load(usbFree, "UsbK_Free", err) || !load(queryIf, "UsbK_QueryInterfaceSettings", err) ||
            !load(queryPipe, "UsbK_QueryPipe", err) || !load(writePipe, "UsbK_WritePipe", err) ||
            !load(setPolicy, "UsbK_SetPipePolicy", err) || !load(resetPipe, "UsbK_ResetPipe", err)) {
            return false;
        }
        if (!lstInit(&list, 0)) {
            err = "Device list failed: " + winErrorText(GetLastError());
            return false;
        }
        if (!lstFind(list, vid, pid, &dev)) {
            err = "The plotter was not found. Check the USB cable and the libusbK driver.";
            return false;
        }
        if (!usbInit(&h, dev)) {
            err = "The plotter could not be opened: " + winErrorText(GetLastError());
            return false;
        }
        UsbIfDesc ifd;
        ZeroMemory(&ifd, sizeof(ifd));
        if (!queryIf(h, 0, &ifd)) {
            err = "Cannot read the plotter interface: " + winErrorText(GetLastError());
            return false;
        }
        char line[160];
        snprintf(line, sizeof(line), "interface %u, %u endpoints", ifd.bInterfaceNumber, ifd.bNumEndpoints);
        info = line;
        int found = -1;
        for (UCHAR i = 0; i < ifd.bNumEndpoints; ++i) {
            UsbPipeInfo pi;
            ZeroMemory(&pi, sizeof(pi));
            if (!queryPipe(h, 0, i, &pi)) {
                continue;
            }
            snprintf(line, sizeof(line), "; pipe 0x%02X type %d max %u", pi.id, pi.type, pi.maxPacket);
            info += line;
            if (pi.type == 2 && (pi.id & 0x80) == 0 && found < 0 &&
                (forcePipe < 0 || forcePipe == pi.id)) {
                found = pi.id;
            }
        }
        if (found < 0) {
            err = "No bulk OUT endpoint found (" + info + ")";
            return false;
        }
        pipe = static_cast<UCHAR>(found);
        UINT tmo = static_cast<UINT>(timeoutMs);
        if (!setPolicy(h, pipe, 0x03, sizeof(tmo), &tmo)) {
            err = "libusbK could not set the transfer timeout: " + winErrorText(GetLastError());
            return false;
        }
        return true;
    }

    int write(const unsigned char* p, size_t n, size_t& done, std::string& err) override {
        UINT got = 0;
        BOOL ok = writePipe(h, pipe, const_cast<PUCHAR>(p), static_cast<UINT>(n), &got, nullptr);
        done = got;
        if (ok) {
            return 0;
        }
        DWORD e = GetLastError();
        if (e == ERROR_SEM_TIMEOUT || e == ERROR_TIMEOUT || e == WAIT_TIMEOUT) {
            err = "libusbK transfer timed out: " + winErrorText(e);
            return 1;
        }
        resetPipe(h, pipe);
        err = "libusbK transfer failed: " + winErrorText(e);
        return -1;
    }

    void close() override {
        if (h != nullptr) {
            usbFree(h);
            h = nullptr;
        }
        if (list != nullptr) {
            lstFree(list);
            list = nullptr;
        }
        if (dll != nullptr) {
            FreeLibrary(dll);
            dll = nullptr;
        }
    }
};
