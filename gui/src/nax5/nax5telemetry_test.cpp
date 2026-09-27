#include "nax5/nax5telemetry.h"

#include "nax5/nax5clientreport.h"
#include "nax5/nax5processlog.h"
#include "nax5/nax5reportqueue.h"
#include "nax5/nax5pathprobe.h"
#include "nax5/nax5streamhealth.h"

#include <QByteArray>
#include <QDateTime>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QtGlobal>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <cmath>
#include <cstdio>

static QString test_log_dir;
QString GetLogBaseDir()
{
    return test_log_dir;
}

static int g_failed = 0;

static void expect(bool condition, const char *name)
{
    if (condition)
        std::printf("ok %s\n", name);
    else
    {
        std::fprintf(stderr, "FAIL %s\n", name);
        g_failed++;
    }
}

static void test_payload_has_version_and_sha()
{
    const QByteArray body = nax5BuildClientEventBatch(QStringLiteral("APP_STARTED"));
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonObject installation = root.value(QStringLiteral("installation")).toObject();
    const QJsonObject event = root.value(QStringLiteral("events")).toArray().at(0).toObject();
    expect(installation.value(QStringLiteral("client_version")).toString() == QStringLiteral("testver"), "installation version is NAX5 product version");
    expect(installation.value(QStringLiteral("client_version")).toString() != QStringLiteral("1.10.0"), "installation version is not chiaki");
    expect(installation.value(QStringLiteral("client_sha")).toString() == QStringLiteral("testsha"), "installation sha is git short hash");
    expect(event.value(QStringLiteral("client_version")).toString() == QStringLiteral("testver"), "event version is NAX5 product version");
    expect(event.value(QStringLiteral("client_sha")).toString() == QStringLiteral("testsha"), "event sha is git short hash");
    expect(!installation.value(QStringLiteral("installation_id")).toString().isEmpty(), "installation id present");
    expect(nax5InstallationId() == nax5InstallationId(), "installation id persists across reads");
}

static void test_installation_os_and_locale_are_real()
{
    const QByteArray body = nax5BuildClientEventBatch(QStringLiteral("APP_STARTED"));
    const QJsonObject installation = QJsonDocument::fromJson(body).object().value(QStringLiteral("installation")).toObject();
    const QString os_version = installation.value(QStringLiteral("os_version")).toString();
    const QString locale = installation.value(QStringLiteral("locale")).toString();
    expect(os_version != QStringLiteral("windows"), "os_version is not the windows stub");
    expect(!os_version.isEmpty(), "os_version is populated");
    expect(os_version.contains(QRegularExpression(QStringLiteral("\\d"))), "os_version contains a digit");
    expect(locale == QLocale::system().name(), "locale matches QLocale::system");
    expect(locale != QStringLiteral("ru-RU") || QLocale::system().name() == QStringLiteral("ru-RU"), "locale is not a hardcoded ru-RU stub");
}

static void test_report_has_product_version()
{
    const QByteArray body = nax5BuildClientReportJson(Nax5ClientReportKindQuit);
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    expect(root.value(QStringLiteral("client_version")).toString() == QStringLiteral("testver"), "report version is NAX5 product version");
    expect(root.value(QStringLiteral("client_sha")).toString() == QStringLiteral("testsha"), "report sha is git short hash");
    expect(nax5ClientVersion() == QStringLiteral("testver"), "nax5ClientVersion is product version");
    expect(nax5BuildInfoText().contains(QStringLiteral("client_version=testver")), "build info has product version");
    expect(!nax5BuildInfoText().contains(QStringLiteral("client_version=1.10.0")), "build info version is not chiaki");
}

static bool hasLine(const QString &text, const QString &prefix)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines)
    {
        if (line.startsWith(prefix))
            return true;
    }
    return false;
}

