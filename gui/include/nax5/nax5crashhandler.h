#pragma once

#include <QString>

// Process-wide crash capture. On an unhandled exception, abort() or
// std::terminate() the handler writes <dir>/NAX5-<utc>-<pid>.dmp plus a text
// summary (.txt) with the exception code, faulting module and offset, then lets
// Windows Error Reporting continue as usual. Files are uploaded on the next
// start by nax5QueueCrashDumps(). No-op on other platforms.
QString nax5CrashDumpDir();
bool nax5InstallCrashHandler(const QString &dump_dir);
