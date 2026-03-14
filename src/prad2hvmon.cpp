// ─────────────────────────────────────────────────────────────────────────────
// prad2hvmon – PRad-II High-Voltage Monitor
//
// Modes:
//   gui                    – launch the Qt WebEngine dashboard  (default)
//   read  [-s file.json]   – read all writable params, save to JSON
//   write -f file.json     – restore writable params from JSON
//   convert -f old.txt -s new.json  – convert old text format to JSON
//   hv_params              – print discovered board/channel param info
//
// Author: Chao Peng (Argonne National Laboratory)
// Date:   03/09/2026
// ─────────────────────────────────────────────────────────────────────────────

#include <QApplication>
#include <QEvent>
#include <QThread>
#include <QWebEngineView>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QUrl>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QShortcut>
#include <QKeySequence>
#include <QPixmap>
#include <QDateTime>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QPointer>
#include <memory>

#include <ConfigParser.h>
#include <ConfigOption.h>
#include <caen_channel.h>
#include <fmt/format.h>

#include "hv_monitor.h"
#include "booster_monitor.h"
#include "file_fault_logger.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>


// ── Load crate list from JSON ────────────────────────────────────────────────
static std::vector<std::pair<std::string, std::string>>
load_crate_list(const QString &path)
{
    std::vector<std::pair<std::string, std::string>> list;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "ERROR: cannot open crate config: "
                  << path.toStdString() << "\n";
        return list;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        std::cerr << "ERROR: invalid JSON in crate config: "
                  << err.errorString().toStdString() << "\n";
        return list;
    }
    for (const auto &val : doc.array()) {
        QJsonObject obj = val.toObject();
        list.emplace_back(obj["name"].toString().toStdString(),
                          obj["ip"].toString().toStdString());
    }
    std::cout << fmt::format("Loaded {} crate(s) from {}\n",
                             list.size(), path.toStdString());
    return list;
}

// ── Load error-ignore list from JSON ────────────────────────────────────────
static std::vector<CAEN_Channel::ErrorIgnoreRule>
load_error_ignore_rules(const QString &path)
{
    std::vector<CAEN_Channel::ErrorIgnoreRule> rules;
    if (path.isEmpty()) return rules;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "WARNING: cannot open error-ignore file: "
                  << path.toStdString() << "\n";
        return rules;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        std::cerr << "WARNING: invalid JSON in error-ignore file: "
                  << err.errorString().toStdString() << "\n";
        return rules;
    }
    for (const auto &val : doc.object()["ignore"].toArray()) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        CAEN_Channel::ErrorIgnoreRule rule;
        rule.pattern = obj["name"].toString().toStdString();
        for (const auto &e : obj["errors"].toArray())
            rule.errors.push_back(e.toString().toStdString());
        if (!rule.pattern.empty() && !rule.errors.empty())
            rules.push_back(rule);
    }

    std::cout << fmt::format("Loaded {} error-ignore rule(s) from {}\n",
                             rules.size(), path.toStdString());
    return rules;
}

// ── Load voltage limits from JSON ───────────────────────────────────────────
// File format:
//   {
//     "limits": [
//       { "pattern": "G*",    "voltage": 1950 },
//       { "pattern": "W*",    "voltage": 1450 },
//       { "pattern": "G235",  "voltage": 1800 },
//       { "pattern": "*",     "voltage": 1500 }
//     ]
//   }
//
// Rules are evaluated in order — first matching pattern wins.
// Patterns: "G*" matches any name starting with "G"; "G235" is exact;
// "*" matches everything (use as a catch-all default at the end).
static int load_voltage_limits(const QString &path, bool user_specified = false)
{
    if (path.isEmpty()) return 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Only warn if the user explicitly specified the file via -l
        if (user_specified) {
            std::cerr << "WARNING: cannot open voltage-limits file: "
                      << path.toStdString() << "\n";
        }
        return 0;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        std::cerr << "WARNING: invalid JSON in voltage-limits file: "
                  << err.errorString().toStdString() << "\n";
        return 0;
    }

    CAEN_Channel::ClearVoltageLimits();

    int count = 0;
    for (const auto &val : doc.object()["limits"].toArray()) {
        QJsonObject obj = val.toObject();
        std::string pattern = obj["pattern"].toString().toStdString();
        double voltage = obj["voltage"].toDouble(0);
        if (pattern.empty() || voltage <= 0) {
            std::cerr << "WARNING: skipping invalid voltage-limit rule in "
                      << path.toStdString() << "\n";
            continue;
        }
        CAEN_Channel::SetVoltageLimit(pattern, static_cast<float>(voltage));
        ++count;
    }

    std::cout << fmt::format("Loaded {} voltage-limit rule(s) from {}\n",
                             count, path.toStdString());
    return count;
}


