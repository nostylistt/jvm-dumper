// heap_dumper.cpp — injectable DLL that dumps the full JVM heap on demand.
//
// Hotkeys (inside target JVM process):
//   F8  = take a dump (each press -> new heap_dumpN.bin + heap_dumpN_regions.txt in %TEMP%)
//   END = unload the DLL
//
// Dump layout (identical across presses, so analyzers can rely on it):
//   Region #00 = whole configured heap range as a SPARSE file:
//                committed pages are written at (VA - HEAP_LO), gaps stay as zeros.
//                File size is exactly (HEAP_HI - HEAP_LO), even if most of it is empty.
//   Region #01..N = committed private/mapped regions outside the heap (class space, metaspace,
//                   JVM internals), each up to 256 MB, appended sequentially.
// The companion heap_dumpN_regions.txt is an index: Region # | VA range | size | file offset.
//
// Defaults assume HotSpot with ~4 GB max heap and the common base 0x6C0000000..0x7C0000000.
// Override with env vars HEAP_LO / HEAP_HI (hex, no 0x) if your JVM uses a different base.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>

static HMODULE  g_h;
static uint64_t g_heapLo = 0x6C0000000ull;
static uint64_t g_heapHi = 0x7C0000000ull;
static const uint64_t CHUNK = 64ull << 20;   // 64 MB write chunk

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a);
    va_end(a);
    wprintf(L"%ls\n", buf);
    OutputDebugStringW(buf);
    fflush(stdout);
}

static uint64_t umin(uint64_t a, uint64_t b) { return a < b ? a : b; }

static void seek(HANDLE h, uint64_t off) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
}

// Writes [base,end) of live memory to file at the current position. Returns true on full success.
static bool writeRange(HANDLE h, uint64_t base, uint64_t end) {
    uint64_t off = base;
    while (off < end) {
        DWORD chunk = (DWORD)umin(CHUNK, end - off), wr = 0;
        if (!WriteFile(h, (LPCVOID)off, chunk, &wr, nullptr) || wr != chunk) return false;
        off += chunk;
    }
    return true;
}

