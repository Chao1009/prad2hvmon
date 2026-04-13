#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// VMonRecorder – binary VMon data logger  (VMDF v1 format)
//
// Writes high-frequency VMon readings to daily binary files for long-term
// stability analysis.  Designed for crash-safe append-only operation.
//
// File format ("VMDF v1"):
//
//   HEADER  (20 bytes, written once at file creation)
//     char[4]   magic        "VMD1"
//     uint16    version      1
//     uint16    n_channels   number of HV channels
//     uint16    interval_ms  nominal poll interval
//     uint16    flags        reserved (0)
//     int64     t0_epoch_ms  epoch-ms of file creation
//
//   RECORDS  (tagged, variable-length, appended sequentially)
//
//     Tag 0x01 — Channel Table
//       uint8    tag          0x01
//       uint32   dt_ms        offset from t0
//       char[12] names[]      n_channels × 12-byte null-padded names
//
//     Tag 0x02 — VMon Snapshot
//       uint8    tag          0x02
//       uint32   dt_ms        offset from t0
//       float32  vmon[]       n_channels × 4-byte IEEE 754 values
//                             (NaN = channel offline / missing)
//
// Daily rotation: a new file is started each calendar day.
// Crash recovery: on open, an existing file is scanned forward from the
// header; any partial trailing record is truncated.
// ─────────────────────────────────────────────────────────────────────────────

#include <caen_channel.h>
#include <fmt/format.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class VMonRecorder
{
public:
    explicit VMonRecorder(const std::string &dir, uint16_t interval_ms = 200)
        : dir_(dir), interval_ms_(interval_ms)
    {
        if (!dir_.empty()) {
            fs::create_directories(dir_);
            std::cout << "VMon recorder: " << dir_ << "/\n";
        }
    }

    ~VMonRecorder() { close(); }

    bool enabled() const { return !dir_.empty(); }

    // ── Called once after initCrates to register the channel layout ──────
    void setChannels(const std::vector<CAEN_Crate*> &crates)
    {
        channels_.clear();
        for (auto *cr : crates) {
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    channels_.push_back(ch);
                }
            }
        }
        n_ch_ = static_cast<uint16_t>(channels_.size());
        // Snapshot the current name list
        buildNameTable();
    }

    // ── Write one VMon snapshot (called every fast poll cycle) ───────────
    void writeSnapshot()
    {
        if (!enabled() || channels_.empty()) return;
        ensureOpen();
        if (!file_.is_open()) return;

        auto now_ms = epochMs();
        uint32_t dt = static_cast<uint32_t>(now_ms - t0_);

        uint8_t tag = TAG_VMON;
        file_.write(reinterpret_cast<const char*>(&tag), 1);
        file_.write(reinterpret_cast<const char*>(&dt),  4);

        for (auto *ch : channels_) {
            float v = ch->GetVMon();
            file_.write(reinterpret_cast<const char*>(&v), 4);
        }

        ++records_written_;

        // Flush periodically (every ~5 seconds = ~25 records at 200ms)
        if (records_written_ % 25 == 0)
            file_.flush();
    }

    // ── Call when a channel name changes (set_name command) ──────────────
    void onNameChange()
    {
        if (!enabled() || !file_.is_open()) return;
        buildNameTable();
        writeChannelTable();
    }

    void close()
    {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        current_date_.clear();
    }