static void test_build_info_has_diagnostics_fields()
{
    const QString text = nax5BuildInfoText();
    expect(hasLine(text, QStringLiteral("decoder=")), "build info has decoder");
    expect(hasLine(text, QStringLiteral("resolution=")), "build info has resolution");
    expect(hasLine(text, QStringLiteral("bitrate=")), "build info has bitrate");
    expect(hasLine(text, QStringLiteral("fps=")), "build info has fps");
    expect(hasLine(text, QStringLiteral("show_stream_stats=")), "build info has show_stream_stats");
    expect(hasLine(text, QStringLiteral("log_verbose=")), "build info has log_verbose");
    expect(hasLine(text, QStringLiteral("log_sanitize=")), "build info has log_sanitize");
    expect(hasLine(text, QStringLiteral("os_platform=")), "build info has os_platform");
    expect(hasLine(text, QStringLiteral("os_version=")), "build info has os_version");
    expect(hasLine(text, QStringLiteral("os_build=")), "build info has os_build");
    expect(hasLine(text, QStringLiteral("locale=")), "build info has locale");
    expect(hasLine(text, QStringLiteral("network_adapter_type=")), "build info has network_adapter_type");
    expect(hasLine(text, QStringLiteral("network_adapter_name=")), "build info has network_adapter_name");
    expect(hasLine(text, QStringLiteral("network_link_speed_mbps=")), "build info has network_link_speed_mbps");
    expect(hasLine(text, QStringLiteral("network_active=")), "build info has network_active");
    expect(hasLine(text, QStringLiteral("avg_packet_loss=")), "build info has avg_packet_loss");
    expect(hasLine(text, QStringLiteral("dropped_frames=")), "build info has dropped_frames");
    expect(text.contains(QStringLiteral("render_dropped_semantics=session_total_renderer_drops\n")),
        "build info says dropped frames are a session total");
    expect(text.contains(QStringLiteral("measured_bitrate_semantics=session_mean_kbps_of_measured_seconds\n")),
        "build info says bitrate is a session mean");
    expect(hasLine(text, QStringLiteral("frames_lost=")), "build info has frames_lost");
    expect(hasLine(text, QStringLiteral("measured_bitrate=")), "build info has measured_bitrate");
    expect(hasLine(text, QStringLiteral("session_duration_sec=")), "build info has session_duration_sec");
    expect(hasLine(text, QStringLiteral("stream_connected=")), "build info has stream_connected");
    expect(!text.contains(QStringLiteral("os_version=windows\n")), "build info os_version is not the windows stub");
    expect(nax5OsBuild().contains(QLatin1Char('.')), "os_build is dotted version");
    expect(nax5LocaleName() == QLocale::system().name(), "nax5LocaleName matches system locale");
}

static void test_build_info_snapshot_stream_stats()
{
    Nax5BuildInfoSnapshot snapshot;
    snapshot.decoder = QStringLiteral("vulkan");
    snapshot.resolution = QStringLiteral("1280x720");
    snapshot.bitrate_kbps = QStringLiteral("15000");
    snapshot.fps = QStringLiteral("60");
    snapshot.show_stream_stats = true;
    snapshot.log_verbose = true;
    snapshot.log_sanitize = false;
    snapshot.has_stream_stats = true;
    snapshot.avg_packet_loss = 0.003;
    snapshot.max_packet_loss = QStringLiteral("0.112000");
    snapshot.dropped_frames = 42;
    snapshot.frames_lost = 7;
    snapshot.measured_bitrate_kbps = 12345;
    snapshot.session_duration_sec = 519;
    snapshot.stream_connected = true;
    const QString text = nax5BuildInfoText(snapshot);
    expect(text.contains(QStringLiteral("decoder=vulkan\n")), "snapshot decoder");
    expect(text.contains(QStringLiteral("resolution=1280x720\n")), "snapshot resolution");
    expect(text.contains(QStringLiteral("bitrate=15000\n")), "snapshot bitrate");
    expect(text.contains(QStringLiteral("fps=60\n")), "snapshot fps");
    expect(text.contains(QStringLiteral("show_stream_stats=true\n")), "snapshot show_stream_stats");
    expect(text.contains(QStringLiteral("log_verbose=true\n")), "snapshot log_verbose");
    expect(text.contains(QStringLiteral("log_sanitize=false\n")), "snapshot log_sanitize");
    expect(text.contains(QStringLiteral("avg_packet_loss=0.003000\n")), "snapshot avg_packet_loss");
    expect(text.contains(QStringLiteral("dropped_frames=42\n")), "snapshot dropped_frames");
    expect(text.contains(QStringLiteral("frames_lost=7\n")), "snapshot frames_lost");
    expect(text.contains(QStringLiteral("measured_bitrate=12345\n")), "snapshot measured_bitrate");
    expect(text.contains(QStringLiteral("session_duration_sec=519\n")), "snapshot session_duration_sec");
    expect(text.contains(QStringLiteral("stream_connected=true\n")), "snapshot stream_connected");
}

