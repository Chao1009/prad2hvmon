// ─────────────────────────────────────────────────────────────────────────────
// prad2hvd – PRad-II HV Daemon
//
// A headless daemon that:
//   1. Connects to CAEN SY1527 crates and TDK-Lambda GEN booster supplies
//   2. Polls them continuously on background threads
//   3. Logs fault transitions to rotating daily log files
//   4. Serves live data to clients via WebSocket (JSON protocol)
//   5. Accepts control commands from clients (power, voltage, etc.)
//
// No Qt dependency — pure C++17.
//
// Usage:
//   prad2hvd [-c crates.json] [-m modules.json] [-g gui_config.json]
//            [-d daq_map.json] [-p port] [-i poll_ms]
//
// Author: Chao Peng — Argonne National Laboratory
// ─────────────────────────────────────────────────────────────────────────────

// Include ws_server.h BEFORE hv_daemon.h — websocketpp's internal md5.hpp
// defines/undefines a SET macro that collides with caenhvwrapper.h's #define SET 1.
// By including websocketpp first, its SET is fully consumed before CAEN's appears.
#include "ws_server.h"
#include "hv_daemon.h"
#include "file_fault_logger.h"

#include <nlohmann/json.hpp>
#include <fmt/format.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <getopt.h>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;


// ── Global stop flag (set by signal handler) ─────────────────────────────────
static std::atomic<bool> g_running{true};
static WsServer *g_server = nullptr;  // for signal handler access

static void signalHandler(int /*sig*/)
{
    // Async-signal-safe: only set atomics and call stop().
    // write() to stderr is technically safe; fmt::format is not, so we
    // use a raw write here.
    g_running = false;
    const char msg[] = "\nCaught signal — shutting down...\n";
    (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    if (g_server) g_server->stop();
}


// ── Load crate list from JSON ────────────────────────────────────────────────
static std::vector<HVPoller::CrateDef>
loadCrateList(const std::string &path)
{
    std::vector<HVPoller::CrateDef> list;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot open crate config: " << path << "\n";
        return list;
    }
    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        std::cerr << "ERROR: invalid JSON in crate config: " << path << "\n";
        return list;
    }
    for (const auto &obj : doc) {
        list.emplace_back(obj.value("name", ""), obj.value("ip", ""));
    }
    std::cout << fmt::format("Loaded {} crate(s) from {}\n", list.size(), path);
    return list;
}

// ── Load booster definitions from modules JSON ───────────────────────────────
static std::vector<BoosterPoller::SupplyDef>
loadBoosterDefs(const std::string &modulesPath, const std::string &guiConfigPath)
{
    std::vector<BoosterPoller::SupplyDef> defs;

    // Source 1: hycal_modules.json entries with "t":"booster"
    if (!modulesPath.empty()) {
        std::ifstream f(modulesPath);
        if (f.is_open()) {
            json mods = json::parse(f, nullptr, false);
            if (mods.is_array()) {
                for (const auto &m : mods) {
                    std::string t = m.value("t", "");
                    // Case-insensitive compare
                    std::string tl = t;
                    std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
                    if (tl != "booster") continue;

                    std::string name = m.value("n", "");
                    std::string ip   = m.value("ip", "");
                    uint16_t    port = m.value("port", 8003);
                    if (name.empty() || ip.empty()) {
                        std::cerr << "WARNING: booster entry missing 'n' or 'ip' — skipped\n";
                        continue;
                    }
                    defs.push_back({ name, ip, port });
                    std::cout << fmt::format("Booster {} @ {}:{} (from modules)\n",
                                             name, ip, port);
                }
            }
        }
    }

    // Source 2: gui_config.json "booster" array (fallback)
    if (defs.empty() && !guiConfigPath.empty()) {
        std::ifstream f(guiConfigPath);
        if (f.is_open()) {
            json cfg = json::parse(f, nullptr, false);
            if (cfg.is_object() && cfg.contains("booster")) {
                for (const auto &bs : cfg["booster"]) {
                    std::string name = bs.value("name", "");
                    std::string ip   = bs.value("ip",   "");
                    uint16_t    port = bs.value("port",  8003);
                    if (name.empty() || ip.empty()) continue;
                    defs.push_back({ name, ip, port });
                    std::cout << fmt::format("Booster {} @ {}:{} (from gui_config)\n",
                                             name, ip, port);
                }
            }
        }
    }

    if (defs.empty())
        std::cerr << "WARNING: no booster supplies defined\n";
    return defs;
}

