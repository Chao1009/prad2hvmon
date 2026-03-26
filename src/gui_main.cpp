// ─────────────────────────────────────────────────────────────────────────────
// prad2hvmon – PRad-II HV Monitor (Qt GUI + CLI read/write via daemon)
//
// All modes connect to the prad2hvd daemon via WebSocket — no direct
// hardware access.  The daemon must be running for all modes.
//
// Modes:
//   gui (default)              – launch Qt WebEngine dashboard
//   read  [-s file.json]       – save all writable params to JSON
//   write -f file.json [-P pw] – restore writable params from JSON
//
// Usage:
//   prad2hvmon                              # GUI mode (default)
//   prad2hvmon -H clonpc19                 # GUI: daemon on clonpc19
//   prad2hvmon read -s snapshot.json        # save settings via daemon
//   prad2hvmon write -f snapshot.json       # restore settings via daemon
//   prad2hvmon read -H clonpc19 -s out.json # read from remote daemon
//
// Author: Chao Peng — Argonne National Laboratory
// ─────────────────────────────────────────────────────────────────────────────

#include <QApplication>
#include <QCoreApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QShortcut>
#include <QKeySequence>
#include <QPixmap>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <unistd.h>


// ─────────────────────────────────────────────────────────────────────────────
//  Usage
// ─────────────────────────────────────────────────────────────────────────────
static void printUsage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << "                              # GUI mode (default)\n"
        << "  " << prog << " [gui] [-H host] [-p port]    # GUI with daemon address\n"
        << "  " << prog << " read  [-s file.json]          # save settings via daemon\n"
        << "  " << prog << " write -f file.json -P pw      # restore settings via daemon\n"
        << "\nOptions:\n"
        << "  -H <host>   Daemon hostname (default: localhost)\n"
        << "  -p <port>   Daemon WebSocket port (default: 8765)\n"
        << "  -r <dir>    Resources directory (GUI mode)\n"
        << "  -s <file>   Save output path (read mode)\n"
        << "  -f <file>   Settings file to load (write mode)\n"
        << "  -P <pass>   Expert password (write mode — required if daemon has auth)\n"
        << "  -t <sec>    Timeout in seconds (CLI modes, default: 10)\n"
        << "  -h          Show this help\n"
        << "\nThe daemon (prad2hvd) must be running. CLI read/write connects\n"
        << "to the daemon via WebSocket — same path as the GUI dashboard.\n";
}