static void test_report_zip_is_archive()
{
    Nax5BuildInfoSnapshot snapshot;
    snapshot.decoder = QStringLiteral("d3d11va");
    snapshot.has_stream_stats = true;
    snapshot.avg_packet_loss = 0.01;
    snapshot.dropped_frames = 3;
    const QByteArray zip = nax5BuildClientReportZipBytes(Nax5ClientReportKindQuit, snapshot);
    expect(zip.startsWith("PK\x03\x04"), "zip archive magic");
    expect(zip.contains("BUILD-INFO"), "zip contains BUILD-INFO");
    expect(zip.contains("client_version=testver"), "zip build info has product version");
    expect(zip.contains("decoder=d3d11va"), "zip build info has decoder");
    expect(zip.contains("avg_packet_loss=0.010000"), "zip build info has packet loss");
    expect(zip.contains("dropped_frames=3"), "zip build info has dropped frames");
    expect(zip.contains("network_adapter_type="), "zip build info has network hint");
}

static void test_report_archive_fits_under_cap()
{
    expect(nax5ClientReportUsesArchive(Nax5ClientReportKindQuit), "quit uses archive");
    expect(nax5ClientReportUsesArchive(Nax5ClientReportKindError), "error uses archive");
    expect(!nax5ClientReportUsesArchive(Nax5ClientReportKindReserveFail), "reserve-fail uses json");
    expect(!nax5ClientReportUsesArchive(Nax5ClientReportKindLogout), "logout uses json");
    expect(nax5TailBytes(QByteArray("ab\ncd\nef"), 5).endsWith("ef"), "tail keeps suffix");

    QList<QPair<QString, QByteArray>> files;
    files.append(qMakePair(QStringLiteral("BUILD-INFO"), QByteArray("client_version=testver\n")));
    files.append(qMakePair(QStringLiteral("chiaki_session_now.log"), QByteArray(3 * 1024 * 1024, 'x')));
    files.append(qMakePair(QStringLiteral("nax5_now.log"), QByteArray(700 * 1024, 'y')));
    const QList<QPair<QString, QByteArray>> fitted = nax5FitReportFiles(files, nax5ClientReportMaxArchiveBytes());
    const QByteArray zip = nax5ZipBytes(fitted);
    expect(!zip.isEmpty(), "fitted zip is not empty");
    expect(zip.size() <= nax5ClientReportMaxArchiveBytes(), "fitted zip stays under archive cap");
    expect(zip.contains("BUILD-INFO"), "fitted zip keeps BUILD-INFO");
    expect(zip.contains("client_version=testver"), "fitted zip keeps product version");
    const QByteArray json = nax5BuildClientReportJson(Nax5ClientReportKindQuit, QStringLiteral("sid"));
    expect(json.startsWith('{') && json.endsWith('}'), "json fallback is a complete object");
}

static void test_payload_no_secrets()
{
    const QByteArray body = nax5BuildClientEventBatch(QStringLiteral("PLAY_REQUESTED"));
    expect(!nax5ClientEventPayloadContainsSecrets(body), "payload has no secret patterns");
    expect(!body.contains("regist_key"), "no regist_key");
    expect(!body.contains("X-Session-Token"), "no session token");
}

static void test_event_batching()
{
    nax5QueueClientEvent(QStringLiteral("PLAY_REQUESTED"));
    nax5QueueClientEvent(QStringLiteral("RESERVE_SUCCEEDED"), QStringLiteral("11111111-1111-4111-8111-111111111111"));
    expect(nax5QueuedClientEventCount() == 2, "queued two client events");
    const QByteArray body = nax5TakeQueuedClientEventBatch();
    expect(nax5QueuedClientEventCount() == 0, "queue drained after take");
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonArray events = root.value(QStringLiteral("events")).toArray();
    expect(events.size() == 2, "batch contains two events");
    expect(events.at(0).toObject().value(QStringLiteral("event_type")).toString() == QStringLiteral("PLAY_REQUESTED"), "first queued event type");
    expect(events.at(1).toObject().value(QStringLiteral("event_type")).toString() == QStringLiteral("RESERVE_SUCCEEDED"), "second queued event type");
    expect(events.at(1).toObject().value(QStringLiteral("session_public_id")).toString() == QStringLiteral("11111111-1111-4111-8111-111111111111"), "queued session id");
    expect(!nax5ClientEventPayloadContainsSecrets(body), "batched payload has no secrets");
}

