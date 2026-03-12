#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HVMonitor – QObject bridge between the CAEN HV back-end and the
//             QWebEngineView front-end via QWebChannel.
//
// Threading model
// ───────────────
//   HVPoller   lives on a dedicated QThread (worker).
//              Owns all CAEN_Crate objects.
//              Its QTimer fires there → CheckStatus / ReadVoltage never block
//              the GUI thread.
//              Exposes slots for write commands so they are queued onto the
//              worker thread automatically via QMetaObject::invokeMethod.
//
//   HVMonitor  lives on the GUI thread.
//              Registered with QWebChannel → all JS calls land here.
//              Holds only a cached JSON snapshot (QString) updated by the
//              poller via a queued signal.  No mutexes needed – Qt's event
//              loop serialises the handoff.
//
// Lifecycle (in main):
//   HVPoller *poller = new HVPoller(crate_list);
//   poller->initCrates();              // blocking, runs before thread starts
//   HVMonitor monitor(poller, ...);
//   QThread workerThread;
//   poller->moveToThread(&workerThread);
//   // Destroy poller on the worker thread during its teardown:
//   QObject::connect(&workerThread, &QThread::finished,
//                    poller, &QObject::deleteLater);
//   workerThread.start();
//   QMetaObject::invokeMethod(poller, "startPolling", Qt::QueuedConnection);
//   ...app.exec()...
//   workerThread.quit(); workerThread.wait();  // poller deleted here
// ─────────────────────────────────────────────────────────────────────────────

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QString>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <caen_channel.h>
#include <fmt/format.h>
#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <cmath>


// ═════════════════════════════════════════════════════════════════════════════
//  HVPoller – lives on the worker thread, owns all hardware objects
// ═════════════════════════════════════════════════════════════════════════════
class HVPoller : public QObject
{
    Q_OBJECT

public:
    explicit HVPoller(
        const std::vector<std::pair<std::string, std::string>> &crate_list,
        QObject *parent = nullptr)
        : QObject(parent), poll_interval_ms_(3000), crate_defs_(crate_list)
    {
        // NOTE: timer_ is intentionally NOT constructed here.
        // HVPoller is constructed on the main thread but will be moved to the
        // worker thread before polling starts.  QTimer must be constructed on
        // the thread that will run it, so we create it lazily in startPolling()
        // which is always invoked via a queued call after moveToThread().
    }

    ~HVPoller() override
    {
        // Runs on the worker thread (via deleteLater from QThread::finished),
        // so stopping the timer and deleting CAEN objects is safe here.
        if (timer_) {
            timer_->stop();
            delete timer_;
        }
        for (auto *c : crates_) delete c;
    }

