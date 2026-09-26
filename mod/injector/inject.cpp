// nbn_inject: a development tool that loads our DLL into the running Rocksmith2014.exe.
//
// HOW DLL INJECTION WORKS (the classic LoadLibrary method):
//   1. Find the game's process ID.
//   2. Allocate a bit of memory INSIDE the game and write the DLL's full path there.
//   3. Start a thread inside the game (CreateRemoteThread) whose start routine is
//      LoadLibraryW, with that path as its argument. Windows then loads our DLL into
//      the game and runs its DllMain.
// This works because kernel32.dll is loaded at the same address in every process of the same
// bitness during a boot session, so LoadLibraryW's address in our process is valid in the game.
// That's also why this injector must be 32-bit like the game.
//
// Usage: nbn_inject.exe [path\to\dll]   (default: NoteByNoteProbe.dll next to this exe)
// Later, the mod will load itself through a proxy DLL and this tool won't be needed.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

static DWORD FindProcess(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv) {
    // Resolve the DLL path: an argument, or the probe next to this exe.
    wchar_t path[MAX_PATH];
    if (argc > 1) {
        GetFullPathNameW(argv[1], MAX_PATH, path, nullptr);
    } else {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path);
        p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"NoteByNoteProbe.dll";
        wcscpy_s(path, p.c_str());
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"DLL not found: %s\n", path);
        return 1;
    }

    // Inject a uniquely named COPY from a "run" subfolder. Windows locks a loaded DLL, so this lets
    // us rebuild while the game is running, unload the old probe (F11) and inject the new one.
    // The probe writes its log next to itself, so it's always run\NoteByNoteProbe.log.
    {
        std::wstring src(path);
        std::wstring dir = src.substr(0, src.find_last_of(L"\\/") + 1) + L"run\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring copy = dir + L"NoteByNoteProbe_" + std::to_wstring(GetTickCount()) + L".dll";
        if (!CopyFileW(src.c_str(), copy.c_str(), FALSE)) { wprintf(L"Copy failed (%lu)\n", GetLastError()); return 1; }
        wcscpy_s(path, copy.c_str());
    }

    DWORD pid = FindProcess(L"Rocksmith2014.exe");
    if (!pid) {
        wprintf(L"Rocksmith2014.exe is not running. Start the game first.\n");
        return 1;
    }
    wprintf(L"Game PID %lu. Injecting %s\n", pid, path);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed (%lu). Try running as administrator.\n", GetLastError()); return 1; }

    size_t bytes = (wcslen(path) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(proc, remote, path, bytes, nullptr);

    auto loadLibrary = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) { wprintf(L"CreateRemoteThread failed (%lu)\n", GetLastError()); return 1; }

    WaitForSingleObject(thread, 10000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);  // LoadLibrary's return value (truncated HMODULE): 0 = failure
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);

    if (!module) { wprintf(L"LoadLibrary failed inside the game.\n"); return 1; }
    wprintf(L"OK: DLL loaded into the game. Log: the .log file with the same name as the injected copy, in run\\.\n");
    return 0;
}
