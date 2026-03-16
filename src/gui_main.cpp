// ─────────────────────────────────────────────────────────────────────────────
// prad2hvmon – PRad-II HV Monitor (Qt GUI client + CLI read/write tool)
//
// Modes:
//   gui (default)              – launch Qt WebEngine dashboard
//   read  [-s file.json]       – save all writable params to JSON
//   write -f file.json         – restore writable params from JSON
//
// GUI mode connects to the prad2hvd daemon via WebSocket.
// CLI read/write modes connect directly to CAEN crates (no daemon needed).
//
// Usage:
//   prad2hvmon                              # GUI mode (default)
//   prad2hvmon -H clonpc19                 # GUI: connect to daemon on clonpc19
//   prad2hvmon read -s snapshot.json        # save settings
//   prad2hvmon write -f snapshot.json       # restore settings
//
// Author: Chao Peng — Argonne National Laboratory
// ─────────────────────────────────────────────────────────────────────────────

#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QUrl>
#include <QUrlQuery>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QPixmap>
#include <QDateTime>

#include <caen_channel.h>
#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cmath>
#include <chrono>
#include <unistd.h>

using json = nlohmann::json;


// ─────────────────────────────────────────────────────────────────────────────
//  Shared helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::pair<std::string, std::string>>
loadCrateList(const std::string &path)
{
    std::vector<std::pair<std::string, std::string>> list;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot open crate config: " << path << "\n";
        return list;
    }
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        std::cerr << "ERROR: invalid JSON in crate config\n";
        return list;
    }
    for (const auto &val : doc)
        list.emplace_back(val.value("name", ""), val.value("ip", ""));
    std::cout << fmt::format("Loaded {} crate(s) from {}\n", list.size(), path);
    return list;
}

static void loadVoltageLimits(const std::string &path)
{
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) return;
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded()) return;
    CAEN_Channel::ClearVoltageLimits();
    int count = 0;
    if (doc.contains("limits")) {
        for (const auto &val : doc["limits"]) {
            std::string pattern = val.value("pattern", "");
            double voltage      = val.value("voltage", 0.0);
            if (pattern.empty() || voltage <= 0) continue;
            CAEN_Channel::SetVoltageLimit(pattern, static_cast<float>(voltage));
            ++count;
        }
    }
    if (count > 0)
        std::cout << fmt::format("Loaded {} voltage-limit rule(s) from {}\n", count, path);
}

static void loadErrorIgnoreRules(const std::string &path)
{
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) return;
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded()) return;
    std::vector<CAEN_Channel::ErrorIgnoreRule> rules;
    if (doc.contains("ignore")) {
        for (const auto &val : doc["ignore"]) {
            CAEN_Channel::ErrorIgnoreRule rule;
            rule.pattern = val.value("name", "");
            if (val.contains("errors"))
                for (const auto &e : val["errors"])
                    rule.errors.push_back(e.get<std::string>());
            if (!rule.pattern.empty() && !rule.errors.empty())
                rules.push_back(rule);
        }
    }
    CAEN_Channel::SetErrorIgnoreRules(rules);
    if (!rules.empty())
        std::cout << fmt::format("Loaded {} error-ignore rule(s)\n", rules.size());
}


// ─────────────────────────────────────────────────────────────────────────────
//  CLI crate init
// ─────────────────────────────────────────────────────────────────────────────
static bool initCratesCLI(
    const std::vector<std::pair<std::string, std::string>> &defs,
    std::vector<CAEN_Crate*> &crates,
    std::map<std::string, CAEN_Crate*> &crate_map)
{
    int crid = 0;
    for (const auto &[name, ip] : defs) {
        auto *cr = new CAEN_Crate(crid++, name, ip,
                                  CAENHV::SY1527, LINKTYPE_TCPIP,
                                  "admin", "admin");
        crates.push_back(cr);
        crate_map[name] = cr;
    }
    int ok = 0;
    for (auto *cr : crates) {
        if (cr->Initialize()) {
            std::cout << fmt::format("Connected to {} @ {}\n", cr->GetName(), cr->GetIP());
            cr->PrintCrateMap();
            ++ok;
        } else {
            std::cerr << fmt::format("Cannot connect to {} @ {}\n", cr->GetName(), cr->GetIP());
        }
    }
    std::cout << fmt::format("Init DONE — {}/{} crates OK\n", ok, crates.size());
    return (ok == static_cast<int>(crates.size()));
}