static void test_full_session_is_not_silently_truncated()
{
    QTemporaryDir dir;
    test_log_dir = dir.path();
    QFile log(dir.filePath(QStringLiteral("chiaki_session_test.log")));
    expect(log.open(QIODevice::WriteOnly), "open synthetic session log");
    QByteArray original("SESSION-BEGIN-MARKER\n");
    for (int i = 0; i < 65536; ++i) original += QByteArray(64, 'x') + '\n';
    original += "SESSION-END-MARKER\n";
    log.write(original);
    log.close();
    const QString root = dir.filePath(QStringLiteral("queue"));
    const QString id = nax5BeginReport(root, 123, QStringLiteral("session-A"), QStringLiteral("client_version=testver\n"), {{log.fileName(), 0}});
    expect(!id.isEmpty(), "durable journal created");
    expect(nax5FinishReport(root, id, QStringLiteral("quit"), QStringLiteral("client_version=testver\n")), "freeze final report before upload");
    expect(log.open(QIODevice::Append), "append after finalization fixture");
    log.write("NEXT-SESSION-MUST-NOT-ENTER-PREVIOUS-REPORT\n");
    log.close();
    for (int i = 0; i < 40; ++i) expect(nax5CaptureReports(root, 123, 2), "bounded capture succeeds");
    expect(nax5NextReportPart(root, 456).path.isEmpty(), "another account cannot upload queued report");
    QByteArray restored;
    int count = 0;
    bool saw_final = false;
    while (true)
    {
        const auto part = nax5NextReportPart(root, 123);
        if (part.path.isEmpty()) break;
        const QString fixture_dir = QString::fromLocal8Bit(qgetenv("NAX5_DIAGNOSTIC_TEST_EXPORT_DIR"));
        if (!fixture_dir.isEmpty())
        {
            QDir().mkpath(fixture_dir);
            QFile fixture(QDir(fixture_dir).filePath(QStringLiteral("part-%1.zip").arg(count, 4, 10, QLatin1Char('0'))));
            expect(fixture.open(QIODevice::WriteOnly) && fixture.write(part.archive) == part.archive.size(), "export synthetic contract fixture");
        }
        expect(part.session_id == QStringLiteral("session-A"), "queue keeps original session after reconnect");
        expect(part.client_version == QStringLiteral("testver") && part.client_sha == QStringLiteral("testsha"), "queued report keeps original build provenance after upgrade");
        expect(part.archive.size() <= nax5ClientReportMaxArchiveBytes(), "each full-log part below server limit");
        expect(nax5NextReportPart(root, 123).path == part.path, "no acknowledgement preserves exact retry part");
        // Read stored ZIP local entries; test byte-for-byte reconstruction, not just markers.
        int pos = 0;
        auto u16 = [&](int p) { return quint16(quint8(part.archive[p])) | (quint16(quint8(part.archive[p + 1])) << 8); };
        auto u32 = [&](int p) { return quint32(u16(p)) | (quint32(u16(p + 2)) << 16); };
        while (part.archive.mid(pos, 4) == QByteArray("PK\x03\x04", 4))
        {
            const int size = int(u32(pos + 18));
            const int name_size = u16(pos + 26);
            const int extra = u16(pos + 28);
            const QByteArray name = part.archive.mid(pos + 30, name_size);
            const QByteArray payload = part.archive.mid(pos + 30 + name_size + extra, size);
            if (name == "log-part.txt") restored += payload;
            if (name == "MANIFEST.json" && QJsonDocument::fromJson(payload).object().value("final").toBool()) saw_final = true;
            pos += 30 + name_size + extra + size;
        }
        expect(nax5AcknowledgeReportPart(part), "ack removes only uploaded part");
        ++count;
    }
    expect(restored.contains("SESSION-BEGIN-MARKER"), "full session preserves beginning over archive limit");
    expect(restored.contains("SESSION-END-MARKER"), "full session preserves ending");
    expect(restored == original, "full multi-megabyte session reconstructed byte for byte");
    expect(!restored.contains("NEXT-SESSION"), "finalized source boundary excludes later session data");
    expect(count > 2 && saw_final, "multipart report includes explicit completion manifest");
    test_log_dir.clear();
}