// ─────────────────────────────────────────────────────────────────────────────
//  Pretty-print settings as a text table
// ─────────────────────────────────────────────────────────────────────────────
static void printSettingsTable(const QJsonObject &root)
{
    QJsonArray channels = root["channels"].toArray();
    int nch = channels.size();
    if (nch == 0) { std::cout << "0 channels\n"; return; }

    std::string ts = root["timestamp"].toString("?").toStdString();
    std::cout << "Settings: " << ts << " (" << nch << " channels)\n\n";

    // ── Pass 1: discover param columns, check if each is all-integer ──
    struct Col {
        std::string name;
        int  width;
        bool allInt;   // true if every value in this column is integer-valued
    };
    std::vector<Col> cols;
    std::map<std::string, size_t> colMap;
    int wCrate = 5, wName = 4;

    for (int i = 0; i < nch; i++) {
        QJsonObject ch = channels[i].toObject();
        int cl = static_cast<int>(ch["crate"].toString().size());
        int nl = static_cast<int>(ch["name"].toString().size());
        if (cl > wCrate) wCrate = cl;
        if (nl > wName)  wName  = nl;

        QJsonObject params = ch["params"].toObject();
        for (auto it = params.begin(); it != params.end(); ++it) {
            std::string pn = it.key().toStdString();
            double v = it.value().toDouble();
            bool isInt = (v == std::floor(v));

            auto [mit, inserted] = colMap.try_emplace(pn, cols.size());
            if (inserted)
                cols.push_back({pn, static_cast<int>(pn.size()), true});
            if (!isInt)
                cols[mit->second].allInt = false;
        }
    }

    // ── Pass 2: compute column widths using the determined format ──────
    for (int i = 0; i < nch; i++) {
        QJsonObject params = channels[i].toObject()["params"].toObject();
        for (auto it = params.begin(); it != params.end(); ++it) {
            Col &c = cols[colMap[it.key().toStdString()]];
            double v = it.value().toDouble();
            char buf[32];
            int w;
            if (c.allInt)
                w = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(v));
            else
                w = std::snprintf(buf, sizeof(buf), "%.2f", v);
            if (w > c.width) c.width = w;
        }
    }

    // ── Header ────────────────────────────────────────────────────────
    std::cout << std::left  << std::setw(wCrate) << "Crate"
              << "  " << std::right << std::setw(4) << "Slot"
              << "  " << std::setw(3) << "Ch"
              << "  " << std::left  << std::setw(wName) << "Name";
    for (const auto &c : cols)
        std::cout << "  " << std::right << std::setw(c.width) << c.name;
    std::cout << "\n";

    // ── Separator ─────────────────────────────────────────────────────
    int total = wCrate + 2 + 4 + 2 + 3 + 2 + wName;
    for (const auto &c : cols) total += 2 + c.width;
    std::cout << std::string(total, '-') << "\n";

    // ── Rows ──────────────────────────────────────────────────────────
    for (int i = 0; i < nch; i++) {
        QJsonObject ch = channels[i].toObject();
        std::cout << std::left  << std::setw(wCrate) << ch["crate"].toString().toStdString()
                  << "  " << std::right << std::setw(4) << ch["slot"].toInt()
                  << "  " << std::setw(3) << ch["channel"].toInt()
                  << "  " << std::left  << std::setw(wName) << ch["name"].toString().toStdString();

        QJsonObject params = ch["params"].toObject();
        for (const auto &c : cols) {
            QString key = QString::fromStdString(c.name);
            if (params.contains(key)) {
                double v = params[key].toDouble();
                char buf[32];
                if (c.allInt)
                    std::snprintf(buf, sizeof(buf), "%*d", c.width, static_cast<int>(v));
                else
                    std::snprintf(buf, sizeof(buf), "%*.2f", c.width, v);
                std::cout << "  " << buf;
            } else {
                std::cout << "  " << std::right << std::setw(c.width) << "-";
            }
        }
        std::cout << "\n";
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  CLI read: connect to daemon, send save_settings, receive response, write file
// ─────────────────────────────────────────────────────────────────────────────
static int doRead(const std::string &host, const std::string &port,
                  const std::string &savePath, int timeoutSec)
{
    QCoreApplication *app = QCoreApplication::instance();
    QWebSocket ws;
    bool done = false;
    int  exitCode = 0;

    QObject::connect(&ws, &QWebSocket::connected, [&]() {
        std::cout << "Connected to daemon at " << host << ":" << port << "\n";
        // Send save_settings command
        ws.sendTextMessage(R"({"type":"save_settings"})");
        std::cout << "Requesting settings snapshot…\n";
    });

    QObject::connect(&ws, &QWebSocket::textMessageReceived, [&](const QString &msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (doc.isNull()) return;
        QJsonObject obj = doc.object();
        QString type = obj["type"].toString();

        if (type == "error") {
            std::cerr << "ERROR: " << obj["message"].toString().toStdString() << "\n";
            exitCode = 1;
            done = true;
            ws.close();
            app->quit();
            return;
        }

        if (type == "settings_snapshot") {
            // Extract the settings data
            QJsonValue data = obj["data"];
            QJsonDocument outDoc;
            if (data.isObject())
                outDoc = QJsonDocument(data.toObject());
            else if (data.isString())
                outDoc = QJsonDocument::fromJson(data.toString().toUtf8());

            QJsonObject root = outDoc.object();

            // Save JSON to file if -s specified
            if (!savePath.empty()) {
                QByteArray jsonBytes = outDoc.toJson(QJsonDocument::Indented);
                std::ofstream f(savePath);
                f.write(jsonBytes.constData(), jsonBytes.size());
                std::cout << "Saved to " << savePath << "\n\n";
            }

            // Print human-readable table to stdout
            printSettingsTable(root);

            done = true;
            exitCode = 0;
            ws.close();
            app->quit();
        }
        // Ignore other message types (init, hv_snapshot, etc.)
    });

    QObject::connect(&ws, &QWebSocket::disconnected, [&]() {
        if (!done) {
            std::cerr << "Disconnected from daemon before receiving settings\n";
            exitCode = 1;
            app->quit();
        }
    });

    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                     [&](QAbstractSocket::SocketError err) {
        std::cerr << "WebSocket error: " << ws.errorString().toStdString() << "\n";
        exitCode = 1;
        app->quit();
    });

    // Timeout
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        if (!done) {
            std::cerr << "Timeout waiting for daemon response (" << timeoutSec << "s)\n";
            exitCode = 1;
            ws.close();
            app->quit();
        }
    });
    timeout.start(timeoutSec * 1000);

    // Connect
    QString url = QString("ws://%1:%2").arg(
        QString::fromStdString(host), QString::fromStdString(port));
    ws.open(QUrl(url));

    app->exec();
    return exitCode;
}


