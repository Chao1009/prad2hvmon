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
#include <QWebEngineView>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QUrl>
#include <QFile>
#include <QDir>

#include <ConfigParser.h>
#include <ConfigOption.h>
#include <caen_channel.h>
#include <fmt/format.h>

#include "hv_monitor.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>

// ── Crate list (shared by every mode) ────────────────────────────────────────
static std::vector<std::pair<std::string, std::string>> crate_list = {
    {"PRadHV_1", "129.57.160.67"},
    {"PRadHV_2", "129.57.160.68"},
    {"PRadHV_3", "129.57.160.69"},
    {"PRadHV_4", "129.57.160.70"},
    {"PRadHV_5", "129.57.160.71"},
};

// ── Forward declarations (console helpers, unchanged) ────────────────────────
static bool init_crates_console(std::vector<CAEN_Crate*> &crates,
                                std::map<std::string, CAEN_Crate*> &crate_map);
static void print_channels(std::vector<CAEN_Crate*> &crates,
                           const std::string &save_path);
static void write_channels(std::map<std::string, CAEN_Crate*> &crate_map,
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
    co.SetDesc("usage: %0 <mode> [gui, read, write]");
    co.SetDesc('f', "path to the channel voltage-setting file (write mode).");
    co.SetDesc('s', "path to save channel readings (read mode, optional).");
    co.SetDesc('p', "poll interval in ms for GUI mode (default 3000).");
    co.SetDesc('m', "path to module geometry JSON file (GUI mode).");
    co.SetDesc('h', "show help messages.");

    co.AddOpts(ConfigOption::arg_require, 'f', "file");
    co.AddOpts(ConfigOption::arg_require, 's', "save");
    co.AddOpts(ConfigOption::arg_require, 'p', "poll");
    co.AddOpts(ConfigOption::arg_require, 'm', "module-geo");
    co.AddOpts(ConfigOption::help_message, 'h', "help");

    if (!co.ParseArgs(argc, argv)) {
        std::cout << co.GetInstruction() << std::endl;
        return -1;
    }

    std::string setting_file, save_file, module_geo_file;
    int poll_ms = 3000;

    for (auto &opt : co.GetOptions()) {
        switch (opt.mark) {
        case 'f': setting_file    = opt.var.String(); break;
        case 's': save_file       = opt.var.String(); break;
        case 'p': poll_ms         = opt.var.Int();    break;
        case 'm': module_geo_file = opt.var.String(); break;
        }
    }

    // Default mode is "gui" when no positional argument is given
    std::string mode = (co.NbofArgs() >= 1) ? co.GetArgument(0).String()
                                            : "gui";

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

        HVMonitor monitor(crate_list, moduleGeoPath, poll_ms);
        if (!monitor.initCrates()) {
            std::cerr << "WARNING: not all crates connected – "
                         "dashboard will show partial data.\n";
        }

        // Web channel (C++ <-> JS bridge)
        QWebChannel channel;
        channel.registerObject(QStringLiteral("hvMonitor"), &monitor);

        // Web view
        QWebEngineView view;
        view.setWindowTitle("PRad-II HV Monitor");
        view.resize(1400, 900);
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
            return -1;
        }
        view.setUrl(QUrl::fromLocalFile(QDir(htmlPath).absolutePath()));
        view.show();

        // Start periodic polling
        monitor.startPolling();

        return app.exec();
    }

    // ── Console modes (read / write) ────────────────────────────────────
    std::vector<CAEN_Crate*> crates;
    std::map<std::string, CAEN_Crate*> crate_map;

    if (!init_crates_console(crates, crate_map)) {
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

static bool init_crates_console(std::vector<CAEN_Crate*> &crates,
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

static void print_channels(std::vector<CAEN_Crate*> &crates,
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

static void write_channels(std::map<std::string, CAEN_Crate*> &crate_map,
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

        auto *crate = crate_map[crate_name];
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

// ── Pull in MOC-generated code for the header-only QObject ──────────────────
#include "moc_hv_monitor.cpp"
