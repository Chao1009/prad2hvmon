// ─────────────────────────────────────────────────────────────────────────────
// prad2hvmon – PRad-II High-Voltage Monitor
//
// Modes:
//   gui              – launch the Qt WebEngine dashboard  (default)
//   read  [-s file]  – one-shot console read
//   write -f file    – restore voltages from a settings file
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

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>


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
static std::vector<std::string>
load_error_ignore_list(const QString &path)
{
    std::vector<std::string> list;
    if (path.isEmpty()) return list;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "WARNING: cannot open error-ignore file: "
                  << path.toStdString() << "\n";
        return list;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        std::cerr << "WARNING: invalid JSON in error-ignore file: "
                  << err.errorString().toStdString() << "\n";
        return list;
    }
    for (const auto &val : doc.object()["ignore"].toArray())
        list.push_back(val.toString().toStdString());

    std::cout << fmt::format("Loaded {} error-ignore channel(s) from {}\n",
                             list.size(), path.toStdString());
    return list;
}


static bool init_crates_console(const std::vector<std::pair<std::string, std::string>> &crate_list,
                                std::vector<CAEN_Crate*> &crates,
                                std::map<std::string, CAEN_Crate*> &crate_map);
static void print_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::string &save_path);
static void write_channels(const std::map<std::string, CAEN_Crate*> &crate_map,
                           const std::string &setting_path);