// ─────────────────────────────────────────────────────────────────────────────
//  CLI write: connect to daemon, send load_settings with file contents
// ─────────────────────────────────────────────────────────────────────────────
static int doWrite(const std::string &host, const std::string &port,
                   const std::string &settingsPath, int timeoutSec,
                   const std::string &password)
{
    // Read the settings file
    std::ifstream f(settingsPath);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot open settings file: " << settingsPath << "\n";
        return 1;
    }
    std::string fileContent((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    // Validate JSON
    QJsonParseError parseErr;
    QJsonDocument settingsDoc = QJsonDocument::fromJson(
        QByteArray::fromStdString(fileContent), &parseErr);
    if (settingsDoc.isNull()) {
        std::cerr << "ERROR: invalid JSON: " << parseErr.errorString().toStdString() << "\n";
        return 1;
    }

    QJsonObject root = settingsDoc.object();
    if (!root.contains("channels") || !root["channels"].isArray()) {
        std::cerr << "ERROR: missing 'channels' array in settings file\n";
        return 1;
    }

    int nch = root["channels"].toArray().size();
    std::cout << "Settings file: " << root["timestamp"].toString("?").toStdString()
              << " (" << nch << " channels)\n";

    QCoreApplication *app = QCoreApplication::instance();
    QWebSocket ws;
    bool done = false;
    int  exitCode = 0;

    // Helper: send the actual load_settings command
    auto sendLoadCmd = [&]() {
        QJsonObject cmd;
        cmd["type"] = "load_settings";
        cmd["settings"] = settingsDoc.object();

        QByteArray payload = QJsonDocument(cmd).toJson(QJsonDocument::Compact);
        ws.sendTextMessage(QString::fromUtf8(payload));

        std::cout << "Sent load_settings (" << nch << " channels), waiting for daemon…\n";
    };

    QObject::connect(&ws, &QWebSocket::connected, [&]() {
        std::cout << "Connected to daemon at " << host << ":" << port << "\n";

        if (!password.empty()) {
            // Authenticate as Expert before sending the command
            QJsonObject auth;
            auth["type"]     = "auth";
            auth["level"]    = 2;
            auth["password"] = QString::fromStdString(password);
            ws.sendTextMessage(QJsonDocument(auth).toJson(QJsonDocument::Compact));
        } else {
            // No password — send command directly (works when auth is disabled)
            sendLoadCmd();
        }
    });

    QObject::connect(&ws, &QWebSocket::textMessageReceived, [&](const QString &msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (doc.isNull()) return;
        QJsonObject obj = doc.object();
        QString type = obj["type"].toString();

        // ── Authentication result ────────────────────────────────────
        if (type == "auth_result") {
            int granted = obj["granted"].toInt();
            if (granted >= 2) {
                std::cout << "Authenticated as Expert\n";
                sendLoadCmd();
            } else {
                std::string reason = obj["reason"].toString("authentication failed").toStdString();
                std::cerr << "ERROR: " << reason << "\n";
                exitCode = 1;
                done = true;
                ws.close();
                app->quit();
            }
            return;
        }

        // ── Daemon-side error (e.g. access_denied) ──────────────────
        if (type == "error") {
            std::string code    = obj["code"].toString().toStdString();
            std::string message = obj["message"].toString().toStdString();
            std::cerr << "ERROR";
            if (!code.empty()) std::cerr << " [" << code << "]";
            std::cerr << ": " << message << "\n";
            if (code == "access_denied")
                std::cerr << "Hint: use -P <password> to authenticate as Expert\n";
            exitCode = 1;
            done = true;
            ws.close();
            app->quit();
            return;
        }

        // ── Load settings result ─────────────────────────────────────
        if (type != "load_settings_done") return;

        QJsonObject data = obj["data"].toObject();
        if (data.contains("error")) {
            std::cerr << "ERROR: " << data["error"].toString().toStdString() << "\n";
            exitCode = 1;
        } else {
            int restored  = data["restored"].toInt();
            int unch      = data["unchanged"].toInt();
            int skip      = data["skipped"].toInt();
            int errs      = data["errors"].toInt();
            std::cout << "Done — " << restored << " restored, "
                      << unch << " unchanged, "
                      << skip << " skipped, " << errs << " errors\n";
            exitCode = (errs > 0) ? 1 : 0;
        }
        done = true;
        ws.close();
        app->quit();
    });

    QObject::connect(&ws, &QWebSocket::disconnected, [&]() {
        if (!done) {
            std::cerr << "Disconnected from daemon unexpectedly\n";
            exitCode = 1;
            app->quit();
        }
    });

    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                     [&](QAbstractSocket::SocketError) {
        std::cerr << "WebSocket error: " << ws.errorString().toStdString() << "\n";
        exitCode = 1;
        app->quit();
    });

    // Timeout
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        if (!done) {
            std::cerr << "Timeout waiting for daemon response (" << timeoutSec << "s)\n";
            exitCode = 1;
            ws.close();
            app->quit();
        }
    });
    timeout.start(timeoutSec * 1000);

    QString url = QString("ws://%1:%2").arg(
        QString::fromStdString(host), QString::fromStdString(port));
    ws.open(QUrl(url));

    app->exec();
    return exitCode;
}


// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    // Determine mode from first non-flag argument
    std::string mode = "gui";
    int mode_arg_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            mode = argv[i];
            mode_arg_idx = i;
            break;
        }
    }

    // Remove mode argument from argv so getopt doesn't choke
    int eff_argc = argc;
    if (mode_arg_idx > 0) {
        for (int i = mode_arg_idx; i < argc - 1; i++)
            argv[i] = argv[i + 1];
        eff_argc = argc - 1;
    }

    // Parse options
    std::string host = "localhost", port_str = "8765", resourceDir;
    std::string saveFile, settingsFile, cliPassword;
    int timeoutSec = 10;

    optind = 1;
    int opt;
    while ((opt = getopt(eff_argc, argv, "H:p:r:s:f:t:P:h")) != -1) {
        switch (opt) {
        case 'H': host         = optarg; break;
        case 'p': port_str     = optarg; break;
        case 'r': resourceDir  = optarg; break;
        case 's': saveFile     = optarg; break;
        case 'f': settingsFile = optarg; break;
        case 't': timeoutSec   = std::atoi(optarg); break;
        case 'P': cliPassword  = optarg; break;
        case 'h': printUsage(argv[0]); return 0;
        default:  printUsage(argv[0]); return 1;
        }
    }

    // ══════════════════════════════════════════════════════════════════════
    //  GUI mode
    // ══════════════════════════════════════════════════════════════════════
    if (mode == "gui") {
        QApplication app(argc, argv);
        app.setApplicationName("PRad-II HV Monitor");

        if (resourceDir.empty()) {
            QStringList candidates = {
                QCoreApplication::applicationDirPath() + "/../resources",
                QCoreApplication::applicationDirPath() + "/../../resources",
#ifdef RESOURCE_DIR
                QString::fromStdString(RESOURCE_DIR),
#endif
            };
            for (const auto &p : candidates) {
                if (QFile::exists(p + "/monitor.html")) {
                    resourceDir = QDir(p).absolutePath().toStdString();
                    break;
                }
            }
        }

        std::cout << "Daemon: " << host << ":" << port_str << "\n";
        if (!resourceDir.empty())
            std::cout << "Resources: " << resourceDir << "\n";

        QWebEngineView view;
        view.setWindowTitle("PRad-II HV Monitor");
        view.resize(1400, 900);

        QUrl url;
        if (!resourceDir.empty()) {
            url = QUrl::fromLocalFile(QString::fromStdString(resourceDir + "/monitor.html"));
            QUrlQuery query;
            query.addQueryItem("host", QString::fromStdString(host));
            query.addQueryItem("port", QString::fromStdString(port_str));
            url.setQuery(query);
        } else {
            url = QUrl(QString("http://%1:%2/monitor.html")
                .arg(QString::fromStdString(host), QString::fromStdString(port_str)));
        }
        std::cout << "Loading: " << url.toString().toStdString() << "\n";
        view.setUrl(url);

        // Screenshot shortcut (Ctrl+S)
        std::string dbDir = DATABASE_DIR;
        auto *sc = new QShortcut(QKeySequence("Ctrl+S"), &view);
        QObject::connect(sc, &QShortcut::activated, [&view, &dbDir]() {
            const QString dir = QString::fromStdString(dbDir) + "/screenshots";
            QDir().mkpath(dir);
            const QString ts   = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            const QString path = dir + "/prad2hvmon_" + ts + ".png";
            if (view.grab().save(path))
                std::cout << "Screenshot saved: " << path.toStdString() << "\n";
            else
                std::cerr << "Screenshot failed: " << path.toStdString() << "\n";
        });

        view.show();
        return app.exec();
    }

    // ══════════════════════════════════════════════════════════════════════
    //  CLI modes (read / write) — via daemon WebSocket
    // ══════════════════════════════════════════════════════════════════════
    if (mode != "read" && mode != "write") {
        std::cerr << "Unknown mode: " << mode << "\n";
        printUsage(argv[0]);
        return 1;
    }

    if (mode == "write" && settingsFile.empty()) {
        std::cerr << "ERROR: write mode requires -f <settings.json>\n";
        return 1;
    }

    // CLI modes need a QCoreApplication for the event loop (QWebSocket)
    QCoreApplication app(argc, argv);

    if (mode == "read")
        return doRead(host, port_str, saveFile, timeoutSec);
    else
        return doWrite(host, port_str, settingsFile, timeoutSec, cliPassword);
}