static void test_queue_recovery_and_sanitization()
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("queue"));
    QFile log(dir.filePath(QStringLiteral("session.log")));
    expect(log.open(QIODevice::WriteOnly), "open recovery fixture");
    log.write("password=DO-NOT-UPLOAD\nAuthorization: Bearer SECRET-TOKEN\nnormal-stream-line\n");
    log.close();
    const QString a = nax5BeginReport(root, 1, QStringLiteral("session-A"), QStringLiteral("version=test\n"), {{log.fileName(), 0}});
    const QString b = nax5BeginReport(root, 2, QStringLiteral("session-B"), QStringLiteral("version=test\n"), {{log.fileName(), 0}});
    nax5RecoverReports(root, 1);
    expect(nax5CaptureReports(root, 1, 4), "crash journal recovers on next login");
    const auto part = nax5NextReportPart(root, 1);
    expect(!part.archive.contains("DO-NOT-UPLOAD") && !part.archive.contains("SECRET-TOKEN"), "archive sanitizes credentials before durable storage");
    expect(part.archive.contains("normal-stream-line"), "sanitization preserves diagnostic content");
    expect(part.kind == QStringLiteral("crash"), "recovered report marked unclean exit");
    expect(QFile::exists(QDir(root).filePath(b + QStringLiteral(".journal"))), "recovery does not consume another account's journal");
    expect(nax5NextReportPart(root, 2).path.isEmpty(), "account B still has independent capture");
    expect(nax5AcknowledgeReportPart(part), "first account ack works");
    expect(!nax5NextReportPart(root, 1).path.isEmpty(), "one ack does not erase remaining parts");
}

static void write_old_file(const QString &path, const QByteArray &data)
{
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly), "open crash fixture");
    file.write(data);
    file.close();
    // Crash files younger than 5 s are treated as still being written.
    expect(file.open(QIODevice::ReadWrite)
        && file.setFileTime(QDateTime::currentDateTime().addSecs(-60), QFileDevice::FileModificationTime), "age crash fixture");
    file.close();
}

static void test_crash_dumps_are_queued_as_crash_parts()
{
    QTemporaryDir dir;
    const QString root = dir.filePath(QStringLiteral("queue"));
    const QString dumps = dir.filePath(QStringLiteral("crash-dumps"));
    expect(QDir().mkpath(dumps), "crash dump dir");
    const QString play = nax5BeginReport(root, 7, QStringLiteral("session-crashed"), QStringLiteral("version=test\n"), {});
    expect(!play.isEmpty(), "play journal before crash");
    {
        // The play started two minutes before the crash fixtures below.
        QFile journal(QDir(root).filePath(play + QStringLiteral(".journal")));
        expect(journal.open(QIODevice::ReadOnly), "read play journal");
        auto object = QJsonDocument::fromJson(journal.readAll()).object();
        journal.close();
        object.insert("created_utc", QDateTime::currentDateTimeUtc().addSecs(-120).toString(Qt::ISODateWithMs));
        expect(journal.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && journal.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0, "backdate play journal");
        journal.close();
    }
    write_old_file(QDir(dumps).filePath(QStringLiteral("NAX5-20260926-190321-4242.dmp")), QByteArray("MDMP") + QByteArray(4096, '\x01'));
    write_old_file(QDir(dumps).filePath(QStringLiteral("NAX5-20260926-190321-4242.txt")),
        "reason=unhandled_exception\nexception_code=0xC0000005\nfault_module=C:\\NAX5\\chiaki.exe\nfault_offset=0x1234\n");
    write_old_file(QDir(dumps).filePath(QStringLiteral("NAX5-20260926-190500-4243.dmp")), QByteArray(3 * 1024 * 1024, '\x02'));
    write_old_file(QDir(dumps).filePath(QStringLiteral("NAX5-20260926-190500-4243.txt")), "reason=abort\n");
    // WER LocalDumps copy of the crash already captured in-app (pid 4242) and a WER-only crash.
    write_old_file(QDir(dumps).filePath(QStringLiteral("chiaki.exe.4242.dmp")), QByteArray("MDMP-wer-duplicate"));
    write_old_file(QDir(dumps).filePath(QStringLiteral("chiaki.exe.999.dmp")), QByteArray("MDMP-wer-only"));

    expect(nax5QueueCrashDumps(root, 7, dumps, QStringLiteral("client_version=testver\n")) == 3,
        "in-app crashes and WER-only crash queued, WER duplicate dropped");
    expect(QDir(dumps).entryList(QDir::Files).isEmpty(), "queued crash files removed from disk");

    int with_dump = 0, summary_only = 0, wer_only = 0;
    for (int i = 0; i < 3; ++i)
    {
        const auto part = nax5NextReportPart(root, 7);
        expect(!part.path.isEmpty(), "crash part available for upload");
        expect(part.kind == QStringLiteral("crash"), "crash part uses crash kind");
        expect(part.session_id == QStringLiteral("session-crashed"), "crash attributed to the play that was running");
        expect(part.archive.size() <= nax5ClientReportMaxArchiveBytes(), "crash part fits upload cap");
        expect(!part.archive.contains("MDMP-wer-duplicate"), "WER duplicate never uploaded");
        if (part.archive.contains("chiaki.exe.999.dmp"))
            ++wer_only;
        else if (part.archive.contains("NAX5-20260926-190321-4242.dmp"))
        {
            expect(part.archive.contains("fault_module=") && part.archive.contains("0xC0000005"), "summary travels with dump");
            ++with_dump;
        }
        else
        {
            expect(part.archive.contains("too_large") && part.archive.contains("reason=abort"), "oversized dump keeps summary");
            ++summary_only;
        }
        expect(nax5AcknowledgeReportPart(part), "ack crash part");
    }
    expect(with_dump == 1 && summary_only == 1 && wer_only == 1, "one dump shipped, one reduced to summary, one WER dump shipped");
    expect(nax5QueueCrashDumps(root, 7, dumps, QString()) == 0, "nothing queued twice");
}