    // Call this BEFORE moveToThread – runs on the calling (main) thread,
    // which is fine because the worker thread hasn't started yet.
    bool initCrates()
    {
        int crid = 0;
        for (const auto &[name, ip] : crate_defs_) {
            auto *cr = new CAEN_Crate(crid++, name, ip,
                                      CAENHV::SY1527, LINKTYPE_TCPIP,
                                      "admin", "admin");
            crates_.push_back(cr);
            crate_map_[name] = cr;
        }
        int ok = 0;
        for (auto *cr : crates_) {
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
        std::cout << fmt::format("Init DONE - {}/{} crates OK\n",
                                 ok, crates_.size());
        return (ok == static_cast<int>(crates_.size()));
    }

public slots:
    // ── Timer control ────────────────────────────────────────────────────
    // All of these are always invoked via QueuedConnection from the GUI
    // thread, so they execute on the worker thread's event loop.

    void startPolling()
    {
        if (!timer_) {
            // Constructed here on the worker thread — correct thread affinity.
            timer_ = new QTimer(this);
            timer_->setTimerType(Qt::CoarseTimer);
            connect(timer_, &QTimer::timeout, this, &HVPoller::doPoll);
        }
        if (!timer_->isActive())
            timer_->start(poll_interval_ms_);
    }

    void stopPolling()
    {
        if (timer_) timer_->stop();
    }

    void setPollInterval(int ms)
    {
        poll_interval_ms_ = (ms < 500) ? 500 : ms;
        if (timer_ && timer_->isActive()) {
            timer_->stop();
            timer_->start(poll_interval_ms_);
        }
    }

    // ── Write commands (queued from GUI thread) ──────────────────────────
    void setChannelPower(const QString &crateName, int slot,
                         int channel, bool on)
    {
        auto *ch = findChannel(crateName, slot, channel);
        if (ch) ch->SetPower(on);
    }

    void setAllPower(bool on)
    {
        for (auto *cr : crates_)
            for (auto *bd : cr->GetBoardList())
                for (auto *ch : bd->GetChannelList())
                    ch->SetPower(on);
    }

    void setChannelVoltage(const QString &crateName, int slot,
                           int channel, float voltage)
    {
        auto *ch = findChannel(crateName, slot, channel);
        if (ch) ch->SetVoltage(voltage);
    }

    void setChannelName(const QString &crateName, int slot,
                        int channel, const QString &newName)
    {
        auto *ch = findChannel(crateName, slot, channel);
        if (ch) ch->SetName(newName.toStdString());
    }

    void setChannelCurrent(const QString &crateName, int slot,
                           int channel, float current)
    {
        auto *ch = findChannel(crateName, slot, channel);
        if (ch) ch->SetCurrent(current);
    }

signals:
    // Emitted on the worker thread; Qt queues delivery to the GUI thread.
    void snapshotReady(const QString &jsonData);

private slots:
    void doPoll()
    {
        for (auto *cr : crates_) {
            cr->CheckStatus();
            cr->ReadVoltage();
        }
        emit snapshotReady(buildSnapshot());
    }

private:
    CAEN_Channel *findChannel(const QString &crateName, int slot, int channel)
    {
        auto it = crate_map_.find(crateName.toStdString());
        if (it == crate_map_.end()) return nullptr;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return nullptr;
        return bd->GetChannel(channel);
    }

    QString buildSnapshot()
    {
        QJsonArray arr;
        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    QJsonObject o;
                    o["crate"]      = QString::fromStdString(cr->GetName());
                    o["ip"]         = QString::fromStdString(cr->GetIP());
                    o["slot"]       = bd->GetSlot();
                    o["model"]      = QString::fromStdString(bd->GetModel());
                    o["channel"]    = ch->GetChannel();
                    o["name"]       = QString::fromStdString(ch->GetName());
                    o["vmon"]       = ch->GetVMon();
                    o["vset"]       = ch->GetVSet();
                    o["iSupported"] = ch->SupportsCurrentIO();
                    if (ch->SupportsCurrentIO()) {
                        o["imon"] = std::isnan(ch->GetIMon()) ? QJsonValue::Null : QJsonValue(ch->GetIMon());
                        o["iset"] = std::isnan(ch->GetISet()) ? QJsonValue::Null : QJsonValue(ch->GetISet());
                    }
                    o["on"]     = ch->IsTurnedOn();
                    o["status"] = QString::fromStdString(ch->GetStatusString());
                    arr.append(o);
                }
            }
        }
        return QString(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QTimer *timer_ = nullptr;   // created in startPolling() on the worker thread
    int     poll_interval_ms_;
    std::vector<std::pair<std::string, std::string>> crate_defs_;
    std::vector<CAEN_Crate *> crates_;
    std::map<std::string, CAEN_Crate *> crate_map_;
};


