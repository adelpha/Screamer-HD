// Debug builds: every CRT assertion also writes its message and the call stack to
// assert.txt next to the executable (and stderr), so a dialog seen on screen can be traced.
#include "debug_report.h"

#if defined(_MSC_VER) && defined(_DEBUG)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::string g_path;

int __cdecl report_hook(int type, char* message, int* ret) {
    if (type != _CRT_ASSERT && type != _CRT_ERROR) return FALSE;
    HANDLE proc = GetCurrentProcess();
    static bool sym_ready = false;
    if (!sym_ready) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        sym_ready = SymInitialize(proc, nullptr, TRUE) != FALSE;
    }
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    std::string text = message ? message : "";
    text += "stack:\n";
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    for (USHORT i = 0; i < n; ++i) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        char line[512];
        IMAGEHLP_LINE64 li{};
        li.SizeOfStruct = sizeof li;
        DWORD disp = 0;
        if (sym_ready && SymFromAddr(proc, addr, nullptr, sym)) {
            if (SymGetLineFromAddr64(proc, addr, &disp, &li))
                std::snprintf(line, sizeof line, "  %s  %s:%lu\n", sym->Name, li.FileName, li.LineNumber);
            else
                std::snprintf(line, sizeof line, "  %s\n", sym->Name);
        } else {
            std::snprintf(line, sizeof line, "  0x%llx\n", static_cast<unsigned long long>(addr));
        }
        text += line;
    }
    std::fputs(text.c_str(), stderr);
    if (std::FILE* f = g_path.empty() ? nullptr : std::fopen(g_path.c_str(), "a")) {
        std::fputs(text.c_str(), f);
        std::fputs("\n", f);
        std::fclose(f);
    }
    if (ret) *ret = 0;
    return FALSE;  // the usual report (dialog, or stderr in headless runs) follows
}

} // namespace

void install_debug_report(bool no_dialogs) {
    if (no_dialogs) {
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
    char exe[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p(exe, len);
    const size_t slash = p.find_last_of("/\\");
    g_path = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "assert.txt";
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, report_hook);
}
#else
void install_debug_report(bool) {}
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>

void attach_parent_console() {
    auto missing = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h == nullptr || h == INVALID_HANDLE_VALUE;
    };
    const bool out = missing(STD_OUTPUT_HANDLE), err = missing(STD_ERROR_HANDLE);
    if (!out && !err) return;  // pipes or files given: they stay
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    std::FILE* f = nullptr;
    if (out) freopen_s(&f, "CONOUT$", "w", stdout);
    if (err) freopen_s(&f, "CONOUT$", "w", stderr);
}
#else
void attach_parent_console() {}
#endif