static void test_live_report_queue_batches_small_appends()
{
    QTemporaryDir dir;
    expect(dir.isValid(), "live queue temp dir");
    const QString root = dir.filePath(QStringLiteral("queue"));
    const QString process_path = dir.filePath(QStringLiteral("nax5.log"));
    const QString stream_path = dir.filePath(QStringLiteral("chiaki.log"));
    QFile process(process_path);
    QFile stream(stream_path);
    expect(process.open(QIODevice::WriteOnly), "open live process log");
    expect(stream.open(QIODevice::WriteOnly), "open live stream log");
    const QString id = nax5BeginReport(root, 321, QStringLiteral("session-live"),
        QStringLiteral("client_version=testver\n"), {{process_path, 0}, {stream_path, 0}});
    expect(!id.isEmpty(), "begin live report");

    for (int i = 0; i < 100; ++i)
    {
        process.write(QByteArray("process-") + QByteArray::number(i) + QByteArray("\n"));
        stream.write(QByteArray("stream-") + QByteArray::number(i) + QByteArray("\n"));
        process.flush();
        stream.flush();
        expect(nax5CaptureReports(root, 321, 4), "capture small live append");
    }
    expect(QDir(root).entryList({QStringLiteral("*.part")}, QDir::Files).isEmpty(),
        "small live appends wait for batching threshold");

    process.close();
    stream.close();
    expect(nax5FinishReport(root, id, QStringLiteral("quit"), QStringLiteral("client_version=testver\n")),
        "finish batched live report");
    for (int i = 0; i < 10; ++i)
        expect(nax5CaptureReports(root, 321, 4), "flush batched live report");
    expect(QDir(root).entryList({QStringLiteral("*.part")}, QDir::Files).size() <= 3,
        "short live session creates bounded multipart count");
}

static bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

static void test_path_summary()
{
    const Nax5PathWindow w = nax5SummarizePath({10, 12, 11, 50}, 5, 1);
    expect(w.sent == 5 && w.received == 4 && w.lost == 1, "path summary counts");
    expect(near(w.rtt_p50_ms, 11) && near(w.rtt_p95_ms, 50) && near(w.rtt_max_ms, 50), "path summary percentiles");
    expect(near(w.jitter_ms, 14), "path summary jitter is mean consecutive delta");
    const Nax5PathWindow empty = nax5SummarizePath({}, 3, 3);
    expect(empty.rtt_p50_ms < 0 && empty.jitter_ms < 0 && empty.lost == 3, "path summary with no replies");
}

static void test_path_fields()
{
    // Build 12 stream_sample keys must stay; build 14 adds p95/jitter and the home_* set.
    QJsonObject metric;
    nax5InsertPathFields(metric, QStringLiteral("vps"), nax5SummarizePath({10, 12, 11, 50}, 10, 1));
    nax5InsertPathFields(metric, QStringLiteral("home"), nax5SummarizePath({30}, 10, 9));
    const QStringList expected = {"home_lost", "home_rtt_max_ms", "home_rtt_p50_ms", "home_rtt_p95_ms", "home_sent",
        "vps_jitter_ms", "vps_lost", "vps_rtt_max_ms", "vps_rtt_p50_ms", "vps_rtt_p95_ms", "vps_sent"};
    expect(metric.keys() == expected, "path fields: vps_* and home_* keys");
    expect(metric.value("vps_sent").toInt() == 10 && metric.value("vps_lost").toInt() == 1, "path fields: counts");
    expect(near(metric.value("vps_rtt_p95_ms").toDouble(), 50) && near(metric.value("vps_jitter_ms").toDouble(), 14),
        "path fields: p95 and jitter");
    expect(!metric.contains("home_jitter_ms"), "path fields: no jitter from a single reply");
    QJsonObject silent;
    nax5InsertPathFields(silent, QStringLiteral("home"), nax5SummarizePath({}, 10, 10));
    expect(silent.keys() == QStringList({"home_lost", "home_sent"}), "path fields: no RTT keys without replies");
}

