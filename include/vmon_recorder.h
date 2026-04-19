#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// VMonRecorder – binary dV data logger  (VMDF v2 format)
//
// Writes dV = VMon - V0Set for every HV channel at each fast poll cycle to
// daily binary files for long-term stability analysis.  Booster (TDK-Lambda)
// supplies are logged alongside: setpoints (VSet/ISet) in a "table" record
// that only re-emits on change, measurements (VMon/IMon) in a separate
// snapshot record that is also emit-on-change (so duplicates across HV fast
// polls are skipped naturally).  Recording dV rather than raw HV VMon keeps
// values near zero regardless of setpoint and improves compression.
// Append-only and crash-safe.
//
// File format ("VMD2"):
//
//   HEADER  (20 bytes, written once at file creation)
//     char[4]   magic        "VMD2"
//     uint16    version      2
//     uint16    n_channels   number of HV channels
//     uint16    interval_ms  nominal fast poll interval
//     uint16    n_boosters   number of booster supplies (0 = none recorded)
//     int64     t0_epoch_ms  epoch-ms of file creation
//
//   RECORDS  (tagged, variable-length, appended sequentially)
//
//     Tag 0x01 — HV Channel Table  (name + V0Set per channel)
//       uint8    tag          0x01
//       uint32   dt_ms        offset from t0
//       per channel (n_channels entries, 16 bytes each):
//         char[12] name       null-padded 12-byte name
//         float32  v0set      V0Set active when the table was written
//
//     Tag 0x02 — HV dV Snapshot
//       uint8    tag          0x02
//       uint32   dt_ms        offset from t0
//       float32  dv[]         n_channels × 4-byte IEEE 754 values
//                             dv[i] = VMon[i] - V0Set[i]
//                             (NaN if either side missing / offline)
//
//     Tag 0x03 — Booster Table  (name + VSet + ISet per booster)
//       uint8    tag          0x03
//       uint32   dt_ms        offset from t0
//       per booster (n_boosters entries, 20 bytes each):
//         char[12] name       null-padded 12-byte label
//         float32  vset       output-voltage setpoint
//         float32  iset       output-current setpoint
//
//     Tag 0x04 — Booster Snapshot  (VMon + IMon per booster)
//       uint8    tag          0x04
//       uint32   dt_ms        offset from t0
//       per booster (n_boosters entries, 8 bytes each):
//         float32  vmon       measured output voltage
//         float32  imon       measured output current
//                             (NaN if supply offline / unread)
//
// Tables (0x01, 0x03) are emitted at file open and any time they change;
// dV snapshots (0x02) every fast poll; booster snapshots (0x04) only when
// measurement bytes change so the file naturally carries at most the
// booster-poll rate rather than the HV fast-poll rate.
//
// Daily rotation: a new file is started each calendar day.
// Crash recovery: on open, an existing file is scanned forward from the
// header; any partial trailing record is truncated.
// ─────────────────────────────────────────────────────────────────────────────

#include <booster_supply.h>
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
            std::error_code ec;
            fs::create_directories(dir_, ec);
            if (ec) {
                std::cerr << "*** WARNING: VMonRecorder cannot create '"
                          << dir_ << "': " << ec.message()
                          << " — recording DISABLED\n";
                dir_.clear();   // disables enabled() → no write attempts later
                return;
            }
            std::cout << "VMon recorder: " << dir_ << "/\n";
        }
    }

    ~VMonRecorder() { close(); }

    bool enabled() const { return !dir_.empty(); }

    // ── Called once after initCrates to register the HV channel layout ───
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
        last_table_.clear();   // force a fresh channel table on first write
    }

    // ── Register booster supplies (optional) ─────────────────────────────
    //    Pointers are not owned; they must outlive the recorder.  Reads are
    //    racy with the BoosterPoller thread but bounded to aligned 4/8-byte
    //    fields so at worst we log a stale measurement — acceptable for a
    //    monitoring recorder.
    void setBoosters(const std::vector<BoosterSupply*> &supplies)
    {
        boosters_ = supplies;
        n_bst_ = static_cast<uint16_t>(supplies.size());
        last_booster_table_.clear();
        last_booster_snap_.clear();
    }

    // ── Write one fast-poll worth of records ─────────────────────────────
    void writeSnapshot()
    {
        if (!enabled() || channels_.empty()) return;
        ensureOpen();
        if (!file_.is_open()) return;

        // Tables first so any dV/booster snapshot that follows is consistent
        // with the latest names / setpoints on disk.
        maybeWriteChannelTable();
        maybeWriteBoosterTable();

        auto now_ms = epochMs();
        uint32_t dt = static_cast<uint32_t>(now_ms - t0_);

        // ── HV dV snapshot (Tag 0x02) ───────────────────────────────────
        uint8_t tag = TAG_DV;
        file_.write(reinterpret_cast<const char*>(&tag), 1);
        file_.write(reinterpret_cast<const char*>(&dt),  4);
        for (auto *ch : channels_) {
            float dv = ch->GetVMon() - ch->GetVSet();   // NaN-propagating
            file_.write(reinterpret_cast<const char*>(&dv), 4);
        }

        // ── Booster snapshot (Tag 0x04) — only when values change ───────
        maybeWriteBoosterSnapshot(dt);

        ++records_written_;
        if (records_written_ % 25 == 0)
            file_.flush();
    }

    // ── Call when an HV channel name changes (set_name / load_settings) ──
    void onNameChange()
    {
        if (!enabled() || !file_.is_open()) return;
        maybeWriteChannelTable();
    }

    void close()
    {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        current_date_.clear();
        last_table_.clear();
        last_booster_table_.clear();
        last_booster_snap_.clear();
    }

