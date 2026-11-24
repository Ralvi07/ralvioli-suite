// Defines the plotter transport contract and implements file and legacy libusbK backends.
#pragma once
// Transport interface and legacy libusbK backend.
// Include after <windows.h>.

#include <cstdio>
#include <string>

struct Transport {
    virtual ~Transport() {}
    virtual bool open(std::string& err) = 0;
    virtual std::string description() const { return {}; }
    // 0 = ok, 1 = timeout, 2 = busy, -1 = error. Check the transferred count on failure.
    virtual int write(const unsigned char* p, size_t n, size_t& done, std::string& err) = 0;
    virtual bool read(unsigned char* p, size_t n, size_t& done, std::string& err) = 0;
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

    UINT vid;
    UINT pid;
    int forcePipe;
    int timeoutMs;
    HMODULE dll = nullptr;
    void* list = nullptr;
    void* dev = nullptr;
    void* h = nullptr;
    UCHAR pipe = 0;
    UCHAR pipeIn = 0;
    int forceIn = 0x81;
    LstInitFn lstInit = nullptr;
    LstFreeFn lstFree = nullptr;
    LstFindFn lstFind = nullptr;
    InitFn usbInit = nullptr;
    FreeFn usbFree = nullptr;
    QueryIfFn queryIf = nullptr;
    QueryPipeFn queryPipe = nullptr;
    WriteFn writePipe = nullptr;
    WriteFn readPipe = nullptr;
    PolicyFn setPolicy = nullptr;
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
        dll = LoadLibraryExW(L"libusbK.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll == nullptr) {
            err = "libusbK.dll was not found next to the program";
            return false;
        }
        if (!load(lstInit, "LstK_Init", err) || !load(lstFree, "LstK_Free", err) ||
            !load(lstFind, "LstK_FindByVidPid", err) || !load(usbInit, "UsbK_Init", err) ||
            !load(usbFree, "UsbK_Free", err) || !load(queryIf, "UsbK_QueryInterfaceSettings", err) ||
            !load(queryPipe, "UsbK_QueryPipe", err) || !load(writePipe, "UsbK_WritePipe", err) ||
            !load(readPipe, "UsbK_ReadPipe", err) || !load(setPolicy, "UsbK_SetPipePolicy", err)) {
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
            if (pi.type == 2 && (pi.id & 0x80) && !pipeIn && (forceIn < 0 || forceIn == pi.id)) {
                pipeIn = pi.id;
            }
            if (pi.type == 2 && (pi.id & 0x80) == 0 && found < 0 &&
                (forcePipe < 0 || forcePipe == pi.id)) {
                found = pi.id;
            }
        }
        if (found < 0 || !pipeIn) {
            err = "Missing matching bulk OUT or IN endpoint (" + info + ")";
            return false;
        }
        pipe = static_cast<UCHAR>(found);
        UINT tmo = static_cast<UINT>(timeoutMs);
        if (!setPolicy(h, pipe, 0x03, sizeof(tmo), &tmo) ||
            !setPolicy(h, pipeIn, 0x03, sizeof(tmo), &tmo)) {
            err = "libusbK could not set the transfer timeout: " + winErrorText(GetLastError());
            return false;
        }
        return true;
    }

    bool enableRasterEndpoints(std::string& error) {
        UINT timeout = static_cast<UINT>(timeoutMs);
        if (setPolicy(h, 0x02, 0x03, sizeof(timeout), &timeout) &&
            setPolicy(h, 0x82, 0x03, sizeof(timeout), &timeout)) return true;
        error = "Could not configure VEGA raster endpoints: " + winErrorText(GetLastError());
        return false;
    }

    int write(const unsigned char* p, size_t n, size_t& done, std::string& err) override {
        return writeEndpoint(pipe, p, n, done, err);
    }

    int writeEndpoint(UCHAR endpoint, const unsigned char* p, size_t n, size_t& done, std::string& err) {
        UINT got = 0;
        BOOL ok = writePipe(h, endpoint, const_cast<PUCHAR>(p), static_cast<UINT>(n), &got, nullptr);
        done = got;
        if (ok) {
            return 0;
        }
        DWORD e = GetLastError();
        if (e == ERROR_SEM_TIMEOUT || e == ERROR_TIMEOUT || e == WAIT_TIMEOUT) {
            err = "libusbK transfer timed out: " + winErrorText(e);
            return 1;
        }
        err = "libusbK transfer failed: " + winErrorText(e);
        return e == ERROR_BUSY ? 2 : -1;
    }

    bool read(unsigned char* p, size_t n, size_t& done, std::string& err) override {
        return readEndpoint(pipeIn, p, n, done, err);
    }

    bool readEndpoint(UCHAR endpoint, unsigned char* p, size_t n, size_t& done, std::string& err) {
        UINT got = 0;
        const BOOL ok = readPipe(h, endpoint, p, static_cast<UINT>(n), &got, nullptr);
        done = got;
        if (!ok) err = "libusbK read failed: " + winErrorText(GetLastError());
        return ok != FALSE;
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
