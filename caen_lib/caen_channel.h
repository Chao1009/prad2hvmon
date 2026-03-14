#pragma once
//============================================================================//
// C++ wrapper for CAEN HV systems with generic parameter discovery           //
//                                                                            //
// Parameters are auto-discovered from hardware at init time via              //
// CAENHV_GetChParamInfo / CAENHV_GetBdParamInfo and their properties.        //
// No hard-coded param names in the data model — any board model works.       //
//                                                                            //
// Chao Peng — Argonne National Laboratory                                    //
//============================================================================//

#include "caenhvwrapper.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <cmath>

class CAEN_Channel;
class CAEN_Board;
class CAEN_Crate;

// ── Free functions ───────────────────────────────────────────────────────────
std::ostream &operator<<(std::ostream &os, CAEN_Board const &b);
void CAEN_ShowError(const std::string &prefix, const int &err, const bool &ShowSuccess = false);
float CAEN_VoltageLimit(const std::string &name);
void CAEN_ShowChError(const std::string &n, const unsigned int &bitmap);
bool CAEN_IsUnsupportedParam(int err);

// ── Parameter metadata (discovered once per board model at init) ─────────────
struct ParamInfo {
    std::string    name;          // "V0Set", "IMon", "Pw", "Status", "Trip", etc.
    unsigned       type  = 0;     // PARAM_TYPE_NUMERIC, PARAM_TYPE_ONOFF, etc.
    unsigned       mode  = 0;     // PARAM_MODE_RDONLY, PARAM_MODE_WRONLY, PARAM_MODE_RDWR
    // Numeric-only metadata:
    float          minval = 0;
    float          maxval = 0;
    unsigned short unit   = 0;    // PARAM_UN_VOLT, PARAM_UN_AMPERE, etc.
    short          exp    = 0;    // SI prefix exponent (+3=k, -3=m, -6=µ)

    bool isFloat()    const { return type == PARAM_TYPE_NUMERIC; }
    bool isUInt()     const { return type == PARAM_TYPE_ONOFF || type == PARAM_TYPE_CHSTATUS
                                  || type == PARAM_TYPE_BDSTATUS || type == PARAM_TYPE_BINARY; }
    bool isReadable() const { return mode == PARAM_MODE_RDONLY || mode == PARAM_MODE_RDWR; }
    bool isWritable() const { return mode == PARAM_MODE_WRONLY || mode == PARAM_MODE_RDWR; }
};

// ── Parameter value (runtime, one per channel/board per parameter) ───────────
struct ParamValue {
    enum Tag { Empty, Float, UInt } tag = Empty;
    float        f = NAN;
    unsigned int u = 0;

    static ParamValue fromFloat(float v)        { ParamValue p; p.tag = Float; p.f = v; return p; }
    static ParamValue fromUInt(unsigned int v)   { ParamValue p; p.tag = UInt;  p.u = v; return p; }
};


// ═════════════════════════════════════════════════════════════════════════════
//  CAEN_Channel
// ═════════════════════════════════════════════════════════════════════════════
class CAEN_Channel
{
private:
    CAEN_Board *mother;
    unsigned short channel;
    std::string name;
    float limit;                  // software voltage limit (from CAEN_VoltageLimit)

    // Generic parameter values — populated by Board::ReadAllParams()
    std::unordered_map<std::string, ParamValue> params_;

public:
    // Constructor
    CAEN_Channel(CAEN_Board *m, const unsigned short &c, const std::string &n)
        : mother(m), channel(c), name(n), limit(CAEN_VoltageLimit(n))
    {}

    virtual ~CAEN_Channel();

    // ── Generic parameter access ─────────────────────────────────────────
    float        GetFloat(const std::string &pname) const;
    unsigned int GetUInt(const std::string &pname) const;
    bool         HasParam(const std::string &pname) const;
    const std::unordered_map<std::string, ParamValue> &GetParams() const { return params_; }

    // Direct cache update (called by Board::ReadAllParams, no hardware I/O)
    void SetParamDirect(const std::string &pname, float v)        { params_[pname] = ParamValue::fromFloat(v); }
    void SetParamDirect(const std::string &pname, unsigned int v) { params_[pname] = ParamValue::fromUInt(v); }

    // Write to hardware + update cache (single-channel)
    bool SetFloat(const std::string &pname, float value);
    bool SetUInt(const std::string &pname, unsigned int value);