inline void write_lines(std::ostream &out,
                        const std::vector<std::string> &lines);

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
    co.AddOpts(ConfigOption::arg_require, 's', "save");
    co.AddOpts(ConfigOption::arg_require, 'm', "module-geo");
    co.AddOpts(ConfigOption::help_message, 'h', "help");

    // help messages
    co.SetDesc("usage: %0 <mode> [gui, read, write]");
    co.SetDesc('c', "path to crates JSON file (default: auto-discover).");
    co.SetDesc('f', "path to the channel voltage-setting file (write mode).");
    co.SetDesc('i', "path to error-ignore JSON file (default: auto-discover).");
    co.SetDesc('s', "path to save channel readings (read mode, optional).");
    co.SetDesc('m', "path to module geometry JSON file (GUI mode).");
    co.SetDesc('h', "show help messages.");

    if (!co.ParseArgs(argc, argv)) {
        std::cout << co.GetInstruction() << std::endl;
        return -1;
    }

    std::string setting_file, save_file, module_geo_file, crate_config_file, ignore_file;

    for (auto &opt : co.GetOptions()) {
        switch (opt.mark) {
        case 'c': crate_config_file = opt.var.String(); break;
        case 'f': setting_file      = opt.var.String(); break;
        case 'i': ignore_file       = opt.var.String(); break;
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
            std::string(RESOURCE_DIR) + "/crates.json");
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
        CAEN_Channel::SetErrorIgnoreList(load_error_ignore_list(ignoreConfigPath));
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
                QCoreApplication::applicationDirPath() + "/../resources/hycal_modules.json",
                QCoreApplication::applicationDirPath() + "/../../resources/hycal_modules.json",
                QString::fromStdString(std::string(RESOURCE_DIR) + "/hycal_modules.json"),
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
                QCoreApplication::applicationDirPath() + "/../resources/gui_config.json",
                QCoreApplication::applicationDirPath() + "/../../resources/gui_config.json",
                QString::fromStdString(std::string(RESOURCE_DIR) + "/gui_config.json"),
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
                QCoreApplication::applicationDirPath() + "/../resources/daq_map.json",
                QCoreApplication::applicationDirPath() + "/../../resources/daq_map.json",
                QString::fromStdString(std::string(RESOURCE_DIR) + "/daq_map.json"),
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
            QPointer<QWebEngineView> geo;
            QPointer<QWebEngineView> booster;
        };
        auto detached = std::make_shared<DetachedViews>();

        QObject::connect(&monitor, &HVMonitor::detachRequested,
                         &view,
                         [&monitor, &bMonitor, htmlPath, detached](const QString &tabId)
        {
            QPointer<QWebEngineView> &slot =
                (tabId == QLatin1String("geo-tab")) ? detached->geo
                                                    : detached->booster;
            if (slot && slot->isVisible()) {
                slot->raise();
                slot->activateWindow();
                return;
            }
            auto *dv = new QWebEngineView();
            dv->setAttribute(Qt::WA_DeleteOnClose);
            dv->setWindowTitle(
                tabId == QLatin1String("geo-tab")
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
        boosterThread.quit();
        boosterThread.wait();
        return ret;
    }

    // ── Console modes (read / write) ────────────────────────────────────
    std::vector<CAEN_Crate*> crates;
    std::map<std::string, CAEN_Crate*> crate_map;

    if (!init_crates_console(crate_list, crates, crate_map)) {
        std::cerr << "Aborted! Crates initialisation failed!\n";
        return -1;
    }

    if (mode == "read") {
        print_channels(crates, save_file);
    } else if (mode == "write") {
        write_channels(crate_map, setting_file);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n";
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

inline void write_lines(std::ostream &out,
                        const std::vector<std::string> &lines)
{
    for (const auto &l : lines) out << l << '\n';
}

static void print_channels(const std::vector<CAEN_Crate*> &crates,
                           const std::string &save_path)
{
    std::vector<std::string> lines;
    lines.push_back(fmt::format("# {:10s} {:4s} {:8s} {:16s} {:8s} {:8s}",
                                "crate", "slot", "channel",
                                "name", "VMon", "VSet"));
    for (auto *cr : crates) {
        cr->ReadVoltage();
        for (auto *bd : cr->GetBoardList()) {
            for (auto *ch : bd->GetChannelList()) {
                lines.push_back(
                    fmt::format("{:12s} {:4d} {:8d} {:16s} {:8.2f} {:8.2f}",
                                cr->GetName(), bd->GetSlot(),
                                ch->GetChannel(), ch->GetName(),
                                ch->GetVMon(), ch->GetVSet()));
            }
        }
    }
    write_lines(std::cout, lines);
    if (!save_path.empty()) {
        std::ofstream f(save_path);
        write_lines(f, lines);
    }
}

static void write_channels(const std::map<std::string, CAEN_Crate*> &crate_map,
                           const std::string &setting_path)
{
    ConfigParser parser;
    parser.ReadFile(setting_path);

    std::string miss_crate;
    int miss_slot = -1;
    std::vector<std::string> missing;

    while (parser.ParseLine()) {
        std::string crate_name, ch_name;
        int slot;
        unsigned short channel;
        float VMon, VSet;

        if (parser.NbofElements() == 5)
            parser >> crate_name >> slot >> channel >> ch_name >> VSet;
        else if (parser.NbofElements() == 6)
            parser >> crate_name >> slot >> channel >> ch_name >> VMon >> VSet;

        auto *crate = crate_map.at(crate_name);
        auto *board = crate->GetBoard(slot);
        if (!board) {
            if (crate_name == miss_crate && slot == miss_slot) continue;
            missing.push_back(
                fmt::format("skipped crate: {:8s} slot: {:4d}, board not found!",
                            crate_name, slot));
            miss_crate = crate_name;
            miss_slot  = slot;
            continue;
        }
        auto *ch = board->GetChannel(channel);
        if (ch) {
            ch->SetName(ch_name);
            ch->SetVoltage(VSet);
            std::cout << fmt::format(
                "crate: {:8s} slot: {:4d} ch: {:4d} → {:12s} {:8.2f} V\n",
                crate_name, slot, channel, ch_name, VSet);
        } else {
            std::cout << fmt::format(
                "crate: {:8s} slot: {:4d} ch: {:4d} not found!\n",
                crate_name, slot, channel);
        }
    }
    for (const auto &m : missing) std::cout << m << '\n';
    std::cout << "Restored HV settings from " << setting_path << '\n';
}

// ── Pull in MOC-generated code for the header-only QObjects ──────────────────
#include "moc_hv_monitor.cpp"
#include "moc_booster_monitor.cpp"