static bool readable(const MEMORY_BASIC_INFORMATION& m) {
    return m.State == MEM_COMMIT
        && (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                       | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static std::string TempPath() {
    char tmp[MAX_PATH] = {};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) { strcpy(tmp, "C:\\Temp\\"); }
    return std::string(tmp);
}

static bool DumpOne(int n) {
    std::string dir = TempPath();
    char bin[MAX_PATH], idxp[MAX_PATH];
    snprintf(bin,  sizeof(bin),  "%sheap_dump%d.bin",          dir.c_str(), n);
    snprintf(idxp, sizeof(idxp), "%sheap_dump%d_regions.txt",  dir.c_str(), n);

    HANDLE hf = CreateFileA(bin, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { Log(L"[!] cannot create %S", bin); return false; }

    std::string idx = "=== JVM HEAP REGIONS DUMP INDEX ===\n";
    idx += "Target Dump File: "; idx += bin; idx += "\n";
    idx += "Format: [Region #] BaseAddress - EndAddress | Size (MB) | DumpFileOffset\n";
    idx += "--------------------------------------------------------------------\n";

    char line[256]; int regN = 0; uint64_t total = 0;
    MEMORY_BASIC_INFORMATION mbi;

    // --- Region #00: whole heap as sparse ---
    ULONGLONG t = GetTickCount64();
    for (uint64_t p = g_heapLo; p < g_heapHi; ) {
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        uint64_t base = (uint64_t)mbi.BaseAddress;
        uint64_t next = base + mbi.RegionSize;
        if (readable(mbi)) {
            uint64_t a = base < g_heapLo ? g_heapLo : base;
            uint64_t b = next > g_heapHi ? g_heapHi : next;
            seek(hf, a - g_heapLo);
            writeRange(hf, a, b);
        }
        p = next > p ? next : p + 0x1000;
    }
    seek(hf, g_heapHi - g_heapLo);
    SetEndOfFile(hf);

    snprintf(line, sizeof(line),
             "Region #%02d: 0x%012llX - 0x%012llX | %6.1f MB | FileOffset: 0x%012llX\n",
             regN++, (unsigned long long)g_heapLo, (unsigned long long)g_heapHi,
             (g_heapHi - g_heapLo) / 1048576.0, 0ull);
    idx += line;
    total += g_heapHi - g_heapLo;
    uint64_t fileOff = g_heapHi - g_heapLo;

    // --- Extra regions: committed private/mapped areas outside heap (class/meta/etc) ---
    for (uint64_t p = 0; p < 0x7FFFFFFF0000ull; ) {
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        uint64_t base = (uint64_t)mbi.BaseAddress;
        uint64_t next = base + mbi.RegionSize;
        bool inHeap = base >= g_heapLo && base < g_heapHi;
        if (readable(mbi) && !inHeap && mbi.Type != MEM_IMAGE
            && mbi.RegionSize <= (256ull << 20)) {
            seek(hf, fileOff);
            if (writeRange(hf, base, next)) {
                snprintf(line, sizeof(line),
                         "Region #%02d: 0x%012llX - 0x%012llX | %6.1f MB | FileOffset: 0x%012llX\n",
                         regN++, (unsigned long long)base, (unsigned long long)next,
                         mbi.RegionSize / 1048576.0, (unsigned long long)fileOff);
                idx += line;
                total += mbi.RegionSize;
                fileOff += mbi.RegionSize;
            }
        }
        p = next > p ? next : p + 0x1000;
    }
    CloseHandle(hf);

    idx += "--------------------------------------------------------------------\n";
    snprintf(line, sizeof(line),
             "Total Dumped Regions: %d\nTotal Dumped Size: %.2f MB\n",
             regN, total / 1048576.0);
    idx += line;

    HANDLE hi = CreateFileA(idxp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hi != INVALID_HANDLE_VALUE) {
        DWORD wr; WriteFile(hi, idx.data(), (DWORD)idx.size(), &wr, nullptr);
        CloseHandle(hi);
    }

    Log(L"[OK] dump #%d done in %llu ms: %S (%.0f MB, %d regions)",
        n, GetTickCount64() - t, bin, total / 1048576.0, regN);
    return true;
}

static void ApplyEnvOverrides() {
    char buf[32] = {};
    if (GetEnvironmentVariableA("HEAP_LO", buf, sizeof(buf)) > 0) {
        g_heapLo = _strtoui64(buf, nullptr, 16);
    }
    if (GetEnvironmentVariableA("HEAP_HI", buf, sizeof(buf)) > 0) {
        g_heapHi = _strtoui64(buf, nullptr, 16);
    }
}

static DWORD WINAPI Worker(LPVOID) {
    AllocConsole();
    FILE* fp; freopen_s(&fp, "CONOUT$", "w", stdout);
    SetConsoleTitleW(L"JVM Heap Dumper");

    ApplyEnvOverrides();

    Log(L"====== JVM Heap Dumper ======");
    Log(L"Heap range: 0x%012llX - 0x%012llX (%.1f GB)",
        (unsigned long long)g_heapLo, (unsigned long long)g_heapHi,
        (g_heapHi - g_heapLo) / 1073741824.0);
    Log(L"F8  = take full heap dump (heap_dumpN.bin + _regions.txt in %%TEMP%%).");
    Log(L"END = unload.\n");
    Log(L"Each dump equals the heap range size on disk (sparse). Watch free space.\n");

    int n = 0; bool f8 = false;
    while (!(GetAsyncKeyState(VK_END) & 0x8000)) {
        bool now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (now && !f8) {
            ++n;
            Log(L"[..] taking dump #%d ...", n);
            DumpOne(n);
        }
        f8 = now;
        Sleep(30);
    }

    Log(L"\n[INFO] unloaded.");
    FreeConsole();
    FreeLibraryAndExitThread(g_h, 0);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_h = h;
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