// ═════════════════════════════════════════════════════════════════════════════
//  HVMonitor – GUI-thread QWebChannel bridge
// ═════════════════════════════════════════════════════════════════════════════
class HVMonitor : public QObject
{
    Q_OBJECT

public:
    explicit HVMonitor(
        HVPoller        *poller,
        const QString   &module_json_path = "",
        const QString   &gui_config_path  = "",
        const QString   &daq_map_path     = "",
        QObject         *parent = nullptr)
        : QObject(parent), poller_(poller),
          module_json_path_(module_json_path),
          gui_config_path_(gui_config_path),
          daq_map_path_(daq_map_path)
    {
        // Queued connection: snapshot string delivered from worker to GUI thread.
        connect(poller_, &HVPoller::snapshotReady,
                this,    &HVMonitor::onSnapshotReady,
                Qt::QueuedConnection);
    }

public slots:

    // ── Polling control (forwarded to worker via queued invocation) ──────
    void startPolling()
    {
        QMetaObject::invokeMethod(poller_, "startPolling", Qt::QueuedConnection);
    }

    void stopPolling()
    {
        QMetaObject::invokeMethod(poller_, "stopPolling", Qt::QueuedConnection);
    }

    void setPollInterval(int ms)
    {
        QMetaObject::invokeMethod(poller_, "setPollInterval",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, ms));
    }

    int getPollInterval() { return 0; }   // not used by JS currently

    // ── JS-callable: return last cached snapshot (instant, no I/O) ──────
    // Returns "[]" rather than "" before the first poll snapshot arrives,
    // so JSON.parse() in the JS bootstrap never sees an empty string.
    QString readAll()
    {
        return cachedSnapshot_.isEmpty() ? QStringLiteral("[]") : cachedSnapshot_;
    }

    // ── JS-callable: static file reads (GUI thread, no hardware) ────────
    QString getModuleGeometry() { return readJsonFile(module_json_path_, "[]"); }
    QString getGuiConfig()      { return readJsonFile(gui_config_path_,  "{}"); }
    QString getDAQMap()         { return readJsonFile(daq_map_path_,     "[]"); }

    // ── JS-callable: write commands (forwarded to worker thread) ────────
    void setChannelPower(const QString &crateName, int slot,
                         int channel, bool on)
    {
        QMetaObject::invokeMethod(poller_, "setChannelPower",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, crateName),
                                  Q_ARG(int,     slot),
                                  Q_ARG(int,     channel),
                                  Q_ARG(bool,    on));
    }

    void setAllPower(bool on)
    {
        QMetaObject::invokeMethod(poller_, "setAllPower",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, on));
    }

    void setChannelVoltage(const QString &crateName, int slot,
                           int channel, float voltage)
    {
        QMetaObject::invokeMethod(poller_, "setChannelVoltage",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, crateName),
                                  Q_ARG(int,     slot),
                                  Q_ARG(int,     channel),
                                  Q_ARG(float,   voltage));
    }

    Q_INVOKABLE
    void setChannelName(const QString &crateName, int slot,
                        int channel, const QString &newName)
    {
        QMetaObject::invokeMethod(poller_, "setChannelName",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, crateName),
                                  Q_ARG(int,     slot),
                                  Q_ARG(int,     channel),
                                  Q_ARG(QString, newName));
    }

    Q_INVOKABLE
    void setChannelCurrent(const QString &crateName, int slot,
                           int channel, float current)
    {
        QMetaObject::invokeMethod(poller_, "setChannelCurrent",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, crateName),
                                  Q_ARG(int,     slot),
                                  Q_ARG(int,     channel),
                                  Q_ARG(float,   current));
    }

signals:
    // Forwarded to JS via QWebChannel.
    void channelsUpdated(const QString &jsonData);

private slots:
    // Receives snapshot string from worker thread (queued → GUI thread).
    void onSnapshotReady(const QString &json)
    {
        cachedSnapshot_ = json;
        emit channelsUpdated(json);
    }

private:
    QString readJsonFile(const QString &path, const char *fallback)
    {
        if (path.isEmpty()) return QLatin1String(fallback);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Cannot open file: " << path.toStdString() << "\n";
            return QLatin1String(fallback);
        }
        return QString::fromUtf8(f.readAll());
    }

    HVPoller *poller_;
    QString   cachedSnapshot_;
    QString   module_json_path_;
    QString   gui_config_path_;
    QString   daq_map_path_;
};


