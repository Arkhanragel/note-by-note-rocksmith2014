#include "report.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "log.h"

namespace nbn::report {

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
std::map<std::string, int> g_counts;  // Limited(): lines written per key

void Write(const char* msg) {
    Log("report: %s", msg);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "%s\n", msg);
    std::fflush(g_file);  // the tester may quit the game any moment
}
}  // namespace

void Open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file = _wfopen(path.c_str(), L"w");
    g_counts.clear();
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "Note-by-Note report, %04d-%02d-%02d %02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::fprintf(g_file, "(please send this file together with NoteByNote.log)\n\n");
    std::fflush(g_file);
}

void Close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void Line(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Write(msg);
}

void Limited(const char* key, int limit, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_counts[key]++ >= limit) return;
    }
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Write(msg);
}

}  // namespace nbn::report