    // ── Convenience wrappers (inline, thin) ──────────────────────────────
    float GetVMon()  const { return GetFloat("VMon"); }
    float GetVSet()  const { return GetFloat("V0Set"); }
    float GetIMon()  const { return GetFloat("IMon"); }
    float GetISet()  const { return GetFloat("I0Set"); }
    float GetSVMax() const { return GetFloat("SVMax"); }
    bool  IsTurnedOn() const { return GetUInt("Pw") != 0; }
    unsigned int GetStatus() const { return GetUInt("Status"); }

    // Voltage set with limit enforcement
    void SetVoltage(float v);
    // Power set with primary-channel logic
    void SetPower(bool on);
    // Current set / SVMax (simple wrappers)
    void SetCurrent(float i)  { SetFloat("I0Set", i); }
    void SetSVMax(float v)    { SetFloat("SVMax", v); }

    // Channel name (read/write via CAENHV_SetChName, not a param)
    void SetName(const std::string &n);
    const std::string &GetName() const { return name; }
    const unsigned short &GetChannel() const { return channel; }
    CAEN_Board *GetMother() { return mother; }

    // ── Software voltage limit ───────────────────────────────────────────
    float GetLimit() const { return limit; }
    void  SetLimit(float l) { limit = l; }

    // Software-only status bit for VMon over configurable limit.
    // Bit 16 is unused by CAEN hardware (which uses bits 0–15).
    static constexpr unsigned int OVL_BIT = 16;

    // Check VMon vs limit and set/clear OVL in the Status param.
    // Called after all params are read each poll cycle.
    void EvaluateOVL();

    // Status string builder (reads Status param from cache)
    std::string GetStatusString() const;

    // ── Configurable voltage limits (static, shared) ─────────────────────
    struct VoltageLimitRule {
        std::string pattern;
        float       limit;
    };
    static void SetVoltageLimit(const std::string &pattern, float limit);
    static void ClearVoltageLimits();
    static const std::vector<VoltageLimitRule> &GetVoltageLimitRules();

    // ── Error ignore rules (static, shared) ────────────────────────────
    // Each rule maps a channel name pattern to a set of status abbreviations
    // (e.g. "OV", "UV", "OVL") that should be suppressed for matching channels.
    // Suppressed errors show as amber warnings (~OV) instead of red faults.
    struct ErrorIgnoreRule {
        std::string              pattern;   // channel name or wildcard ("W*", "G29")
        std::vector<std::string> errors;    // status abbrevs to suppress
    };
    static void SetErrorIgnoreRules(const std::vector<ErrorIgnoreRule> &rules);
    static const std::vector<ErrorIgnoreRule> &GetErrorIgnoreRules();
    static bool IsErrorSuppressed(const std::string &ch_name, const std::string &error_abbr);
};


// ═════════════════════════════════════════════════════════════════════════════
//  CAEN_Board
// ═════════════════════════════════════════════════════════════════════════════
class CAEN_Board
{
private:
    CAEN_Crate *mother;
    std::string model;
    std::string desc;
    unsigned short slot;
    unsigned short nChan;
    unsigned short serNum;
    unsigned char fmwLSB;
    unsigned char fmwMSB;
    int primary;
    std::vector<CAEN_Channel*> channelList;

    // Discovered parameter metadata (populated once in ReadBoardMap)
    std::vector<ParamInfo> ch_param_info_;    // channel-level params
    std::vector<ParamInfo> bd_param_info_;    // board-level params

    // Board-level parameter values (populated by ReadAllParams)
    std::unordered_map<std::string, ParamValue> bd_params_;

    // Params where bulk read fails (e.g. PRIMARY ch doesn't support SVMax).
    // Maps param name → list of channel indices that DO support it.
    // First cycle: bulk fails → probe each channel once → build list.
    // Subsequent cycles: single bulk read of only the supporting channels.
    std::unordered_map<std::string, std::vector<unsigned short>> ch_param_fallback_list_;

public:
    CAEN_Board(CAEN_Crate *mo)
        : mother(mo), slot(-1), nChan(0), serNum(0), fmwLSB(0), fmwMSB(0), primary(-1)
    {}
    CAEN_Board(CAEN_Crate *mo, std::string m, std::string d, unsigned short s, unsigned short n,
               unsigned short ser, unsigned char lsb, unsigned char msb)
        : mother(mo), model(m), desc(d), slot(s), nChan(n), serNum(ser),
          fmwLSB(lsb), fmwMSB(msb), primary(-1)
    {}
    CAEN_Board(CAEN_Crate *mo, char* m, char* d, unsigned short s, unsigned short n,
               unsigned short ser, unsigned char lsb, unsigned char msb)
        : mother(mo), model(m), desc(d), slot(s), nChan(n), serNum(ser),
          fmwLSB(lsb), fmwMSB(msb), primary(-1)
    {}