static bool init_crates_console(const std::vector<std::pair<std::string, std::string>> &crate_list,
                                std::vector<CAEN_Crate*> &crates,
                                std::map<std::string, CAEN_Crate*> &crate_map);
static void print_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::string &save_path);
static void write_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::map<std::string, CAEN_Crate*> &crate_map,
                           const std::string &setting_path);
static void convert_old_to_json(const std::string &old_path,
                                const std::string &json_path);
static void dump_board_params(const std::vector<CAEN_Crate*> &crates);

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    // ── Parse command-line ───────────────────────────────────────────────
    ConfigOption co;
    co.AddOpts(ConfigOption::arg_require, 'c', "crates");
    co.AddOpts(ConfigOption::arg_require, 'f', "file");
    co.AddOpts(ConfigOption::arg_require, 'i', "ignore");
    co.AddOpts(ConfigOption::arg_require, 'l', "limits");
    co.AddOpts(ConfigOption::arg_require, 's', "save");
    co.AddOpts(ConfigOption::arg_require, 'm', "module-geo");
    co.AddOpts(ConfigOption::help_message, 'h', "help");

    // help messages
    co.SetDesc("usage: %0 <mode> [gui, read, write, convert, hv_params]");
    co.SetDesc('c', "path to crates JSON file (default: auto-discover).");
    co.SetDesc('f', "path to settings file (write: JSON to restore; convert: old text input).");
    co.SetDesc('i', "path to error-ignore JSON file (default: auto-discover).");
    co.SetDesc('l', "path to voltage-limits JSON file (default: auto-discover).");
    co.SetDesc('s', "path to save output (read: JSON snapshot; convert: JSON output).");
    co.SetDesc('m', "path to module geometry JSON file (GUI mode).");
    co.SetDesc('h', "show help messages.");

    if (!co.ParseArgs(argc, argv)) {
        std::cout << co.GetInstruction() << std::endl;
        return -1;
    }

    std::string setting_file, save_file, module_geo_file, crate_config_file, ignore_file, limits_file;

    for (auto &opt : co.GetOptions()) {
        switch (opt.mark) {
        case 'c': crate_config_file = opt.var.String(); break;
        case 'f': setting_file      = opt.var.String(); break;
        case 'i': ignore_file       = opt.var.String(); break;
        case 'l': limits_file       = opt.var.String(); break;
        case 's': save_file         = opt.var.String(); break;
        case 'm': module_geo_file   = opt.var.String(); break;
        }
    }

    // Default mode is "gui" when no positional argument is given
    std::string mode = (co.NbofArgs() >= 1) ? co.GetArgument(0).String()
                                            : "gui";

    // ── Locate and load crate config ────────────────────────────────────
    QString crateConfigPath;
    if (!crate_config_file.empty()) {
        crateConfigPath = QString::fromStdString(crate_config_file);
    } else {
        crateConfigPath = QString::fromStdString(
            std::string(DATABASE_DIR) + "/crates.json");
    }
    if (!QFile::exists(crateConfigPath)) {
        std::cerr << "ERROR: cannot find crates.json at "
                  << crateConfigPath.toStdString() << "\n"
                  << "Use -c <path> to specify the crate config file.\n";
        return -1;
    }
    auto crate_list = load_crate_list(crateConfigPath);
    if (crate_list.empty()) {
        std::cerr << "ERROR: no crates defined in "
                  << crateConfigPath.toStdString() << "\n";
        return -1;
    }

    // ── Load and apply error-ignore list ────────────────────────────────
    {
        QString ignoreConfigPath;
        if (!ignore_file.empty()) {
            ignoreConfigPath = QString::fromStdString(ignore_file);
        } else {
            ignoreConfigPath = QString::fromStdString(
                std::string(DATABASE_DIR) + "/error_ignore.json");
        }
        CAEN_Channel::SetErrorIgnoreRules(load_error_ignore_rules(ignoreConfigPath));
    }

    // ── Load and apply voltage limits ───────────────────────────────────
    {
        QString limitsConfigPath;
        bool limitsUserSpecified = false;
        if (!limits_file.empty()) {
            limitsConfigPath = QString::fromStdString(limits_file);
            limitsUserSpecified = true;
        } else {
            limitsConfigPath = QString::fromStdString(
                std::string(DATABASE_DIR) + "/voltage_limits.json");
        }
        load_voltage_limits(limitsConfigPath, limitsUserSpecified);
    }

    // ── GUI mode ────────────────────────────────────────────────────────
    if (mode == "gui") {
        QApplication app(argc, argv);
        app.setApplicationName("PRad-II HV Monitor");

        // Back-end object
        // Locate module geometry JSON
        QString moduleGeoPath;
        if (!module_geo_file.empty()) {
            moduleGeoPath = QString::fromStdString(module_geo_file);
        } else {
            // Auto-discover next to monitor.html
            QStringList geoCandidates = {
                QCoreApplication::applicationDirPath() + "/../database/hycal_modules.json",
                QCoreApplication::applicationDirPath() + "/../../database/hycal_modules.json",
                QString::fromStdString(std::string(DATABASE_DIR) + "/hycal_modules.json"),
            };
            for (const auto &p : geoCandidates) {
                if (QFile::exists(p)) { moduleGeoPath = QDir(p).absolutePath(); break; }
            }
        }
        if (!moduleGeoPath.isEmpty())
            std::cout << "Module geometry: " << moduleGeoPath.toStdString() << "\n";

        // Locate GUI config JSON
        QString guiConfigPath;
        {
            QStringList candidates = {
                QCoreApplication::applicationDirPath() + "/../database/gui_config.json",
                QCoreApplication::applicationDirPath() + "/../../database/gui_config.json",
                QString::fromStdString(std::string(DATABASE_DIR) + "/gui_config.json"),
            };
            for (const auto &p : candidates) {
                if (QFile::exists(p)) { guiConfigPath = QDir(p).absolutePath(); break; }
            }
        }
        if (!guiConfigPath.isEmpty())
            std::cout << "GUI config: " << guiConfigPath.toStdString() << "\n";

        // Locate DAQ connection map JSON
        QString daqMapPath;
        {
            QStringList candidates = {
                QCoreApplication::applicationDirPath() + "/../database/daq_map.json",
                QCoreApplication::applicationDirPath() + "/../../database/daq_map.json",
                QString::fromStdString(std::string(DATABASE_DIR) + "/daq_map.json"),
            };
            for (const auto &p : candidates) {
                if (QFile::exists(p)) { daqMapPath = QDir(p).absolutePath(); break; }
            }
        }
        if (!daqMapPath.isEmpty())
            std::cout << "DAQ map: " << daqMapPath.toStdString() << "\n";

        // ── Hardware poller (heap-allocated, will live on worker thread) ─────
        // Heap allocation is required: poller must be destroyed on the worker
        // thread (via deleteLater), not on the main thread stack.  A stack
        // variable would have its destructor run on the main thread after
        // wait(), but its thread affinity is the worker – undefined behaviour.
        //
        // initCrates() runs here before moveToThread – safe because the
        // worker thread has not started yet, so there is no race.
        HVPoller *poller = new HVPoller(crate_list);
        if (!poller->initCrates()) {
            std::cerr << "WARNING: not all crates connected – "
                         "dashboard will show partial data.\n";
        }

        // ── GUI-thread bridge (registered with QWebChannel) ─────────────
        HVMonitor monitor(poller, moduleGeoPath, guiConfigPath, daqMapPath);

        // ── Booster HV supplies (TDK-Lambda GEN via SCPI/TCP) ────────────
        // No hardcoded addresses. Priority order:
        //   1. hycal_modules.json — entries with "t":"booster" carry "ip"
        //      (and optionally "port") alongside their geometry fields.
        //      This is the canonical source: add/change a booster by editing
        //      the same file that defines its geometry block.
        //   2. gui_config.json "booster" array — legacy/override path,
        //      used only when the modules file has no booster entries.
        //
        // Names MUST match the "n" field in hycal_modules.json so that
        // boosterByName[mod.n] lookups in the frontend resolve correctly.
        std::vector<std::tuple<QString,QString,quint16>> boosterDefs;

        // Source 1: hycal_modules.json booster entries
        if (!moduleGeoPath.isEmpty()) {
            QFile mf(moduleGeoPath);
            if (mf.open(QIODevice::ReadOnly)) {
                QJsonArray mods = QJsonDocument::fromJson(mf.readAll()).array();
                for (const auto &v : mods) {
                    QJsonObject m = v.toObject();
                    if (m["t"].toString().compare("booster", Qt::CaseInsensitive) != 0)
                        continue;
                    QString name = m["n"].toString();
                    QString ip   = m["ip"].toString();
                    quint16 port = static_cast<quint16>(m["port"].toInt(8003));
                    if (name.isEmpty() || ip.isEmpty()) {
                        std::cerr << "WARNING: booster entry in modules JSON missing "
                                     "'n' or 'ip' field — skipped\n";
                        continue;
                    }
                    boosterDefs.emplace_back(name, ip, port);
                    std::cout << fmt::format("Booster {:s} @ {:s}:{:d} (from modules JSON)\n",
                                             name.toStdString(), ip.toStdString(), port);
                }
            }
        }

        // Source 2: gui_config.json "booster" array (fallback)
        if (boosterDefs.empty() && !guiConfigPath.isEmpty()) {
            QFile bcf(guiConfigPath);
            if (bcf.open(QIODevice::ReadOnly)) {
                QJsonObject bcfg = QJsonDocument::fromJson(bcf.readAll()).object();
                QJsonArray bsupplies = bcfg["booster"].toArray();
                for (const auto &v : bsupplies) {
                    QJsonObject bs = v.toObject();
                    QString name = bs["name"].toString();
                    QString ip   = bs["ip"].toString();
                    quint16 port = static_cast<quint16>(bs["port"].toInt(8003));
                    if (name.isEmpty() || ip.isEmpty()) continue;
                    boosterDefs.emplace_back(name, ip, port);
                    std::cout << fmt::format("Booster {:s} @ {:s}:{:d} (from gui_config)\n",
                                             name.toStdString(), ip.toStdString(), port);
                }
            }
        }

        if (boosterDefs.empty())
            std::cerr << "WARNING: no booster supplies defined — "
                         "add entries with \"t\":\"booster\" and \"ip\" to hycal_modules.json\n";
        BoosterPoller  *bPoller  = new BoosterPoller(boosterDefs);
        BoosterMonitor  bMonitor(bPoller);

        // ── Fault logger (shared by both pollers, thread-safe) ───────────
        FileFaultLogger faultLogger(
            std::string(DATABASE_DIR) + "/fault_log");
        poller->setFaultLogger(&faultLogger);
        bPoller->setFaultLogger(&faultLogger);
        std::cout << "Fault logger: " << std::string(DATABASE_DIR)
                  << "/fault_log/\n";

        // ── Worker thread ────────────────────────────────────
        QThread workerThread;
        poller->moveToThread(&workerThread);
        // When the worker event loop exits, Qt destroys poller via deleteLater
        // on the worker thread itself – the correct thread for ~HVPoller().
        QObject::connect(&workerThread, &QThread::finished,
                         poller, &QObject::deleteLater);

        // Booster poller also runs on the same worker thread
        QThread boosterThread;
        bPoller->moveToThread(&boosterThread);
        QObject::connect(&boosterThread, &QThread::finished,
                         bPoller, &QObject::deleteLater);

        // Web channel (C++ <-> JS bridge)
        QWebChannel channel;
        channel.registerObject(QStringLiteral("hvMonitor"),    &monitor);
        channel.registerObject(QStringLiteral("boosterMonitor"), &bMonitor);

        // Read window size from gui_config.json (default 1400x900)
        int winW = 1400, winH = 900;
        if (!guiConfigPath.isEmpty()) {
            QFile cf(guiConfigPath);
            if (cf.open(QIODevice::ReadOnly)) {
                QJsonObject cfg = QJsonDocument::fromJson(cf.readAll()).object();
                QJsonObject win = cfg["window"].toObject();
                if (win.contains("width"))  winW = win["width"].toInt(1400);
                if (win.contains("height")) winH = win["height"].toInt(900);
            }
        }

        // Web view
        QWebEngineView view;
        view.setWindowTitle("PRad-II HV Monitor");
        view.resize(winW, winH);
        view.page()->setWebChannel(&channel);

        // Load the dashboard HTML from the resources directory next to the
        // binary, or fall back to the source tree location.
        QString htmlPath;
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + "/../resources/monitor.html",
            QCoreApplication::applicationDirPath() + "/../../resources/monitor.html",
            QString::fromStdString(std::string(RESOURCE_DIR) + "/monitor.html"),
        };
        for (const auto &p : candidates) {
            if (QFile::exists(p)) { htmlPath = p; break; }
        }
        if (htmlPath.isEmpty()) {
            std::cerr << "ERROR: cannot find monitor.html\n";
            delete poller;   // workerThread never started, deleteLater won't fire
            delete bPoller;  // same for booster thread
            return -1;
        }
        view.setUrl(QUrl::fromLocalFile(QDir(htmlPath).absolutePath()));
        // Don't let detached windows keep the app alive after the main window closes.
        // We quit explicitly when the main view receives its close event.
        app.setQuitOnLastWindowClosed(false);
        view.show();

        // Quit app when main window is closed (detached windows must not keep it alive)
        struct CloseWatcher : QObject {
            QApplication *app_;
            explicit CloseWatcher(QApplication *a, QObject *parent)
                : QObject(parent), app_(a) {}
            bool eventFilter(QObject *, QEvent *e) override {
                if (e->type() == QEvent::Close) { app_->quit(); }
                return false;   // don't consume the event
            }
        };
        view.installEventFilter(new CloseWatcher(&app, &view));

        // Detached windows — one per detachable tab.
        // QPointers let us detect if a window is still open and raise it
        // instead of creating a duplicate.
        struct DetachedViews {
            QPointer<QWebEngineView> board;
            QPointer<QWebEngineView> geo;
            QPointer<QWebEngineView> booster;
        };
        auto detached = std::make_shared<DetachedViews>();

        QObject::connect(&monitor, &HVMonitor::detachRequested,
                         &view,
                         [&monitor, &bMonitor, htmlPath, detached](const QString &tabId)
        {
            QPointer<QWebEngineView> &slot =
                (tabId == QLatin1String("board-tab"))   ? detached->board
                : (tabId == QLatin1String("geo-tab"))   ? detached->geo
                                                        : detached->booster;
            if (slot && slot->isVisible()) {
                slot->raise();
                slot->activateWindow();
                return;
            }
            auto *dv = new QWebEngineView();
            dv->setAttribute(Qt::WA_DeleteOnClose);
            dv->setWindowTitle(
                tabId == QLatin1String("board-tab")
                    ? QStringLiteral("Board Status — PRad-II HV Monitor")
                : tabId == QLatin1String("geo-tab")
                    ? QStringLiteral("HyCal Geometry — PRad-II HV Monitor")
                    : QStringLiteral("Booster HV — PRad-II HV Monitor"));
            dv->resize(1100, 800);
            // Each detached view gets its own QWebChannel so signal indices
            // are initialised fresh for that client — sharing one QWebChannel
            // across multiple QWebEnginePages causes "unhandled signal" errors.
            auto *ch = new QWebChannel(dv);
            ch->registerObject(QStringLiteral("hvMonitor"),      &monitor);
            ch->registerObject(QStringLiteral("boosterMonitor"), &bMonitor);
            dv->page()->setWebChannel(ch);
            QUrl url = QUrl::fromLocalFile(QDir(htmlPath).absolutePath());
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("tab"), tabId);
            url.setQuery(q);
            dv->setUrl(url);
            dv->show();
            slot = dv;
        });

        // Close detached windows when main window closes (app quits)
        QObject::connect(&app, &QApplication::aboutToQuit, [detached]() {
            if (detached->board)   detached->board->close();
            if (detached->geo)     detached->geo->close();
            if (detached->booster) detached->booster->close();
        });

        // Screenshot: Ctrl+S saves a timestamped PNG to the user's Pictures
        // folder (falls back to the current directory if unavailable).
        auto *screenshotShortcut = new QShortcut(QKeySequence("Ctrl+S"), &view);
        QObject::connect(screenshotShortcut, &QShortcut::activated, [&view]() {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
            const QString ts  = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            const QString path = (dir.isEmpty() ? QString(".") : dir)
                                 + "/prad2hvmon_" + ts + ".png";
            const QPixmap px = view.grab();
            if (px.save(path))
                qInfo("Screenshot saved: %s", qPrintable(path));
            else
                qWarning("Screenshot failed: %s", qPrintable(path));
        });

        // Start worker threads, then kick off polling via queued calls
        workerThread.start();
        QMetaObject::invokeMethod(poller, "startPolling", Qt::QueuedConnection);
        boosterThread.start();
        // NOTE: booster polling is NOT started automatically.
        // The TDK-Lambda GEN supplies allow only one TCP connection at a time,
        // so we let the user manually connect from the Booster HV tab to avoid
        // locking out other monitor instances.
        // QMetaObject::invokeMethod(bPoller, "startPolling", Qt::QueuedConnection);

        const int ret = app.exec();

        workerThread.quit();
        workerThread.wait();

        // Shut down the booster worker thread.  The difficulty: doPoll() uses
        // synchronous TCP with 2–3 s timeouts per supply.  If a poll is in
        // flight when we quit, the thread won't stop until those timeouts
        // expire.  We quit the event loop first (so no further timer events
        // fire), then wait with a generous but bounded timeout.
        boosterThread.quit();
        if (!boosterThread.wait(8000)) {
            // If an in-flight doPoll is still blocking on TCP timeouts,
            // force-terminate so the process exits promptly.
            std::cerr << "WARNING: booster thread did not stop in time — terminating\n";
            boosterThread.terminate();
            boosterThread.wait();
        }
        return ret;
    }

    // ── Convert mode (no hardware needed) ──────────────────────────────
    if (mode == "convert") {
        if (setting_file.empty()) {
            std::cerr << "ERROR: convert mode requires -f <old_settings.txt>\n";
            return -1;
        }
        if (save_file.empty()) {
            std::cerr << "ERROR: convert mode requires -s <output.json>\n";
            return -1;
        }
        convert_old_to_json(setting_file, save_file);
        return 0;
    }

    // ── Console modes (read / write / hv_params) — need hardware ────────
    std::vector<CAEN_Crate*> crates;
    std::map<std::string, CAEN_Crate*> crate_map;

    if (!init_crates_console(crate_list, crates, crate_map)) {
        std::cerr << "Aborted! Crates initialisation failed!\n";
        return -1;
    }

    if (mode == "read") {
        print_channels(crates, save_file);
    } else if (mode == "write") {
        if (setting_file.empty()) {
            std::cerr << "ERROR: write mode requires -f <settings.json>\n";
            for (auto *c : crates) delete c;
            return -1;
        }
        write_channels(crates, crate_map, setting_file);
    } else if (mode == "hv_params") {
        dump_board_params(crates);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n";
        for (auto *c : crates) delete c;
        return -1;
    }

    for (auto *c : crates) delete c;
    return 0;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Console-mode helpers (kept from the original programme)
