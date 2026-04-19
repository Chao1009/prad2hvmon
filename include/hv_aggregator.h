#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HVAggregator — server-side aggregation of dV = VMon − V0Set per channel.
//
// On every fast poll the daemon calls push() for each live channel.  Each
// channel accumulates up to fast_n samples (default 5000, ~16.7 min at
// 200 ms/poll); when that fills, we compute {epoch_ms, mean, rms, n} and
// append it to the channel's ring of up to ring_n aggregation points
// (default 100, ~28 h of history at default fast size).  The ring is kept
// in memory only — if you need history across restarts, replay from the
// VMDF recorder files under $dataDir/vmon_data/.
//
// Power-off samples (bit 0 of ch->GetStatus() clear) or NaN readings reset
// the channel's fast buffer without producing an aggregation point, so the
// big VMon-collapses-to-zero artefact never enters the stats.
//
// Thread safety: push() runs on the HVPoller thread; fullSnapshotJson(),
// takeDeltaJson(), and setters run on the WS thread.  One internal mutex.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

class HVAggregator
{
public:
    struct Point {
        int64_t  epoch_ms;
        float    mean;
        float    rms;
        uint32_t n;
    };

    HVAggregator(size_t fast_n = 5000, size_t ring_n = 100)
        : fast_n_(fast_n < 10 ? 10 : fast_n),
          ring_n_(ring_n < 1  ? 1  : ring_n) {}

    void setFastN(size_t n) {
        if (n < 10) n = 10;
        std::lock_guard<std::mutex> lk(mu_);
        fast_n_ = n;
        // Don't truncate in-flight buffers — next flush uses new size.
    }
    void setRingN(size_t n) {
        if (n < 1) n = 1;
        std::lock_guard<std::mutex> lk(mu_);
        ring_n_ = n;
        for (auto &kv : channels_)
            while (kv.second.ring.size() > ring_n_) kv.second.ring.pop_front();
    }
    size_t fastN() const {
        std::lock_guard<std::mutex> lk(mu_);
        return fast_n_;
    }
    size_t ringN() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ring_n_;
    }

    // Called once per fast-poll per channel.
    void push(const std::string &name, float vmon, float vset,
              bool power_on, int64_t epoch_ms)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto &st = channels_[name];

        if (!power_on || std::isnan(vmon) || std::isnan(vset)) {
            // Don't aggregate an off or missing sample — clear partial fill
            // so the next ON epoch starts clean.
            st.fast.clear();
            return;
        }

        st.fast.push_back(vmon - vset);
        if (st.fast.size() < fast_n_) return;

        // ── Flush: compute stats and enqueue an aggregation point ──
        double sum = 0, sumSq = 0;
        for (float v : st.fast) { sum += v; sumSq += double(v) * v; }
        const double n_d = static_cast<double>(st.fast.size());
        const double mean = sum / n_d;
        const double var  = std::max(0.0, sumSq / n_d - mean * mean);

        Point pt;
        pt.epoch_ms = epoch_ms;
        pt.mean     = static_cast<float>(mean);
        pt.rms      = static_cast<float>(std::sqrt(var));
        pt.n        = static_cast<uint32_t>(st.fast.size());

        st.ring.push_back(pt);
        while (st.ring.size() > ring_n_) st.ring.pop_front();

        st.pending.push_back(pt);
        st.fast.clear();
    }

    // Called when a channel is removed / renamed to evict stale state.
    void forgetChannel(const std::string &name) {
        std::lock_guard<std::mutex> lk(mu_);
        channels_.erase(name);
    }

    // ── JSON output ────────────────────────────────────────────────────────
    // Wire format (compact keys):
    //   [{"n":"W123","p":[{"t":<epoch_ms>,"m":<mean>,"r":<rms>,"k":<n>}, ...]}, ...]

    // Full snapshot of every channel's ring — sent in the WS init payload.
    std::string fullSnapshotJson() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream ss;
        ss.precision(6);
        ss << '[';
        bool first_ch = true;
        for (const auto &kv : channels_) {
            if (kv.second.ring.empty()) continue;
            if (!first_ch) ss << ',';
            first_ch = false;
            ss << R"({"n":")" << kv.first << R"(","p":[)";
            bool first_p = true;
            for (const auto &pt : kv.second.ring) {
                if (!first_p) ss << ',';
                first_p = false;
                emitPoint_(ss, pt);
            }
            ss << "]}";
        }
        ss << ']';
        return ss.str();
    }

    // Drain all points produced since the last call.  Returns "[]" when
    // nothing is pending (WS broadcast skips empty deltas).
    std::string takeDeltaJson() {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream ss;
        ss.precision(6);
        ss << '[';
        bool first_ch = true;
        for (auto &kv : channels_) {
            auto &st = kv.second;
            if (st.pending.empty()) continue;
            if (!first_ch) ss << ',';
            first_ch = false;
            ss << R"({"n":")" << kv.first << R"(","p":[)";
            bool first_p = true;
            for (const auto &pt : st.pending) {
                if (!first_p) ss << ',';
                first_p = false;
                emitPoint_(ss, pt);
            }
            ss << "]}";
            st.pending.clear();
        }
        ss << ']';
        return ss.str();
    }

private:
    struct ChState {
        std::vector<float> fast;        // partial fill toward next aggregation
        std::deque<Point>  ring;        // history (cap ring_n_)
        std::vector<Point> pending;     // produced since last delta take
    };

    static void emitPoint_(std::ostringstream &ss, const Point &pt) {
        ss << R"({"t":)" << pt.epoch_ms
           << R"(,"m":)" << pt.mean
           << R"(,"r":)" << pt.rms
           << R"(,"k":)" << pt.n
           << '}';
    }

    mutable std::mutex mu_;
    size_t fast_n_;
    size_t ring_n_;
    std::unordered_map<std::string, ChState> channels_;
};