// ═════════════════════════════════════════════════════════════════════════════
//  BoosterSupply – one TDK-Lambda GEN power supply accessed via SCPI/TCP
// ═════════════════════════════════════════════════════════════════════════════
//
//  Protocol:  plain-text SCPI over TCP port 8003 (TDK-Lambda default).
//  Each supply exposes exactly one channel.
//
//  Commands used:
//    OUTP:STAT?          → "ON" or "OFF"
//    OUTP:STAT 1/0      → turn output on/off
//    MEAS:VOLT?          → measured output voltage (float string)
//    SOUR:VOLT <val>     → set voltage
//    SOUR:MOD?           → operating mode: "CV" or "CC"
//
//  Threading model mirrors HVPoller:
//    BoosterPoller lives on a dedicated QThread, owns QTcpSocket objects.
//    BoosterMonitor lives on the GUI thread and is registered with QWebChannel.

#include <QTcpSocket>
#include <QHostAddress>

// ─────────────────────────────────────────────────────────────────────────────
//  BoosterSupply – owns one QTcpSocket, manages the blocking SCPI dialogue
// ─────────────────────────────────────────────────────────────────────────────
struct BoosterSupply {
    QString name;   // human label, e.g. "Booster 1"
    QString ip;
    quint16 port = 8003;

    // Readback (updated by poll)
    double  vmon     = std::numeric_limits<double>::quiet_NaN();
    double  vset     = std::numeric_limits<double>::quiet_NaN();
    bool    on       = false;
    QString mode;       // "CV", "CC", or ""
    QString error;      // last error string, empty if OK
    bool    connected = false;

    // The socket lives on the BoosterPoller's thread (created in poll()).
    // We use synchronous (blocking) I/O with a short timeout so no event-loop
    // juggling is needed inside the poll slot.
    QTcpSocket *sock = nullptr;

    // Send a command; return trimmed response, or "" on error.
    //
    // Two fixes vs the naive implementation:
    //
    // 1. Drain stale bytes before sending.  When a TCP response arrives in
    //    two segments (e.g. "OFF" then "\r\n"), readAll() on the first
    //    waitForReadyRead() call returns "OFF" correctly, but the trailing
    //    "\r\n" sits in the kernel buffer.  The *next* sendCmd then hits
    //    waitForReadyRead() immediately (bytes already available) and
    //    readAll() returns "\r\n", which trims to "" — triggering a false
    //    "no response" failure.  Draining at the top of each call flushes
    //    any such leftovers before writing the next command.
    //
    // 2. Loop until '\n'.  Responses can be split across multiple TCP
    //    segments.  We accumulate until a newline is present so we always
    //    return a complete line regardless of packetisation.
    QString sendCmd(const QString &cmd, int timeoutMs = 2000)
    {
        if (!sock) return {};

        // Drain any stale bytes left from a previous partial read.
        if (sock->bytesAvailable() > 0)
            sock->readAll();

        const QByteArray tx = (cmd + "\n").toUtf8();
        sock->write(tx);
        if (!sock->waitForBytesWritten(timeoutMs)) {
            sock->abort();          // mark socket as unusable; reconnect next poll
            connected = false;
            return {};
        }

        // Accumulate until we have a complete line (contains '\n').
        QByteArray resp;
        while (!resp.contains('\n')) {
            if (!sock->waitForReadyRead(timeoutMs)) {
                sock->abort();
                connected = false;
                return {};
            }
            resp += sock->readAll();
        }
        return QString::fromUtf8(resp).trimmed();
    }