// Mirror of ALLOWED_METADATA_KEYS additions in nax5-backend client_telemetry/schemas.py.
static const QSet<QString> kBackendHealthKeys = {
    "window_s", "loss_max_pct", "frames_lost", "bitrate_min_kbps", "bitrate_p50_kbps", "render_dropped_max",
    "queue_max", "vps_sent", "vps_lost", "vps_rtt_p50_ms", "vps_rtt_p95_ms", "vps_rtt_max_ms", "vps_jitter_ms"};

static void test_session_totals_cover_whole_session()
{
    Nax5SessionTotals totals;
    expect(totals.averageBitrateKbps() == -1 && totals.averagePacketLoss() == 0, "empty totals");
    // A lossy middle and a quiet final second, as in the 26.09 Maikop session.
    const QList<Nax5StreamSecond> samples = {
        {0.00, 0, 5000, 0, 1}, {0.10, 2, 3000, 1200, 3}, {0.02, 4, 4000, 300, 2}, {0.00, 4, 0, 0, 1}};
    for (const auto &s : samples) totals.add(s);
    expect(totals.seconds() == 4, "totals count seconds");
    expect(totals.renderDroppedTotal() == 1500, "dropped frames summed over the session, not the last second");
    expect(totals.averageBitrateKbps() == 4000, "bitrate averages only measured seconds");
    expect(qAbs(totals.averagePacketLoss() - 0.03) < 1e-9, "packet loss is the session mean");
    expect(qAbs(totals.maxPacketLoss() - 0.10) < 1e-9, "max packet loss over the session");
    totals.reset();
    expect(totals.seconds() == 0 && totals.renderDroppedTotal() == 0, "reset clears totals");
}

static void test_stream_health_window()
{
    Nax5StreamHealth health;
    bool ready = false;
    for (int i = 0; i < Nax5StreamHealth::kWindowSeconds; ++i)
    {
        Nax5StreamSecond s;
        s.packet_loss = i == 7 ? 0.25 : 0.0;
        s.frames_lost_total = 3 + (i >= 10 ? 2 : 0);
        s.bitrate_kbps = 1000 * (i + 1);
        s.render_dropped = i == 3 ? 4 : 0;
        s.queue_depth = 1.5;
        ready = health.add(s);
        if (i < Nax5StreamHealth::kWindowSeconds - 1)
            expect(!ready, "health window not ready early");
    }
    expect(ready, "health window ready after 20 seconds");
    Nax5PathWindow path = nax5SummarizePath({5, 6, 40}, 200, 2);
    const QJsonObject m = health.take(path);
    expect(m.value("window_s").toString() == "20", "health window size");
    expect(m.value("loss_max_pct").toString() == "25.00", "health max loss");
    expect(m.value("frames_lost").toString() == "2", "health frames lost is a delta, not the total");
    expect(m.value("bitrate_min_kbps").toString() == "1000", "health bitrate min");
    expect(m.value("render_dropped_max").toString() == "4", "health render drops");
    expect(m.value("vps_sent").toString() == "200" && m.value("vps_lost").toString() == "2", "health vps counts");
    bool keys_ok = true;
    for (const QString &key : m.keys())
        keys_ok = keys_ok && kBackendHealthKeys.contains(key);
    expect(keys_ok, "health keys are all on the backend allowlist");
    expect(QJsonDocument(m).toJson(QJsonDocument::Compact).size() <= 512, "health metadata fits backend 512-byte limit");
    expect(health.size() == 0, "take clears the window");

    Nax5StreamSecond reset_stream;
    reset_stream.frames_lost_total = 0; // new stream: counter restarted below the previous base
    health.add(reset_stream);
    expect(health.take(Nax5PathWindow()).value("frames_lost").toString() == "0", "frames lost never negative");
}

static void test_telemetry_metadata_batch()
{
    while (nax5QueuedClientEventCount() > 0)
        nax5TakeQueuedClientEventBatch();
    QJsonObject metadata;
    metadata.insert("vps_lost", "3");
    nax5QueueClientEvent(QStringLiteral("STREAM_HEALTH"), QString(), metadata);
    const QByteArray body = nax5TakeQueuedClientEventBatch();
    const QJsonObject event = QJsonDocument::fromJson(body).object().value("events").toArray().at(0).toObject();
    expect(event.value("event_type").toString() == "STREAM_HEALTH", "health event type");
    expect(event.value("metadata").toObject().value("vps_lost").toString() == "3", "health metadata in batch");
    expect(!nax5ClientEventPayloadContainsSecrets(body), "health batch has no secrets");
}