private:
    static constexpr uint8_t  TAG_CHTABLE       = 0x01;
    static constexpr uint8_t  TAG_DV            = 0x02;
    static constexpr uint8_t  TAG_BOOSTER_TABLE = 0x03;
    static constexpr uint8_t  TAG_BOOSTER       = 0x04;
    static constexpr char     MAGIC[4]          = {'V','M','D','2'};
    static constexpr uint16_t VERSION           = 2;
    static constexpr int      NAME_LEN          = 12;
    static constexpr int      CHREC_BYTES       = NAME_LEN + 4;    // name + V0Set
    static constexpr int      BST_TABLE_BYTES   = NAME_LEN + 4 + 4; // name + VSet + ISet
    static constexpr int      BST_SNAP_BYTES    = 4 + 4;            // VMon + IMon

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
            recoverAndReopen(path);
        } else {
            openNewFile(path);
        }
    }

    // ── Create a brand-new file with just the header ────────────────────
    void openNewFile(const std::string &path)
    {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            std::cerr << "VMonRecorder: cannot create " << path << "\n";
            return;
        }

        t0_ = epochMs();
        records_written_ = 0;

        file_.write(MAGIC, 4);
        file_.write(reinterpret_cast<const char*>(&VERSION),      2);
        file_.write(reinterpret_cast<const char*>(&n_ch_),        2);
        file_.write(reinterpret_cast<const char*>(&interval_ms_), 2);
        file_.write(reinterpret_cast<const char*>(&n_bst_),       2);
        file_.write(reinterpret_cast<const char*>(&t0_),          8);
        file_.flush();

        last_table_.clear();
        last_booster_table_.clear();
        last_booster_snap_.clear();

        std::cout << fmt::format("VMonRecorder: opened {} ({} HV channels, "
                                 "{} boosters)\n",
                                 path, n_ch_, n_bst_);
    }

    // ── Recover an existing file: validate and truncate partial tail ────
    void recoverAndReopen(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) { openNewFile(path); return; }

        auto filesize = static_cast<int64_t>(in.tellg());
        if (filesize < 20) {
            in.close();
            openNewFile(path);
            return;
        }

        in.seekg(0);
        char magic[4];
        in.read(magic, 4);
        if (std::memcmp(magic, MAGIC, 4) != 0) {
            std::cerr << "VMonRecorder: bad magic in " << path
                      << " (expected VMD2), starting fresh\n";
            in.close();
            openNewFile(path);
            return;
        }

        uint16_t ver, n_ch, interval, n_bst;
        int64_t t0;
        in.read(reinterpret_cast<char*>(&ver),      2);
        in.read(reinterpret_cast<char*>(&n_ch),     2);
        in.read(reinterpret_cast<char*>(&interval), 2);
        in.read(reinterpret_cast<char*>(&n_bst),    2);
        in.read(reinterpret_cast<char*>(&t0),       8);

        if (n_ch != n_ch_ || n_bst != n_bst_) {
            std::cerr << fmt::format(
                "VMonRecorder: shape mismatch (file={}ch/{}bst vs "
                "now={}ch/{}bst), starting fresh\n",
                n_ch, n_bst, n_ch_, n_bst_);
            in.close();
            openNewFile(path);
            return;
        }

        // Scan forward record by record to find last valid boundary
        int64_t pos = 20;  // after header
        int64_t last_good = pos;
        const int64_t chtable_size   = 1 + 4 + n_ch_ * CHREC_BYTES;
        const int64_t dv_size        = 1 + 4 + n_ch_ * 4;
        const int64_t bst_table_size = 1 + 4 + n_bst_ * BST_TABLE_BYTES;
        const int64_t bst_snap_size  = 1 + 4 + n_bst_ * BST_SNAP_BYTES;

        while (pos < filesize) {
            if (pos + 1 > filesize) break;  // can't read tag
            in.seekg(pos);
            uint8_t tag;
            in.read(reinterpret_cast<char*>(&tag), 1);
            if (!in.good()) break;

            int64_t rec_size = 0;
            switch (tag) {
                case TAG_CHTABLE:       rec_size = chtable_size;   break;
                case TAG_DV:            rec_size = dv_size;        break;
                case TAG_BOOSTER_TABLE: rec_size = bst_table_size; break;
                case TAG_BOOSTER:       rec_size = bst_snap_size;  break;
                default:                rec_size = 0;              break;
            }
            if (rec_size == 0) break;                // unknown tag = corruption
            if (pos + rec_size > filesize) break;    // partial record
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
        last_table_.clear();
        last_booster_table_.clear();
        last_booster_snap_.clear();
        file_.flush();

        std::cout << fmt::format("VMonRecorder: resumed {} ({} bytes, {} HV channels, "
                                 "{} boosters)\n",
                                 path, last_good, n_ch_, n_bst_);
    }

    // ── HV channel table: name + V0Set per channel ──────────────────────
    void buildCurrentTable(std::vector<char> &out) const
    {
        out.assign(static_cast<size_t>(n_ch_) * CHREC_BYTES, '\0');
        for (int i = 0; i < n_ch_; ++i) {
            char *slot = &out[i * CHREC_BYTES];
            const std::string &nm = channels_[i]->GetName();
            size_t len = std::min<size_t>(nm.size(), NAME_LEN - 1);
            std::memcpy(slot, nm.c_str(), len);
            float v0 = channels_[i]->GetVSet();
            std::memcpy(slot + NAME_LEN, &v0, sizeof(float));
        }
    }

    void maybeWriteChannelTable()
    {
        std::vector<char> cur;
        buildCurrentTable(cur);
        if (cur != last_table_) {
            writeTaggedBlob(TAG_CHTABLE, cur);
            last_table_ = std::move(cur);
        }
    }

    // ── Booster table: name + VSet + ISet per booster ───────────────────
    void buildCurrentBoosterTable(std::vector<char> &out) const
    {
        out.assign(static_cast<size_t>(n_bst_) * BST_TABLE_BYTES, '\0');
        for (int i = 0; i < n_bst_; ++i) {
            char *slot = &out[i * BST_TABLE_BYTES];
            const std::string &nm = boosters_[i]->name;
            size_t len = std::min<size_t>(nm.size(), NAME_LEN - 1);
            std::memcpy(slot, nm.c_str(), len);
            float vs = static_cast<float>(boosters_[i]->vset);
            float is = static_cast<float>(boosters_[i]->iset);
            std::memcpy(slot + NAME_LEN,     &vs, sizeof(float));
            std::memcpy(slot + NAME_LEN + 4, &is, sizeof(float));
        }
    }

    void maybeWriteBoosterTable()
    {
        if (n_bst_ == 0) return;
        std::vector<char> cur;
        buildCurrentBoosterTable(cur);
        if (cur != last_booster_table_) {
            writeTaggedBlob(TAG_BOOSTER_TABLE, cur);
            last_booster_table_ = std::move(cur);
        }
    }

    // ── Booster snapshot: VMon + IMon per booster ───────────────────────
    void buildCurrentBoosterSnap(std::vector<char> &out) const
    {
        out.assign(static_cast<size_t>(n_bst_) * BST_SNAP_BYTES, '\0');
        for (int i = 0; i < n_bst_; ++i) {
            char *slot = &out[i * BST_SNAP_BYTES];
            float vm = static_cast<float>(boosters_[i]->vmon);
            float im = static_cast<float>(boosters_[i]->imon);
            std::memcpy(slot,     &vm, sizeof(float));
            std::memcpy(slot + 4, &im, sizeof(float));
        }
    }

    // Uses the dt captured by writeSnapshot so the booster snapshot shares
    // a timestamp with the HV dV record written alongside it.
    void maybeWriteBoosterSnapshot(uint32_t dt)
    {
        if (n_bst_ == 0) return;
        std::vector<char> cur;
        buildCurrentBoosterSnap(cur);
        if (cur != last_booster_snap_) {
            uint8_t tag = TAG_BOOSTER;
            file_.write(reinterpret_cast<const char*>(&tag), 1);
            file_.write(reinterpret_cast<const char*>(&dt),  4);
            file_.write(cur.data(), cur.size());
            last_booster_snap_ = std::move(cur);
        }
    }

    // ── Shared helper: write tag + dt_now + payload ─────────────────────
    void writeTaggedBlob(uint8_t tag, const std::vector<char> &payload)
    {
        if (!file_.is_open()) return;
        uint32_t dt = static_cast<uint32_t>(epochMs() - t0_);
        file_.write(reinterpret_cast<const char*>(&tag), 1);
        file_.write(reinterpret_cast<const char*>(&dt),  4);
        file_.write(payload.data(), payload.size());
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
    uint16_t      n_ch_  = 0;
    uint16_t      n_bst_ = 0;
    int64_t       t0_    = 0;
    std::string   current_date_;
    std::ofstream file_;
    uint64_t      records_written_ = 0;

    std::vector<CAEN_Channel*>    channels_;           // ordered HV channels
    std::vector<BoosterSupply*>   boosters_;           // ordered boosters (may be empty)
    std::vector<char>             last_table_;         // most recent HV channel-table blob
    std::vector<char>             last_booster_table_; // most recent booster-table blob
    std::vector<char>             last_booster_snap_;  // most recent booster VMon/IMon blob
};
