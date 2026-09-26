#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace nbn {

static FILE* g_file = nullptr;
static std::mutex g_mutex;
static DWORD g_t0 = 0;

void LogOpen(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file = _wfopen(path.c_str(), L"w");
    g_t0 = GetTickCount();
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "[%9.3f] %s\n", (GetTickCount() - g_t0) / 1000.0, msg);
    std::fflush(g_file);  // keep the log useful even if the game crashes
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

}  // namespace nbn