#ifdef Q_OS_WIN
#include <windows.h>
static qint64 processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](FILETIME f) { return qint64((quint64(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10000); };
    return ms(k) + ms(u);
}
#else
#include <ctime>
static qint64 processCpuMs() { return qint64(std::clock() * 1000 / CLOCKS_PER_SEC); }
#endif

static void run_loop_ms(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static void test_path_probe_local_echo()
{
    QUdpSocket echo;
    expect(echo.bind(QHostAddress::LocalHost, 0), "local echo bound");
    QObject::connect(&echo, &QUdpSocket::readyRead, [&echo]() {
        while (echo.hasPendingDatagrams())
        {
            QByteArray data(int(echo.pendingDatagramSize()), '\0');
            QHostAddress from;
            quint16 port = 0;
            echo.readDatagram(data.data(), data.size(), &from, &port);
            echo.writeDatagram(data, from, port);
        }
    });
    Nax5PathProbe probe;
    probe.start(QStringLiteral("127.0.0.1"), echo.localPort());
    run_loop_ms(3300);
    const Nax5PathWindow w = probe.window(3);
    QElapsedTimer stop_clock;
    stop_clock.start();
    probe.stop();
    std::printf("  local echo: seconds=%d sent=%d received=%d lost=%d p50=%.2f\n", w.seconds, w.sent, w.received, w.lost, w.rtt_p50_ms);
    expect(w.seconds == 3, "probe reports completed seconds");
    expect(w.sent >= 27 && w.sent <= 33, "probe sends every 100 ms");
    expect(w.lost == 0 && w.received >= 27, "probe gets local echoes");
    expect(w.rtt_p50_ms >= 0 && w.rtt_p50_ms < 50, "probe measures RTT");
    expect(stop_clock.elapsed() < 500, "probe stops promptly");
    expect(!probe.running(), "probe not running after stop");

    QUdpSocket closed;
    closed.bind(QHostAddress::LocalHost, 0);
    const quint16 dead_port = closed.localPort();
    closed.close();
    const qint64 cpu_before = processCpuMs();
    probe.start(QStringLiteral("127.0.0.1"), dead_port);
    run_loop_ms(2300);
    const qint64 cpu_used = processCpuMs() - cpu_before;
    std::printf("  dead port CPU: %lld ms over 2300 ms\n", (long long)cpu_used);
    expect(cpu_used < 500, "probe does not spin when the port is closed");
    const Nax5PathWindow dead = probe.window(2);
    probe.stop();
    std::printf("  dead port: sent=%d received=%d lost=%d\n", dead.sent, dead.received, dead.lost);
    expect(dead.received == 0 && dead.lost > 0, "probe counts loss when nothing answers");
}

// Optional live check against the deployed VPS echo: NAX5_TEST_VPS_ECHO=host
static void test_path_probe_live_vps()
{
    const QString host = qEnvironmentVariable("NAX5_TEST_VPS_ECHO");
    if (host.isEmpty())
        return;
    Nax5PathProbe probe;
    probe.start(host, Nax5PathProbe::kDefaultPort);
    run_loop_ms(5300);
    const Nax5PathWindow w = probe.window(5);
    probe.stop();
    std::printf("  live VPS %s: sent=%d received=%d lost=%d p50=%.2f p95=%.2f jitter=%.2f\n", qPrintable(host),
        w.sent, w.received, w.lost, w.rtt_p50_ms, w.rtt_p95_ms, w.jitter_ms);
    expect(w.sent >= 45 && w.received >= w.sent - 2, "live VPS echo answers the client probe");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settings_dir;
    if (!settings_dir.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
    test_payload_has_version_and_sha();
    test_installation_os_and_locale_are_real();
    test_report_has_product_version();
    test_build_info_has_diagnostics_fields();
    test_build_info_snapshot_stream_stats();
    test_report_zip_is_archive();
    test_report_archive_fits_under_cap();
    test_payload_no_secrets();
    test_event_batching();
    test_full_session_is_not_silently_truncated();
    test_live_report_queue_batches_small_appends();
    test_queue_recovery_and_sanitization();
    test_crash_dumps_are_queued_as_crash_parts();
    test_path_summary();
    test_path_fields();
    test_session_totals_cover_whole_session();
    test_stream_health_window();
    test_telemetry_metadata_batch();
    test_path_probe_local_echo();
    test_path_probe_live_vps();
    return g_failed == 0 ? 0 : 1;
}