// ── Load error-ignore rules ──────────────────────────────────────────────────
static void loadErrorIgnoreRules(const std::string &path)
{
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) return;

    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return;

    std::vector<CAEN_Channel::ErrorIgnoreRule> rules;
    if (doc.contains("ignore")) {
        for (const auto &val : doc["ignore"]) {
            CAEN_Channel::ErrorIgnoreRule rule;
            rule.pattern = val.value("name", "");
            if (val.contains("errors")) {
                for (const auto &e : val["errors"])
                    rule.errors.push_back(e.get<std::string>());
            }
            if (!rule.pattern.empty() && !rule.errors.empty())
                rules.push_back(rule);
        }
    }
    CAEN_Channel::SetErrorIgnoreRules(rules);
    std::cout << fmt::format("Loaded {} error-ignore rule(s) from {}\n",
                             rules.size(), path);
}

// ── Load voltage limits ──────────────────────────────────────────────────────
static void loadVoltageLimits(const std::string &path)
{
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) return;

    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return;

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
    std::cout << fmt::format("Loaded {} voltage-limit rule(s) from {}\n",
                             count, path);
}


// ── Load ΔV warning rules ────────────────────────────────────────────────────
static void loadDvWarnRules(const std::string &path, ChannelClassifier &cls)
{
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) return;

    json doc = json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return;

    std::vector<DvWarnRule> rules;
    if (doc.contains("rules")) {
        for (const auto &val : doc["rules"]) {
            std::string pattern = val.value("pattern", "");
            float max_dv        = val.value("max_dv", 0.0f);
            if (pattern.empty() || max_dv <= 0) continue;
            rules.push_back({ pattern, max_dv });
        }
    }
    // Allow a top-level "default" field
    if (doc.contains("default")) {
        cls.setDefaultDv(doc["default"].get<float>());
    }
    cls.setDvRules(std::move(rules));
    std::cout << fmt::format("Loaded {} ΔV warning rule(s) from {}\n",
                             cls.dvRules().size(), path);
}


// ── Locate a file relative to a base directory ───────────────────────────────
static std::string findFile(const std::string &explicit_path,
                            const std::string &base_dir,
                            const std::string &filename)
{
    if (!explicit_path.empty()) return explicit_path;
    std::string candidate = base_dir + "/" + filename;
    if (fs::exists(candidate)) return candidate;
    return "";
}


// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
static void printUsage(const char *prog)
{
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -c <file>   Crate config JSON       (default: $DATABASE_DIR/crates.json)\n"
              << "  -m <file>   Module geometry JSON     (default: $DATABASE_DIR/hycal_modules.json)\n"
              << "  -g <file>   GUI config JSON          (default: $DATABASE_DIR/gui_config.json)\n"
              << "  -d <file>   DAQ map JSON             (default: $DATABASE_DIR/daq_map.json)\n"
              << "  -i <file>   Error-ignore JSON        (default: $DATABASE_DIR/error_ignore.json)\n"
              << "  -l <file>   Voltage-limits JSON      (default: $DATABASE_DIR/voltage_limits.json)\n"
              << "  -w <file>   DeltaV warning rules JSON (default: $DATABASE_DIR/dv_warn.json)\n"
              << "  -p <port>   WebSocket port           (default: 8765)\n"
              << "  -t <ms>     Poll interval in ms      (default: 2000)\n"
              << "  -h          Show this help\n";
}