// ─────────────────────────────────────────────────────────────────────────────

static bool init_crates_console(const std::vector<std::pair<std::string, std::string>> &crate_list,
                                std::vector<CAEN_Crate*> &crates,
                                std::map<std::string, CAEN_Crate*> &crate_map)
{
    int crid = 0;
    for (const auto &[name, ip] : crate_list) {
        auto *cr = new CAEN_Crate(crid++, name, ip,
                                  CAENHV::SY1527, LINKTYPE_TCPIP,
                                  "admin", "admin");
        crates.push_back(cr);
        crate_map[name] = cr;
    }

    int ok = 0;
    for (auto *cr : crates) {
        if (cr->Initialize()) {
            std::cout << fmt::format("Connected to {:s} @ {:s}\n",
                                     cr->GetName(), cr->GetIP());
            cr->PrintCrateMap();
            ++ok;
        } else {
            std::cerr << fmt::format("Cannot connect to {:s} @ {:s}\n",
                                     cr->GetName(), cr->GetIP());
        }
    }
    std::cout << fmt::format("Init DONE – {}/{} crates OK\n",
                             ok, crates.size());
    return (ok == static_cast<int>(crates.size()));
}

// ── Read mode: save all writable params to JSON ─────────────────────────────

static void print_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::string &save_path)
{
    // Read current values from hardware
    for (auto *cr : crates)
        cr->ReadAllParams();

    QJsonObject root;
    root["format"]    = "prad2hvmon_settings_v1";
    root["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    QJsonArray channels;
    int total = 0;

    for (auto *cr : crates) {
        for (auto *bd : cr->GetBoardList()) {
            const auto &paramInfo = bd->GetChParamInfo();
            for (auto *ch : bd->GetChannelList()) {
                QJsonObject entry;
                entry["crate"]   = QString::fromStdString(cr->GetName());
                entry["slot"]    = bd->GetSlot();
                entry["channel"] = ch->GetChannel();
                entry["name"]    = QString::fromStdString(ch->GetName());

                QJsonObject params;
                for (const auto &pi : paramInfo) {
                    if (!pi.isWritable()) continue;

                    if (pi.isFloat()) {
                        float v = ch->GetFloat(pi.name);
                        if (!std::isnan(v))
                            params[QString::fromStdString(pi.name)] = v;
                    } else if (pi.isUInt()) {
                        if (ch->HasParam(pi.name))
                            params[QString::fromStdString(pi.name)] = static_cast<int>(ch->GetUInt(pi.name));
                    }
                }
                entry["params"] = params;
                channels.append(entry);
                ++total;
            }
        }
    }
    root["channels"] = channels;

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);

    // Always print summary to stdout
    std::cout << fmt::format("Read {} channels from {} crate(s)\n", total, crates.size());

    if (!save_path.empty()) {
        std::ofstream f(save_path);
        f << json.constData();
        std::cout << "Saved to " << save_path << "\n";
    } else {
        // No file specified — print to stdout
        std::cout << json.constData() << std::endl;
    }
}