    virtual ~CAEN_Board();

    // ── Initialization (called once per board) ───────────────────────────
    void ReadBoardMap();         // discover params + initial channel read

    // ── Polling (called every cycle) ─────────────────────────────────────
    void ReadAllParams();        // read all board + channel params generically

    // ── Generic board-level param access ─────────────────────────────────
    float        GetBdFloat(const std::string &pname) const;
    unsigned int GetBdUInt(const std::string &pname) const;
    bool         HasBdParam(const std::string &pname) const;
    const std::unordered_map<std::string, ParamValue> &GetBdParams() const { return bd_params_; }

    // ── Convenience wrappers ─────────────────────────────────────────────
    float GetHVMax()  const { return GetBdFloat("HVMax"); }
    float GetTemp()   const { return GetBdFloat("Temp"); }
    unsigned int GetBdStatus() const { return GetBdUInt("BdStatus"); }
    std::string GetBdStatusString() const;

    // ── Param metadata access ────────────────────────────────────────────
    const std::vector<ParamInfo> &GetChParamInfo() const { return ch_param_info_; }
    const std::vector<ParamInfo> &GetBdParamInfo() const { return bd_param_info_; }
    bool HasChParam(const std::string &name) const;

    // ── Bulk write (kept for backward compat) ────────────────────────────
    void SetPower(const bool &on_off);
    void SetPower(const std::vector<unsigned int> &on_off);
    void SetVoltage(const std::vector<float> &Vset);

    // ── Accessors ────────────────────────────────────────────────────────
    int GetHandle();
    const unsigned short &GetSlot() { return slot; }
    CAEN_Crate *GetMother() { return mother; }
    CAEN_Channel *GetPrimaryChannel();
    CAEN_Channel *GetChannel(int i);
    std::vector<CAEN_Channel*> &GetChannelList() { return channelList; }
    const std::string &GetModel() { return model; }
    const std::string &GetDescription() { return desc; }
    const unsigned short &GetSize() { return nChan; }
    const unsigned short &GetSerialNum() { return serNum; }
    unsigned short GetFirmware();
    const int &GetPrimaryChannelNumber() { return primary; }

private:
    void DiscoverChParams();     // call GetChParamInfo + GetChParamProp
    void DiscoverBdParams();     // call GetBdParamInfo + GetBdParamProp
};


// ═════════════════════════════════════════════════════════════════════════════
//  CAEN_Crate
// ═════════════════════════════════════════════════════════════════════════════
class CAEN_Crate
{
private:
    unsigned char id;
    std::string name;
    std::string ip;
    CAENHV::CAENHV_SYSTEM_TYPE_t sys_type;
    int link_type;
    std::string username;
    std::string password;
    int handle;
    bool mapped;
    short slot_map[MAX_SLOTS];
    std::vector<CAEN_Board*> boardList;

public:
    CAEN_Crate(const unsigned char &i,
               const std::string &n,
               const std::string &p,
               const CAENHV::CAENHV_SYSTEM_TYPE_t &type,
               const int &link,
               const std::string &user,
               const std::string &pwd)
        : id(i), name(n), ip(p), sys_type(type), link_type(link),
          username(user), password(pwd), handle(-1), mapped(false)
    {}

    virtual ~CAEN_Crate();

    bool Initialize();
    bool DeInitialize();
    void ReadCrateMap();
    void PrintCrateMap();
    void HeartBeat();
    void Clear();

    // Polling: reads all board + channel params for every board
    void ReadAllParams();

    void SetPower(const bool &on_off);
    const int &GetHandle() { return handle; }
    const std::string &GetName() { return name; }
    const std::string &GetIP() { return ip; }
    std::vector<CAEN_Board*> &GetBoardList() { return boardList; }
    CAEN_Board *GetBoard(const unsigned short &slot);
};
