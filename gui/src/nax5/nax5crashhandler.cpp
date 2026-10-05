#include "nax5/nax5crashhandler.h"
#include "sessionlog.h"

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>

namespace {
using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Everything the crashing thread touches is prepared at install time: after a
// crash the heap, loader lock and the crashing stack may all be unusable.
wchar_t dump_dir[MAX_PATH];
MiniDumpWriteDumpFn write_dump = nullptr;
HANDLE request_event = nullptr;
HANDLE done_event = nullptr;
volatile LONG crashed = 0;
EXCEPTION_POINTERS *crash_pointers = nullptr;
DWORD crash_thread = 0;
const char *crash_reason = "";

void appendText(char *buf, size_t size, const char *format, ...)
{
    const size_t used = strlen(buf);
    if (used + 1 >= size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(buf + used, size - used, format, args);
    va_end(args);
}

void writeSummary(const wchar_t *path, const SYSTEMTIME &utc)
{
    char text[4096] = {};
    appendText(text, sizeof(text), "reason=%s\nutc=%04u-%02u-%02uT%02u:%02u:%02uZ\npid=%lu\nthread_id=%lu\n",
        crash_reason, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        GetCurrentProcessId(), crash_thread);
    if (crash_pointers && crash_pointers->ExceptionRecord)
    {
        const EXCEPTION_RECORD *record = crash_pointers->ExceptionRecord;
        appendText(text, sizeof(text), "exception_code=0x%08lX\nexception_flags=0x%08lX\nexception_address=0x%p\n",
            record->ExceptionCode, record->ExceptionFlags, record->ExceptionAddress);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            appendText(text, sizeof(text), "access=%s\naccess_address=0x%p\n",
                record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute",
                reinterpret_cast<void *>(record->ExceptionInformation[1]));
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(record->ExceptionAddress), &module) && module)
        {
            wchar_t module_path[MAX_PATH] = {};
            char module_utf8[MAX_PATH * 3] = {};
            GetModuleFileNameW(module, module_path, MAX_PATH);
            WideCharToMultiByte(CP_UTF8, 0, module_path, -1, module_utf8, sizeof(module_utf8), nullptr, nullptr);
            appendText(text, sizeof(text), "fault_module=%s\nfault_module_base=0x%p\nfault_offset=0x%llX\n", module_utf8,
                reinterpret_cast<void *>(module),
                static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) - reinterpret_cast<ULONG_PTR>(module)));
        }
        else
            appendText(text, sizeof(text), "fault_module=unknown\n");
    }
    appendText(text, sizeof(text), "dump=%s\n", write_dump ? "requested" : "dbghelp_unavailable");

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, DWORD(strlen(text)), &written, nullptr);
    CloseHandle(file);
}

// One more line for the summary, written after the fact: a summary that says "requested" and nothing
// else means the dump call never returned (seen in the field: the crashing thread held a lock).
void appendSummaryLine(const wchar_t *path, const char *line)
{
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, DWORD(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

// Runs on its own healthy stack, so stack overflows are captured too.
DWORD WINAPI dumpThread(LPVOID)
{
    WaitForSingleObject(request_event, INFINITE);
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    wchar_t base[MAX_PATH + 64];
    swprintf(base, sizeof(base) / sizeof(base[0]), L"%ls\\NAX5-%04u%02u%02u-%02u%02u%02u-%lu",
        dump_dir, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, GetCurrentProcessId());
    wchar_t path[MAX_PATH + 72];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls.txt", base);
    writeSummary(path, utc);

    if (write_dump)
    {
        wchar_t summary_path[MAX_PATH + 72];
        wcscpy(summary_path, path);
        swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls.dmp", base);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            char line[64] = {};
            appendText(line, sizeof(line), "dump_result=create_failed error=%lu\n", GetLastError());
            appendSummaryLine(summary_path, line);
        }
        else
        {
            MINIDUMP_EXCEPTION_INFORMATION info{crash_thread, crash_pointers, FALSE};
            // Stacks, threads and module list only: no heap, so session tokens and
            // Remote Play keys held in heap objects stay out of the dump.
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo
                | MiniDumpWithUnloadedModules);
            const BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                crash_pointers ? &info : nullptr, nullptr, nullptr);
            const DWORD error = ok ? 0 : GetLastError();
            CloseHandle(file);
            char line[64] = {};
            appendText(line, sizeof(line), ok ? "dump_result=ok\n" : "dump_result=write_failed error=0x%08lX\n", error);
            appendSummaryLine(summary_path, line);
        }
    }
    SetEvent(done_event);
    return 0;
}

void capture(EXCEPTION_POINTERS *pointers, const char *reason)
{
    if (InterlockedExchange(&crashed, 1) != 0) return;
    crash_pointers = pointers;
    crash_thread = GetCurrentThreadId();
    crash_reason = reason;
    SetEvent(request_event);
    WaitForSingleObject(done_event, 60000);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS *pointers)
{
    capture(pointers, "unhandled_exception");
    // Continue to Windows Error Reporting so the Application event log still gets the crash.
    return EXCEPTION_CONTINUE_SEARCH;
}

void abortHandler(int)
{
    capture(nullptr, "abort");
}

void terminateHandler()
{
    capture(nullptr, "std_terminate");
    std::abort();
}
}

QString nax5CrashDumpDir()
{
    const QString base = GetLogBaseDir();
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("crash-dumps"));
}

bool nax5InstallCrashHandler(const QString &dir)
{
    if (dir.isEmpty() || !QDir().mkpath(dir)) return false;
    const QString native = QDir::toNativeSeparators(QDir(dir).absolutePath());
    if (native.size() >= MAX_PATH) return false;
    native.toWCharArray(dump_dir);
    dump_dir[native.size()] = L'\0';

    // System dbghelp only; loaded now because LoadLibrary is unsafe after a crash.
    wchar_t system_dir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH - 16)
    {
        wcscat(system_dir, L"\\dbghelp.dll");
        if (HMODULE dbghelp = LoadLibraryW(system_dir))
            write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    request_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!request_event || !done_event) return false;
    HANDLE thread = CreateThread(nullptr, 256 * 1024, dumpThread, nullptr, 0, nullptr);
    if (!thread) return false;
    CloseHandle(thread);

    SetUnhandledExceptionFilter(unhandledFilter);
    std::signal(SIGABRT, abortHandler);
    std::set_terminate(terminateHandler);
    return true;
}

#else

QString nax5CrashDumpDir() { return QString(); }
bool nax5InstallCrashHandler(const QString &) { return false; }

#endif