    // Send a set command — TDK-Lambda write commands produce no response.
    // Does NOT call waitForReadyRead; just writes and returns.
    void sendSet(const QString &cmd, int timeoutMs = 2000)
    {
        if (!sock) return;
        if (sock->bytesAvailable() > 0)
            sock->readAll();
        const QByteArray tx = (cmd + "\n").toUtf8();
        sock->write(tx);
        if (!sock->waitForBytesWritten(timeoutMs)) {
            sock->abort();
            connected = false;
        }
    }

    // Ensure socket is connected; reconnect if needed.
    //
    // ConnectedState alone is not sufficient: a TCP connection can be
    // half-closed by the remote end without Qt changing the socket state
    // until the next I/O attempt.  We therefore also reconnect whenever
    // the supply was previously marked disconnected (connected == false),
    // which sendCmd sets on any I/O failure.
    bool ensureConnected(int timeoutMs = 3000)
    {
        if (!sock) sock = new QTcpSocket();
        // Reconnect if the socket is not in ConnectedState, OR if a previous
        // sendCmd failure set connected=false (handles half-closed sockets).
        if (sock->state() == QAbstractSocket::ConnectedState && connected)
            return true;
        sock->abort();
        sock->connectToHost(ip, port);
        if (!sock->waitForConnected(timeoutMs)) {
            error     = sock->errorString();
            connected = false;
            return false;
        }
        connected = true;
        error.clear();
        return true;
    }

    // One full poll cycle: read status, voltage, mode.
    void poll()
    {
        if (!ensureConnected()) return;

        auto failWith = [this](const char *msg) {
            connected = false;
            error     = QString::fromUtf8(msg);
        };

        // Output state
        QString s = sendCmd("OUTP:STAT?");
        if (s.isEmpty()) { failWith("no response (OUTP:STAT?)"); return; }
        on = (s.compare("ON", Qt::CaseInsensitive) == 0);

        // Measured voltage — MEAS:VOLT? returns the actual output voltage;
        // SOUR:VOLT? returns the programmed setpoint (same value even when OFF).
        s = sendCmd("MEAS:VOLT?");
        if (s.isEmpty()) { failWith("no response (MEAS:VOLT?)"); return; }
        bool ok;
        double v = s.toDouble(&ok);
        vmon = ok ? v : std::numeric_limits<double>::quiet_NaN();

        // Operating mode
        s = sendCmd("SOUR:MOD?");
        if (s.isEmpty()) { failWith("no response (SOUR:MOD?)"); return; }
        mode = s;

        // VSet (keep a local mirror; also refresh on startup/reconnect)
        // TDK-Lambda GEN uses SOUR:VOLT:LEV:IMM:AMPL? for the voltage setpoint.
        s = sendCmd("SOUR:VOLT:LEV:IMM:AMPL?");
        if (s.isEmpty()) { failWith("no response (SOUR:VOLT:LEV:IMM:AMPL?)"); return; }
        v = s.toDouble(&ok);
        if (ok) vset = v;

        error.clear();
    }

    void setOutput(bool enable)
    {
        if (!ensureConnected()) return;
        sendSet(enable ? "OUTP:STAT 1" : "OUTP:STAT 0");
        on = enable;
    }

    void setVoltage(double volts)
    {
        if (!ensureConnected()) return;
        sendSet(QString("SOUR:VOLT:LEV:IMM:AMPL %1").arg(volts, 0, 'f', 2));
        vset = volts;
    }

    ~BoosterSupply() { if (sock) { sock->abort(); delete sock; } }
};


// ═════════════════════════════════════════════════════════════════════════════
//  BoosterPoller – worker-thread owner of all BoosterSupply objects
// ═════════════════════════════════════════════════════════════════════════════
class BoosterPoller : public QObject
{
    Q_OBJECT

public:
    // supplies_def: list of { name, ip [, port] }
    explicit BoosterPoller(
        const std::vector<std::tuple<QString,QString,quint16>> &supplies_def,
        QObject *parent = nullptr)
        : QObject(parent), poll_interval_ms_(3000)
    {
        for (const auto &[name, ip, port] : supplies_def) {
            auto *s  = new BoosterSupply();
            s->name  = name;
            s->ip    = ip;
            s->port  = port;
            supplies_.push_back(s);
        }
    }