// ── Write mode: restore writable params from JSON ───────────────────────────

static void write_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::map<std::string, CAEN_Crate*> &crate_map,
                           const std::string &setting_path)
{
    QFile f(QString::fromStdString(setting_path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "ERROR: cannot open settings file: " << setting_path << "\n";
        return;
    }
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (doc.isNull()) {
        std::cerr << "ERROR: invalid JSON: " << parseErr.errorString().toStdString() << "\n";
        return;
    }

    QJsonObject root = doc.object();
    QString format = root["format"].toString();
    if (format != "prad2hvmon_settings_v1") {
        std::cerr << "WARNING: unknown format '" << format.toStdString()
                  << "', proceeding anyway\n";
    }
    std::cout << "Settings file timestamp: "
              << root["timestamp"].toString().toStdString() << "\n";

    QJsonArray channels = root["channels"].toArray();
    int restored = 0, skipped = 0, errors = 0;

    // Build a lookup: crate→board param info for writable-param validation
    for (const auto &val : channels) {
        QJsonObject entry = val.toObject();
        std::string crate_name = entry["crate"].toString().toStdString();
        int slot    = entry["slot"].toInt();
        int channel = entry["channel"].toInt();
        std::string ch_name = entry["name"].toString().toStdString();

        auto cit = crate_map.find(crate_name);
        if (cit == crate_map.end()) {
            std::cerr << fmt::format("  SKIP {}/s{}/ch{} — crate not found\n",
                                     crate_name, slot, channel);
            ++skipped;
            continue;
        }
        auto *board = cit->second->GetBoard(slot);
        if (!board) {
            std::cerr << fmt::format("  SKIP {}/s{}/ch{} — board not found\n",
                                     crate_name, slot, channel);
            ++skipped;
            continue;
        }
        auto *ch = board->GetChannel(channel);
        if (!ch) {
            std::cerr << fmt::format("  SKIP {}/s{}/ch{} — channel not found\n",
                                     crate_name, slot, channel);
            ++skipped;
            continue;
        }

        // Restore channel name if different
        if (ch->GetName() != ch_name) {
            ch->SetName(ch_name);
            std::cout << fmt::format("  {}/s{}/ch{} name → {}\n",
                                     crate_name, slot, channel, ch_name);
        }

        // Restore each writable param
        QJsonObject params = entry["params"].toObject();
        const auto &paramInfo = board->GetChParamInfo();

        for (auto it = params.begin(); it != params.end(); ++it) {
            std::string pname = it.key().toStdString();

            // Find the param info to determine type
            const ParamInfo *pi = nullptr;
            for (const auto &info : paramInfo) {
                if (info.name == pname && info.isWritable()) { pi = &info; break; }
            }
            if (!pi) {
                std::cerr << fmt::format("  SKIP {}/s{}/ch{} param {} — not writable or not found\n",
                                         crate_name, slot, channel, pname);
                continue;
            }

            bool ok = false;
            if (pi->isFloat()) {
                float v = static_cast<float>(it.value().toDouble());
                ok = ch->SetFloat(pname, v);
                if (ok)
                    std::cout << fmt::format("  {}/s{}/ch{} {} → {:.2f}\n",
                                             crate_name, slot, channel, pname, v);
            } else if (pi->isUInt()) {
                unsigned int v = static_cast<unsigned int>(it.value().toInt());
                ok = ch->SetUInt(pname, v);
                if (ok)
                    std::cout << fmt::format("  {}/s{}/ch{} {} → {}\n",
                                             crate_name, slot, channel, pname, v);
            }

            if (ok) ++restored;
            else    ++errors;
        }
    }

    std::cout << fmt::format("\nDone — {} params restored, {} skipped, {} errors\n",
                             restored, skipped, errors);
}

// ── Convert old text format to new JSON ─────────────────────────────────────
// Old format:
//   #      crate    slot channel            name      VMon      VSet
//       PRadHV_1       0       0      PRIMARY1_0    1490.8      1500
//
// Produces JSON with V0Set only (the old format only had VSet).

static void convert_old_to_json(const std::string &old_path,
                                const std::string &json_path)
{
    ConfigParser parser;
    parser.ReadFile(old_path);

    QJsonObject root;
    root["format"]    = "prad2hvmon_settings_v1";
    root["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    root["converted_from"] = QString::fromStdString(old_path);

    QJsonArray channels;
    int count = 0;

    while (parser.ParseLine()) {
        std::string crate_name, ch_name;
        int slot;
        unsigned short channel;
        float VMon, VSet;

        if (parser.NbofElements() == 5)
            parser >> crate_name >> slot >> channel >> ch_name >> VSet;
        else if (parser.NbofElements() == 6)
            parser >> crate_name >> slot >> channel >> ch_name >> VMon >> VSet;
        else
            continue;

        QJsonObject entry;
        entry["crate"]   = QString::fromStdString(crate_name);
        entry["slot"]    = slot;
        entry["channel"] = channel;
        entry["name"]    = QString::fromStdString(ch_name);

        QJsonObject params;
        params["V0Set"] = VSet;
        entry["params"] = params;

        channels.append(entry);
        ++count;
    }

    root["channels"] = channels;

    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    std::ofstream f(json_path);
    f << json.constData();

    std::cout << fmt::format("Converted {} channels from '{}' → '{}'\n",
                             count, old_path, json_path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  hv_params mode: print discovered param info from the board objects
// ─────────────────────────────────────────────────────────────────────────────

static const char *paramTypeName(unsigned t) {
    switch (t) {
    case PARAM_TYPE_NUMERIC:  return "NUMERIC";
    case PARAM_TYPE_ONOFF:    return "ONOFF";
    case PARAM_TYPE_CHSTATUS: return "CHSTATUS";
    case PARAM_TYPE_BDSTATUS: return "BDSTATUS";
    case PARAM_TYPE_BINARY:   return "BINARY";
    case PARAM_TYPE_STRING:   return "STRING";
    case PARAM_TYPE_ENUM:     return "ENUM";
    default:                  return "UNKNOWN";
    }
}

static const char *paramModeName(unsigned m) {
    switch (m) {
    case PARAM_MODE_RDONLY: return "RD";
    case PARAM_MODE_WRONLY: return "WR";
    case PARAM_MODE_RDWR:  return "RW";
    default:               return "??";
    }
}

static const char *paramUnitName(unsigned short u) {
    switch (u) {
    case PARAM_UN_NONE:    return "";
    case PARAM_UN_AMPERE:  return "A";
    case PARAM_UN_VOLT:    return "V";
    case PARAM_UN_WATT:    return "W";
    case PARAM_UN_CELSIUS: return "°C";
    case PARAM_UN_HERTZ:   return "Hz";
    case PARAM_UN_BAR:     return "bar";
    case PARAM_UN_VPS:     return "V/s";
    case PARAM_UN_SECOND:  return "s";
    case PARAM_UN_RPM:     return "rpm";
    case PARAM_UN_COUNT:   return "cnt";
    default:               return "?";
    }
}

static const char *expPrefix(short e) {
    switch (e) {
    case  6: return "M";
    case  3: return "k";
    case  0: return "";
    case -3: return "m";
    case -6: return "µ";
    default: return "?";
    }
}

static void printParamList(const std::vector<ParamInfo> &params)
{
    for (const auto &pi : params) {
        std::string extra;
        if (pi.isFloat()) {
            extra = fmt::format("  [{:.1f} .. {:.1f}] {}{}",
                                pi.minval, pi.maxval,
                                expPrefix(pi.exp), paramUnitName(pi.unit));
        }
        std::cout << fmt::format("    {:<16s}  {:10s}  {:2s}{}\n",
                                 pi.name, paramTypeName(pi.type),
                                 paramModeName(pi.mode), extra);
    }
}

static void dump_board_params(const std::vector<CAEN_Crate*> &crates)
{
    std::set<std::string> seen_models;

    for (auto *cr : crates) {
        for (auto *bd : cr->GetBoardList()) {
            std::string key = bd->GetModel();
            bool first_time = seen_models.insert(key).second;

            if (!first_time) {
                std::cout << fmt::format(
                    "--- {} slot {} — Model {} (same as above, skipped)\n",
                    cr->GetName(), bd->GetSlot(), bd->GetModel());
                continue;
            }

            std::cout << fmt::format(
                "\n═══ {} slot {} — Model {} ({} ch, serial {}, fw {}) ═══\n",
                cr->GetName(), bd->GetSlot(), bd->GetModel(),
                bd->GetSize(), bd->GetSerialNum(), bd->GetFirmware());

            std::cout << "\n  Board parameters:\n";
            if (bd->GetBdParamInfo().empty())
                std::cout << "    (none discovered)\n";
            else
                printParamList(bd->GetBdParamInfo());

            std::cout << "\n  Channel parameters (ch 0):\n";
            if (bd->GetChParamInfo().empty())
                std::cout << "    (none discovered)\n";
            else
                printParamList(bd->GetChParamInfo());

            std::cout << "\n";
        }
    }

    if (seen_models.empty())
        std::cout << "No boards found in any crate.\n";
    else
        std::cout << fmt::format("\nDone — {} unique board model(s) queried.\n",
                                 seen_models.size());
}

// ── Pull in MOC-generated code for the header-only QObjects ──────────────────
#include "moc_hv_monitor.cpp"
#include "moc_booster_monitor.cpp"