private:
    static constexpr uint8_t  TAG_CHTABLE = 0x01;
    static constexpr uint8_t  TAG_VMON    = 0x02;
    static constexpr char     MAGIC[4]    = {'V','M','D','1'};
    static constexpr uint16_t VERSION     = 1;

    // ── Ensure the file for today is open, rotating if needed ───────────
    void ensureOpen()
    {
        std::string date = today();
        if (file_.is_open() && date == current_date_)
            return;

        // Close previous day
        close();
        current_date_ = date;

        std::string path = dir_ + "/vmon_" + date + ".dat";
        bool exists = fs::exists(path);

        if (exists) {
            // Re-open existing file, recover from potential crash
            recoverAndReopen(path);
        } else {
            openNewFile(path);
        }
    }

    // ── Create a brand-new file with header + channel table ─────────────
    void openNewFile(const std::string &path)
    {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            std::cerr << "VMonRecorder: cannot create " << path << "\n";
            return;
        }

        t0_ = epochMs();
        records_written_ = 0;

        // Write header
        file_.write(MAGIC, 4);
        file_.write(reinterpret_cast<const char*>(&VERSION),      2);
        file_.write(reinterpret_cast<const char*>(&n_ch_),        2);
        file_.write(reinterpret_cast<const char*>(&interval_ms_), 2);
        uint16_t flags = 0;
        file_.write(reinterpret_cast<const char*>(&flags),        2);
        file_.write(reinterpret_cast<const char*>(&t0_),          8);

        // Write initial channel table
        writeChannelTable();
        file_.flush();

        std::cout << fmt::format("VMonRecorder: opened {} ({} channels)\n",
                                 path, n_ch_);
    }

    // ── Recover an existing file: validate and truncate partial tail ────
    void recoverAndReopen(const std::string &path)
    {
        // Read the file to find the last valid record boundary
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) { openNewFile(path); return; }

        auto filesize = static_cast<int64_t>(in.tellg());
        if (filesize < 20) {
            in.close();
            openNewFile(path);
            return;
        }

        // Read header
        in.seekg(0);
        char magic[4];
        in.read(magic, 4);
        if (std::memcmp(magic, MAGIC, 4) != 0) {
            std::cerr << "VMonRecorder: bad magic in " << path << ", starting fresh\n";
            in.close();
            openNewFile(path);
            return;
        }

        uint16_t ver, n_ch, interval, flags;
        int64_t t0;
        in.read(reinterpret_cast<char*>(&ver),      2);
        in.read(reinterpret_cast<char*>(&n_ch),      2);
        in.read(reinterpret_cast<char*>(&interval),   2);
        in.read(reinterpret_cast<char*>(&flags),      2);
        in.read(reinterpret_cast<char*>(&t0),         8);

        if (n_ch != n_ch_) {
            std::cerr << fmt::format("VMonRecorder: channel count mismatch "
                                     "({} in file vs {} now), starting fresh\n",
                                     n_ch, n_ch_);
            in.close();
            openNewFile(path);
            return;
        }

        // Scan forward record by record to find last valid boundary
        int64_t pos = 20;  // after header
        int64_t last_good = pos;
        const int64_t chtable_size = 1 + 4 + n_ch_ * 12;
        const int64_t vmon_size    = 1 + 4 + n_ch_ * 4;

        while (pos < filesize) {
            if (pos + 1 > filesize) break;  // can't read tag
            in.seekg(pos);
            uint8_t tag;
            in.read(reinterpret_cast<char*>(&tag), 1);
            if (!in.good()) break;

            int64_t rec_size = 0;
            if      (tag == TAG_CHTABLE) rec_size = chtable_size;
            else if (tag == TAG_VMON)    rec_size = vmon_size;
            else break;  // unknown tag = corruption

            if (pos + rec_size > filesize) break;  // partial record
            pos += rec_size;
            last_good = pos;
        }
        in.close();

        // Truncate if needed
        if (last_good < filesize) {
            std::cerr << fmt::format("VMonRecorder: truncating {} from {} to {} bytes "
                                     "(partial record recovery)\n",
                                     path, filesize, last_good);
            fs::resize_file(path, static_cast<uintmax_t>(last_good));
        }

        // Reopen for appending
        file_.open(path, std::ios::binary | std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "VMonRecorder: cannot reopen " << path << "\n";
            return;
        }

        t0_ = t0;
        records_written_ = 0;

        // Write a fresh channel table (names may have changed since last run)
        writeChannelTable();
        file_.flush();

        std::cout << fmt::format("VMonRecorder: resumed {} ({} bytes, {} channels)\n",
                                 path, last_good, n_ch_);
    }

    // ── Write a tag-0x01 channel table record ───────────────────────────
    void writeChannelTable()
    {
        if (!file_.is_open()) return;

        auto now_ms = epochMs();
        uint32_t dt = static_cast<uint32_t>(now_ms - t0_);

        uint8_t tag = TAG_CHTABLE;
        file_.write(reinterpret_cast<const char*>(&tag), 1);
        file_.write(reinterpret_cast<const char*>(&dt),  4);
        file_.write(name_table_.data(), name_table_.size());
    }

    // ── Build the name table from current channel objects ────────────────
    void buildNameTable()
    {
        name_table_.resize(n_ch_ * 12, '\0');
        for (int i = 0; i < n_ch_; ++i) {
            const std::string &name = channels_[i]->GetName();
            size_t len = std::min<size_t>(name.size(), 11);
            std::memcpy(&name_table_[i * 12], name.c_str(), len);
            // Remaining bytes already zero from resize
            // Ensure null termination
            name_table_[i * 12 + len] = '\0';
        }
    }

    // ── Utilities ────────────────────────────────────────────────────────
    static std::string today()
    {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
        return buf;
    }

    static int64_t epochMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ── Data ─────────────────────────────────────────────────────────────
    std::string   dir_;
    uint16_t      interval_ms_;
    uint16_t      n_ch_ = 0;
    int64_t       t0_   = 0;
    std::string   current_date_;
    std::ofstream file_;
    uint64_t      records_written_ = 0;

    std::vector<CAEN_Channel*> channels_;   // ordered channel pointers
    std::vector<char>          name_table_; // packed 12-byte names
};