int main(int argc, char *argv[])
{
    // ── Defaults ─────────────────────────────────────────────────────────
    std::string crateFile, moduleFile, guiConfigFile, daqMapFile;
    std::string ignoreFile, limitsFile, dvWarnFile;
    uint16_t    wsPort       = 8765;
    int         pollInterval = 2000;

    // DATABASE_DIR is a compile-time define (same as the original project)
#ifdef DATABASE_DIR
    std::string dbDir = DATABASE_DIR;
#else
    std::string dbDir = ".";
#endif

    // ── Parse command-line ───────────────────────────────────────────────
    int opt;
    while ((opt = getopt(argc, argv, "c:m:g:d:i:l:w:p:t:h")) != -1) {
        switch (opt) {
        case 'c': crateFile     = optarg; break;
        case 'm': moduleFile    = optarg; break;
        case 'g': guiConfigFile = optarg; break;
        case 'd': daqMapFile    = optarg; break;
        case 'i': ignoreFile    = optarg; break;
        case 'l': limitsFile    = optarg; break;
        case 'w': dvWarnFile    = optarg; break;
        case 'p': wsPort        = static_cast<uint16_t>(std::atoi(optarg)); break;
        case 't': pollInterval  = std::atoi(optarg); break;
        case 'h':
        default:  printUsage(argv[0]); return (opt == 'h') ? 0 : 1;
        }
    }

    // ── Locate config files ──────────────────────────────────────────────
    crateFile     = findFile(crateFile,     dbDir, "crates.json");
    moduleFile    = findFile(moduleFile,    dbDir, "hycal_modules.json");
    guiConfigFile = findFile(guiConfigFile, dbDir, "gui_config.json");
    daqMapFile    = findFile(daqMapFile,    dbDir, "daq_map.json");
    ignoreFile    = findFile(ignoreFile,    dbDir, "error_ignore.json");
    limitsFile    = findFile(limitsFile,    dbDir, "voltage_limits.json");
    dvWarnFile    = findFile(dvWarnFile,    dbDir, "dv_warn.json");

    if (crateFile.empty()) {
        std::cerr << "ERROR: cannot find crates.json — use -c <path>\n";
        return 1;
    }

    // ── Load configs ─────────────────────────────────────────────────────
    auto crateList   = loadCrateList(crateFile);
    auto boosterDefs = loadBoosterDefs(moduleFile, guiConfigFile);

    if (crateList.empty()) {
        std::cerr << "ERROR: no crates defined\n";
        return 1;
    }

    loadErrorIgnoreRules(ignoreFile);
    loadVoltageLimits(limitsFile);

    // ── Shared state ─────────────────────────────────────────────────────
    SnapshotStore store;
    CommandQueue  cmdq;

    std::string logDir = dbDir + "/fault_log";
    FileFaultLogger faultLogger(logDir);
    std::cout << "Fault logger: " << logDir << "/\n";

    // ── HV Poller ────────────────────────────────────────────────────────
    HVPoller hvPoller(crateList);
    hvPoller.setFaultLogger(&faultLogger);
    hvPoller.setPollInterval(pollInterval);
    loadDvWarnRules(dvWarnFile, hvPoller.classifier());

    if (!hvPoller.initCrates()) {
        std::cerr << "WARNING: not all crates connected — "
                     "daemon will serve partial data.\n";
    }

    // ── Booster Poller ───────────────────────────────────────────────────
    BoosterPoller bstPoller(boosterDefs);
    bstPoller.setFaultLogger(&faultLogger);
    bstPoller.setPollInterval(pollInterval);

    // ── Signal handling ──────────────────────────────────────────────────
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ── Start poll threads ───────────────────────────────────────────────
    std::thread hvThread([&]() {
        hvPoller.run(store, cmdq, g_running);
    });

    std::thread bstThread([&]() {
        bstPoller.run(store, cmdq, g_running);
    });

    std::cout << fmt::format("Poll threads started (interval: {} ms)\n",
                             pollInterval);

    // ── Start WebSocket server (blocks on main thread) ───────────────────
    WsServer server(wsPort, store, cmdq, moduleFile, guiConfigFile, daqMapFile);
    g_server = &server;

    server.run();   // blocks until stop() is called

    // ── Shutdown ─────────────────────────────────────────────────────────
    std::cout << "Waiting for poll threads to finish...\n";
    g_running = false;
    hvThread.join();
    bstThread.join();
    g_server = nullptr;

    std::cout << "Daemon stopped.\n";
    return 0;
}
