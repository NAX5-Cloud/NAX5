#pragma once

#include <QString>

// Product mode allows one NAX5 client per Windows user. A second launch asks the
// running one to show its window and exits: two clients of one player sent two
// telemetry streams and two probes and shared one report journal (u92, 28.09).
// Returns false when another instance is already running (the caller exits).
bool nax5AcquireSingleInstance(const QString &name);

// Server name per OS user, so different Windows accounts do not block each other.
QString nax5SingleInstanceName();