// ─────────────────────────────────────────────────────────────────────────────
//  CLI read: save all writable params to JSON
// ─────────────────────────────────────────────────────────────────────────────
static void doRead(const std::vector<CAEN_Crate*> &crates,
                   const std::string &save_path)
{
    for (auto *cr : crates) cr->ReadAllParams();

    json root;
    root["format"]    = "prad2hvmon_settings_v1";
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char tbuf[32];
    std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
    root["timestamp"] = tbuf;

    json channels = json::array();
    int total = 0;

    for (auto *cr : crates) {
        for (auto *bd : cr->GetBoardList()) {
            const auto &paramInfo = bd->GetChParamInfo();
            for (auto *ch : bd->GetChannelList()) {
                json entry;
                entry["crate"]   = cr->GetName();
                entry["slot"]    = bd->GetSlot();
                entry["channel"] = ch->GetChannel();
                entry["name"]    = ch->GetName();

                json params;
                for (const auto &pi : paramInfo) {
                    if (!pi.isWritable()) continue;
                    if (pi.isFloat()) {
                        float v = ch->GetFloat(pi.name);
                        if (!std::isnan(v)) params[pi.name] = v;
                    } else if (pi.isUInt()) {
                        if (ch->HasParam(pi.name))
                            params[pi.name] = ch->GetUInt(pi.name);
                    }
                }
                entry["params"] = params;
                channels.push_back(std::move(entry));
                ++total;
            }
        }
    }
    root["channels"] = channels;

    std::string jsonStr = root.dump(2);
    std::cout << fmt::format("Read {} channels from {} crate(s)\n", total, crates.size());

    if (!save_path.empty()) {
        std::ofstream f(save_path);
        f << jsonStr;
        std::cout << "Saved to " << save_path << "\n";
    } else {
        std::cout << jsonStr << std::endl;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  CLI write: restore writable params from JSON
// ─────────────────────────────────────────────────────────────────────────────
static void doWrite(const std::vector<CAEN_Crate*> &crates,
                    const std::map<std::string, CAEN_Crate*> &crate_map,
                    const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot open settings file: " << path << "\n";
        return;
    }
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded()) {
        std::cerr << "ERROR: invalid JSON in settings file\n";
        return;
    }

    std::string format = doc.value("format", "");
    if (format != "prad2hvmon_settings_v1")
        std::cerr << "WARNING: unknown format '" << format << "'\n";
    std::cout << "Settings file timestamp: " << doc.value("timestamp", "?") << "\n";

    if (!doc.contains("channels") || !doc["channels"].is_array()) {
        std::cerr << "ERROR: 'channels' array not found\n";
        return;
    }

    // Read current params first for board param info
    for (auto *cr : crates) cr->ReadAllParams();

    int restored = 0, skipped = 0, errors = 0;

    for (const auto &entry : doc["channels"]) {
        std::string crate_name = entry.value("crate", "");
        int slot    = entry.value("slot", -1);
        int channel = entry.value("channel", -1);
        std::string ch_name = entry.value("name", "");

        auto cit = crate_map.find(crate_name);
        if (cit == crate_map.end()) { ++skipped; continue; }
        auto *board = cit->second->GetBoard(static_cast<unsigned short>(slot));
        if (!board) { ++skipped; continue; }
        auto *ch = board->GetChannel(channel);
        if (!ch) { ++skipped; continue; }

        if (!ch_name.empty() && ch->GetName() != ch_name) {
            ch->SetName(ch_name);
            std::cout << fmt::format("  {}/s{}/ch{} name → {}\n",
                crate_name, slot, channel, ch_name);
        }

        if (!entry.contains("params") || !entry["params"].is_object()) continue;
        const auto &paramInfo = board->GetChParamInfo();

        for (auto it = entry["params"].begin(); it != entry["params"].end(); ++it) {
            std::string pname = it.key();
            const ParamInfo *pi = nullptr;
            for (const auto &info : paramInfo)
                if (info.name == pname && info.isWritable()) { pi = &info; break; }
            if (!pi) continue;

            bool ok = false;
            if (pi->isFloat()) {
                float v = it.value().get<float>();
                ok = ch->SetFloat(pname, v);
                if (ok) std::cout << fmt::format("  {}/s{}/ch{} {} → {:.2f}\n",
                    crate_name, slot, channel, pname, v);
            } else if (pi->isUInt()) {
                unsigned int v = it.value().get<unsigned int>();
                ok = ch->SetUInt(pname, v);
                if (ok) std::cout << fmt::format("  {}/s{}/ch{} {} → {}\n",
                    crate_name, slot, channel, pname, v);
            }
            if (ok) ++restored; else ++errors;
        }
    }
    std::cout << fmt::format("\nDone — {} params restored, {} skipped, {} errors\n",
        restored, skipped, errors);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Usage
// ─────────────────────────────────────────────────────────────────────────────
static void printUsage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << "                              # GUI mode (default)\n"
        << "  " << prog << " [gui] [-H host] [-p port]    # GUI with daemon address\n"
        << "  " << prog << " read  [-s file.json]          # save all writable params\n"
        << "  " << prog << " write -f file.json            # restore params from JSON\n"
        << "\nGUI options:\n"
        << "  -H <host>   Daemon hostname (default: localhost)\n"
        << "  -p <port>   Daemon WebSocket port (default: 8765)\n"
        << "  -r <dir>    Resources directory\n"
        << "\nCLI options:\n"
        << "  -c <file>   Crate config JSON (default: database/crates.json)\n"
        << "  -l <file>   Voltage-limits JSON\n"
        << "  -i <file>   Error-ignore JSON\n"
        << "  -s <file>   Save output path (read mode)\n"
        << "  -f <file>   Settings file to load (write mode)\n"
        << "  -h          Show this help\n";
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

    // Remove mode argument from argv so getopt doesn't choke on it
    int eff_argc = argc;
    if (mode_arg_idx > 0) {
        for (int i = mode_arg_idx; i < argc - 1; i++)
            argv[i] = argv[i + 1];
        eff_argc = argc - 1;
    }

    // Parse options
    std::string host = "localhost", port_str = "8765", resourceDir;
    std::string crateFile, limitsFile, ignoreFile, saveFile, settingsFile;

    optind = 1;
    int opt;
    while ((opt = getopt(eff_argc, argv, "H:p:r:c:l:i:s:f:h")) != -1) {
        switch (opt) {
        case 'H': host         = optarg; break;
        case 'p': port_str     = optarg; break;
        case 'r': resourceDir  = optarg; break;
        case 'c': crateFile    = optarg; break;
        case 'l': limitsFile   = optarg; break;
        case 'i': ignoreFile   = optarg; break;
        case 's': saveFile     = optarg; break;
        case 'f': settingsFile = optarg; break;
        case 'h': printUsage(argv[0]); return 0;
        default:  printUsage(argv[0]); return 1;
        }
    }

    std::string dbDir = DATABASE_DIR;

    // ══════════════════════════════════════════════════════════════════════
    //  GUI mode
    // ══════════════════════════════════════════════════════════════════════
    if (mode == "gui") {
        QApplication app(argc, argv);  // pass original argc/argv for Qt
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
    //  CLI modes (read / write)
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

    // Locate config files
    if (crateFile.empty())  crateFile  = dbDir + "/crates.json";
    if (limitsFile.empty()) limitsFile = dbDir + "/voltage_limits.json";
    if (ignoreFile.empty()) ignoreFile = dbDir + "/error_ignore.json";

    auto crateList = loadCrateList(crateFile);
    if (crateList.empty()) {
        std::cerr << "ERROR: no crates defined\n";
        return 1;
    }

    loadVoltageLimits(limitsFile);
    loadErrorIgnoreRules(ignoreFile);

    std::vector<CAEN_Crate*> crates;
    std::map<std::string, CAEN_Crate*> crate_map;

    if (!initCratesCLI(crateList, crates, crate_map)) {
        std::cerr << "ERROR: crate initialisation failed\n";
        for (auto *c : crates) delete c;
        return 1;
    }

    if (mode == "read")
        doRead(crates, saveFile);
    else
        doWrite(crates, crate_map, settingsFile);

    for (auto *c : crates) delete c;
    return 0;
}
