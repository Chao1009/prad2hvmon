#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BoosterSupply – one TDK-Lambda GEN power supply accessed via SCPI/TCP
// ─────────────────────────────────────────────────────────────────────────────
//
//  Protocol:  plain-text SCPI over TCP port 8003 (TDK-Lambda default).
//  Each supply exposes exactly one channel.
//
//  Commands used:
//    OUTP:STAT?          → "ON" or "OFF"
//    OUTP:STAT 1/0      → turn output on/off
//    MEAS:VOLT?          → measured output voltage (float string)
//    MEAS:CURR?          → measured output current (float string)
//    SOUR:VOLT <val>     → set voltage
//    SOUR:CURR <val>     → set current limit
//    SOUR:MOD?           → operating mode: "CV" or "CC"
//
//  Threading:
//    BoosterSupply objects are owned by BoosterPoller on a worker thread.
//    All I/O is synchronous (blocking with short timeouts), matching the
//    polling model used by the CAEN HV layer.
// ─────────────────────────────────────────────────────────────────────────────

#include <QTcpSocket>
#include <QHostAddress>
#include <QString>
#include <limits>

struct BoosterSupply {
    QString name;   // human label, e.g. "Booster 1"
    QString ip;
    quint16 port = 8003;

    // Readback (updated by poll)
    double  vmon     = std::numeric_limits<double>::quiet_NaN();
    double  vset     = std::numeric_limits<double>::quiet_NaN();
    double  imon     = std::numeric_limits<double>::quiet_NaN();
    double  iset     = std::numeric_limits<double>::quiet_NaN();
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

    // One full poll cycle: read status, voltage, current, mode, setpoints.
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

        // Measured current — MEAS:CURR? returns actual output current in Amps
        s = sendCmd("MEAS:CURR?");
        if (s.isEmpty()) { failWith("no response (MEAS:CURR?)"); return; }
        v = s.toDouble(&ok);
        imon = ok ? v : std::numeric_limits<double>::quiet_NaN();

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

        // ISet — current limit setpoint
        s = sendCmd("SOUR:CURR:LEV:IMM:AMPL?");
        if (s.isEmpty()) { failWith("no response (SOUR:CURR:LEV:IMM:AMPL?)"); return; }
        v = s.toDouble(&ok);
        if (ok) iset = v;

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

    void setCurrent(double amps)
    {
        if (!ensureConnected()) return;
        sendSet(QString("SOUR:CURR:LEV:IMM:AMPL %1").arg(amps, 0, 'f', 3));
        iset = amps;
    }

    ~BoosterSupply() { if (sock) { sock->abort(); delete sock; } }
};
