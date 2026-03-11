#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HVMonitor – QObject bridge between the CAEN HV back-end and the
//             QWebEngineView front-end via QWebChannel.
//
// Signals carry JSON strings so the JS side can simply JSON.parse() them.
// ─────────────────────────────────────────────────────────────────────────────

#include <QObject>
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


class HVMonitor : public QObject
{
    Q_OBJECT

public:
    explicit HVMonitor(
        const std::vector<std::pair<std::string, std::string>> &crate_list,
        const QString &module_json_path = "",
        const QString &gui_config_path = "",
        int poll_ms = 3000,
        QObject *parent = nullptr)
        : QObject(parent), poll_interval_ms_(poll_ms),
          crate_defs_(crate_list), module_json_path_(module_json_path),
          gui_config_path_(gui_config_path)
    {
        connect(&timer_, &QTimer::timeout, this, &HVMonitor::pollCrates);
    }

    ~HVMonitor() override
    {
        stopPolling();
        for (auto *c : crates_) delete c;
    }

    // ── called once from main before the event-loop starts ──────────────
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
        std::cout << fmt::format("Init DONE – {}/{} crates OK\n",
                                 ok, crates_.size());
        initialized_ = (ok == static_cast<int>(crates_.size()));
        return initialized_;
    }

public slots:

    // ── JS-callable: start / stop the periodic read-back ────────────────
    void startPolling()
    {
        if (!timer_.isActive())
            timer_.start(poll_interval_ms_);
    }

    void stopPolling()
    {
        if (timer_.isActive())
            timer_.stop();
    }

    void setPollInterval(int ms)
    {
        poll_interval_ms_ = (ms < 500) ? 500 : ms;   // floor 0.5 s
        if (timer_.isActive()) {
            timer_.stop();
            timer_.start(poll_interval_ms_);
        }
    }

    int getPollInterval() { return poll_interval_ms_; }

    // ── JS-callable: one-shot read of all channels ──────────────────────
    //    Returns a JSON string: array of channel objects.
    QString readAll()
    {
        return buildSnapshot();
    }

    // ── JS-callable: get the static crate / board topology ──────────────
    QString getTopology()
    {
        QJsonArray arr;
        for (auto *cr : crates_) {
            QJsonObject cObj;
            cObj["crate"] = QString::fromStdString(cr->GetName());
            cObj["ip"]    = QString::fromStdString(cr->GetIP());

            QJsonArray boards;
            for (auto *bd : cr->GetBoardList()) {
                QJsonObject bObj;
                bObj["slot"]     = bd->GetSlot();
                bObj["model"]    = QString::fromStdString(bd->GetModel());
                bObj["desc"]     = QString::fromStdString(bd->GetDescription());
                bObj["nChan"]    = bd->GetSize();
                bObj["serial"]   = bd->GetSerialNum();
                bObj["firmware"] = bd->GetFirmware();
                boards.append(bObj);
            }
            cObj["boards"] = boards;
            arr.append(cObj);
        }
        return QString(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    // ── JS-callable: read the module geometry JSON file ──────────────────
    QString getModuleGeometry()
    {
        if (module_json_path_.isEmpty()) return QStringLiteral("[]");
        QFile f(module_json_path_);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Cannot open module geometry file: "
                      << module_json_path_.toStdString() << "\n";
            return QStringLiteral("[]");
        }
        return QString::fromUtf8(f.readAll());
    }

    // ── JS-callable: read the GUI configuration JSON file ───────────────
    QString getGuiConfig()
    {
        if (gui_config_path_.isEmpty()) return QStringLiteral("{}");
        QFile f(gui_config_path_);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Cannot open GUI config file: "
                      << gui_config_path_.toStdString() << "\n";
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(f.readAll());
    }

    // ── JS-callable: turn a single channel ON / OFF ─────────────────────
    void setChannelPower(const QString &crateName, int slot,
                         int channel, bool on)
    {
        auto it = crate_map_.find(crateName.toStdString());
        if (it == crate_map_.end()) return;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return;
        auto *ch = bd->GetChannel(channel);
        if (ch) ch->SetPower(on);
    }

    // ── JS-callable: turn ALL channels ON or OFF ────────────────────────
    void setAllPower(bool on)
    {
        for (auto *cr : crates_) {
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    ch->SetPower(on);
                }
            }
        }
    }

    // ── JS-callable: set voltage on a single channel ────────────────────
    void setChannelVoltage(const QString &crateName, int slot,
                           int channel, float voltage)
    {
        auto it = crate_map_.find(crateName.toStdString());
        if (it == crate_map_.end()) return;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return;
        auto *ch = bd->GetChannel(channel);
        if (ch) ch->SetVoltage(voltage);
    }

    // ── JS-callable: rename a single channel ────────────────────────────
    Q_INVOKABLE
    void setChannelName(const QString &crateName, int slot,
                        int channel, const QString &newName)
    {
        auto it = crate_map_.find(crateName.toStdString());
        if (it == crate_map_.end()) return;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return;
        auto *ch = bd->GetChannel(channel);
        if (ch) ch->SetName(newName.toStdString());
    }

    // ── JS-callable: set current limit on a single channel ───────────────
    Q_INVOKABLE
    void setChannelCurrent(const QString &crateName, int slot,
                           int channel, float current)
    {
        auto it = crate_map_.find(crateName.toStdString());
        if (it == crate_map_.end()) return;
        auto *bd = it->second->GetBoard(static_cast<unsigned short>(slot));
        if (!bd) return;
        auto *ch = bd->GetChannel(channel);
        if (ch) ch->SetCurrent(current);
    }

signals:
    // Emitted every poll cycle – JS listens via qt.webChannelTransport
    void channelsUpdated(const QString &jsonData);

private slots:
    void pollCrates()
    {
        QString snap = buildSnapshot();
        emit channelsUpdated(snap);
    }

private:
    QString buildSnapshot()
    {
        QJsonArray arr;
        for (auto *cr : crates_) {
            cr->CheckStatus();
            cr->ReadVoltage();
            for (auto *bd : cr->GetBoardList()) {
                for (auto *ch : bd->GetChannelList()) {
                    QJsonObject o;
                    o["crate"]   = QString::fromStdString(cr->GetName());
                    o["ip"]      = QString::fromStdString(cr->GetIP());
                    o["slot"]    = bd->GetSlot();
                    o["model"]   = QString::fromStdString(bd->GetModel());
                    o["channel"] = ch->GetChannel();
                    o["name"]    = QString::fromStdString(ch->GetName());
                    o["vmon"]    = ch->GetVMon();
                    o["vset"]    = ch->GetVSet();
                    o["iSupported"] = ch->SupportsCurrentIO();
                    if (ch->SupportsCurrentIO()) {
                        o["imon"] = std::isnan(ch->GetIMon()) ? QJsonValue::Null : QJsonValue(ch->GetIMon());
                        o["iset"] = std::isnan(ch->GetISet()) ? QJsonValue::Null : QJsonValue(ch->GetISet());
                    }
                    o["on"]      = ch->IsTurnedOn();
		    o["status"]  = QString::fromStdString(ch->GetStatusString());
                    arr.append(o);
                }
            }
        }
        return QString(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }

    QTimer timer_;
    int poll_interval_ms_;
    bool initialized_ = false;
    std::vector<std::pair<std::string, std::string>> crate_defs_;
    QString module_json_path_;
    QString gui_config_path_;
    std::vector<CAEN_Crate*> crates_;
    std::map<std::string, CAEN_Crate*> crate_map_;
};
