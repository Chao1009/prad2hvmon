#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BoosterPoller / BoosterMonitor – QObject bridge between the TDK-Lambda GEN
// booster supplies and the QWebEngineView front-end via QWebChannel.
//
// Threading model (mirrors HVPoller / HVMonitor):
//
//   BoosterPoller   lives on a dedicated QThread (worker).
//                   Owns all BoosterSupply objects.
//                   Its QTimer fires there → poll() never blocks the GUI thread.
//                   Exposes slots for write commands so they are queued onto the
//                   worker thread automatically via QMetaObject::invokeMethod.
//
//   BoosterMonitor  lives on the GUI thread.
//                   Registered with QWebChannel → all JS calls land here.
//                   Holds only a cached JSON snapshot (QString) updated by the
//                   poller via a queued signal.
//
// Lifecycle (in main):
//   BoosterPoller *bPoller = new BoosterPoller(supplies_def);
//   BoosterMonitor bMonitor(bPoller);
//   QThread boosterThread;
//   bPoller->moveToThread(&boosterThread);
//   QObject::connect(&boosterThread, &QThread::finished,
//                    bPoller, &QObject::deleteLater);
//   boosterThread.start();
//   QMetaObject::invokeMethod(bPoller, "startPolling", Qt::QueuedConnection);
//   ...app.exec()...
//   boosterThread.quit(); boosterThread.wait();
// ─────────────────────────────────────────────────────────────────────────────

#include <QObject>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <vector>
#include <tuple>
#include <cmath>

#include "booster_supply.h"
#include "fault_tracker.h"


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

    // Call BEFORE moveToThread — sets up fault logging.
    void setFaultLogger(FaultLogger *logger) { fault_tracker_.setLogger(logger); }

public slots:
    void startPolling()
    {
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setTimerType(Qt::CoarseTimer);
            connect(timer_, &QTimer::timeout, this, &BoosterPoller::doPoll);
        }
        if (!timer_->isActive()) {
            timer_->start(poll_interval_ms_);
            doPoll();  // immediate first poll; inside isActive guard so re-calling startPolling() is safe
        }
    }

    void stopPolling()  { if (timer_) timer_->stop(); }

    // Connect all supplies (called from GUI via BoosterMonitor bridge).
    // Starts polling which implicitly opens TCP connections on first poll.
    void connectAll()
    {
        startPolling();
    }

    // Disconnect all supplies: stop polling and close every TCP socket
    // so that other monitor instances can take over the connections.
    void disconnectAll()
    {
        stopPolling();
        for (auto *s : supplies_) {
            if (s->sock) { s->sock->abort(); }
            s->connected = false;
            s->error.clear();
            s->vmon = std::numeric_limits<double>::quiet_NaN();
            s->imon = std::numeric_limits<double>::quiet_NaN();
            s->on   = false;
            s->mode.clear();
        }
        // Log DISAPPEAR for any active faults since polling has stopped
        for (auto *s : supplies_)
            fault_tracker_.update(s->name.toStdString(), "", "booster");
        fault_tracker_.endCycle();
        // Emit one final snapshot so the UI sees "disconnected" state
        emit snapshotReady(buildSnapshot());
    }

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

    void setCurrent(int idx, double amps)
    {
        if (idx >= 0 && idx < static_cast<int>(supplies_.size()))
            supplies_[idx]->setCurrent(amps);
    }

signals:
    void snapshotReady(const QString &jsonData);

private slots:
    void doPoll()
    {
        for (auto *s : supplies_) s->poll();
        // Track fault transitions — a booster "fault" is any non-empty error
        for (auto *s : supplies_) {
            std::string status = s->connected ? "" : s->error.toStdString();
            fault_tracker_.update(s->name.toStdString(), status, "booster");
        }
        fault_tracker_.endCycle();
        emit snapshotReady(buildSnapshot());
    }

    // Build a snapshot of current supply state.  Called by BoosterMonitor's
    // constructor (before moveToThread) to pre-populate the cache so that
    // readAll() returns supply definitions to the frontend immediately.
public:
    QString initialSnapshot() { return buildSnapshot(); }

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
            o["imon"]      = std::isnan(s->imon) ? QJsonValue::Null : QJsonValue(s->imon);
            o["iset"]      = std::isnan(s->iset) ? QJsonValue::Null : QJsonValue(s->iset);
            arr.append(o);
        }
        return QString(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QTimer *timer_ = nullptr;
    int     poll_interval_ms_;
    std::vector<BoosterSupply*> supplies_;
    FaultTracker fault_tracker_;
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
        // Pre-populate cache with supply definitions before the poller
        // moves to its worker thread.  This lets readAll() return card
        // data to the frontend immediately at init time, so static cards
        // (name, ip) are built before any connection attempt.
        cache_ = poller_->initialSnapshot();

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

    void setCurrent(int idx, double amps)
    {
        QMetaObject::invokeMethod(poller_, "setCurrent",
                                  Qt::QueuedConnection,
                                  Q_ARG(int,    idx),
                                  Q_ARG(double, amps));
    }

    void setPollInterval(int ms)
    {
        QMetaObject::invokeMethod(poller_, "setPollInterval",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, ms));
    }

    void connectAll()
    {
        QMetaObject::invokeMethod(poller_, "connectAll",
                                  Qt::QueuedConnection);
    }

    void disconnectAll()
    {
        QMetaObject::invokeMethod(poller_, "disconnectAll",
                                  Qt::QueuedConnection);
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