    ~BoosterPoller() override
    {
        if (timer_) { timer_->stop(); delete timer_; }
        for (auto *s : supplies_) delete s;
    }

public slots:
    void startPolling()
    {
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setTimerType(Qt::CoarseTimer);
            connect(timer_, &QTimer::timeout, this, &BoosterPoller::doPoll);
        }
        if (!timer_->isActive())
            timer_->start(poll_interval_ms_);
    }

    void stopPolling()  { if (timer_) timer_->stop(); }

    void setPollInterval(int ms)
    {
        poll_interval_ms_ = (ms < 500) ? 500 : ms;
        if (timer_ && timer_->isActive()) { timer_->stop(); timer_->start(poll_interval_ms_); }
    }

    void setOutput(int idx, bool on)
    {
        if (idx >= 0 && idx < static_cast<int>(supplies_.size()))
            supplies_[idx]->setOutput(on);
    }

    void setVoltage(int idx, double volts)
    {
        if (idx >= 0 && idx < static_cast<int>(supplies_.size()))
            supplies_[idx]->setVoltage(volts);
    }

signals:
    void snapshotReady(const QString &jsonData);

private slots:
    void doPoll()
    {
        for (auto *s : supplies_) s->poll();
        emit snapshotReady(buildSnapshot());
    }

private:
    QString buildSnapshot()
    {
        QJsonArray arr;
        for (int i = 0; i < static_cast<int>(supplies_.size()); ++i) {
            const auto *s = supplies_[i];
            QJsonObject o;
            o["idx"]       = i;
            o["name"]      = s->name;
            o["ip"]        = s->ip;
            o["connected"] = s->connected;
            o["on"]        = s->on;
            o["mode"]      = s->mode;
            o["error"]     = s->error;
            o["vmon"]      = std::isnan(s->vmon) ? QJsonValue::Null : QJsonValue(s->vmon);
            o["vset"]      = std::isnan(s->vset) ? QJsonValue::Null : QJsonValue(s->vset);
            arr.append(o);
        }
        return QString(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QTimer *timer_ = nullptr;
    int     poll_interval_ms_;
    std::vector<BoosterSupply*> supplies_;
};


// ═════════════════════════════════════════════════════════════════════════════
//  BoosterMonitor – GUI-thread QWebChannel bridge
// ═════════════════════════════════════════════════════════════════════════════
class BoosterMonitor : public QObject
{
    Q_OBJECT

public:
    explicit BoosterMonitor(BoosterPoller *poller, QObject *parent = nullptr)
        : QObject(parent), poller_(poller)
    {
        connect(poller_, &BoosterPoller::snapshotReady,
                this,    &BoosterMonitor::onSnapshotReady,
                Qt::QueuedConnection);
    }

public slots:
    QString readAll() { return cache_.isEmpty() ? QStringLiteral("[]") : cache_; }

    void setOutput(int idx, bool on)
    {
        QMetaObject::invokeMethod(poller_, "setOutput",
                                  Qt::QueuedConnection,
                                  Q_ARG(int,  idx),
                                  Q_ARG(bool, on));
    }

    void setVoltage(int idx, double volts)
    {
        QMetaObject::invokeMethod(poller_, "setVoltage",
                                  Qt::QueuedConnection,
                                  Q_ARG(int,    idx),
                                  Q_ARG(double, volts));
    }

    void setPollInterval(int ms)
    {
        QMetaObject::invokeMethod(poller_, "setPollInterval",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, ms));
    }

signals:
    void boosterUpdated(const QString &jsonData);

private slots:
    void onSnapshotReady(const QString &json)
    {
        cache_ = json;
        emit boosterUpdated(json);
    }

private:
    BoosterPoller *poller_;
    QString        cache_;
};
