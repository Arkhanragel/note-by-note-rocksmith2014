// log.h: a small thread-safe log file (NoteByNote.log next to the DLL).
#pragma once
#include <string>

namespace nbn {
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogClose();
}  // namespace nbn
