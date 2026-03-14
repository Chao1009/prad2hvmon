//============================================================================//
// C++ wrapper for CAEN HV systems with generic parameter discovery           //
//                                                                            //
// Chao Peng — Argonne National Laboratory                                    //
// 05/17/2016 — original                                                      //
// 03/2026   — refactored to generic parameter discovery                      //
//============================================================================//

#include "caen_channel.h"
#include <cstring>
#include <algorithm>

using namespace std;
using namespace CAENHV;

// ── Helpers ──────────────────────────────────────────────────────────────────

bool CAEN_IsUnsupportedParam(int err)
{
    return (err == 0xe || err == 0x16 || err == 0x1b || err == -5);
}

//============================================================================//
// CAEN_Channel
//============================================================================//

CAEN_Channel::~CAEN_Channel() {}

// ── Generic getters ──────────────────────────────────────────────────────────

float CAEN_Channel::GetFloat(const string &pname) const
{
    auto it = params_.find(pname);
    if (it == params_.end() || it->second.tag != ParamValue::Float) return NAN;
    return it->second.f;
}

unsigned int CAEN_Channel::GetUInt(const string &pname) const
{
    auto it = params_.find(pname);
    if (it == params_.end() || it->second.tag != ParamValue::UInt) return 0;
    return it->second.u;
}

bool CAEN_Channel::HasParam(const string &pname) const
{
    return params_.count(pname) > 0;
}

// ── Generic setters (write to hardware) ──────────────────────────────────────

bool CAEN_Channel::SetFloat(const string &pname, float value)
{
    float val = value;
    int handle = mother->GetHandle();
    unsigned short slot = mother->GetSlot();
    int err = CAENHV_SetChParam(handle, slot, pname.c_str(), 1, &channel, &val);

    if (CAEN_IsUnsupportedParam(err)) return false;
    CAEN_ShowError("HV Channel Set " + pname, err);

    if (err == CAENHV_OK) {
        params_[pname] = ParamValue::fromFloat(val);
        return true;
    }
    return false;
}

bool CAEN_Channel::SetUInt(const string &pname, unsigned int value)
{
    unsigned int val = value;
    int handle = mother->GetHandle();
    unsigned short slot = mother->GetSlot();
    int err = CAENHV_SetChParam(handle, slot, pname.c_str(), 1, &channel, &val);

    if (CAEN_IsUnsupportedParam(err)) return false;
    CAEN_ShowError("HV Channel Set " + pname, err);

    if (err == CAENHV_OK) {
        params_[pname] = ParamValue::fromUInt(val);
        return true;
    }
    return false;
}

// ── SetVoltage (with limit enforcement) ──────────────────────────────────────

void CAEN_Channel::SetVoltage(float v)
{
    float val = v;
    if (v > limit) {
        cerr << "HV Channel ERROR: Trying to set voltage " << v
             << " V, which exceeds limit " << limit
             << " V for channel " << name
             << ". Clamping to limit." << endl;
        val = limit;
    }
    SetFloat("V0Set", val);
}

// ── SetPower (with primary-channel logic) ────────────────────────────────────

void CAEN_Channel::SetPower(bool on)
{
    int err;
    unsigned int val = on ? 1 : 0;
    int handle = mother->GetHandle();
    unsigned short slot = mother->GetSlot();

    if (on && mother->GetPrimaryChannelNumber() >= 0) {
        unsigned short list[2];
        list[0] = mother->GetPrimaryChannelNumber();
        list[1] = channel;
        unsigned int vallist[2] = {val, val};
        err = CAENHV_SetChParam(handle, slot, "Pw", 2, list, vallist);
    } else {
        err = CAENHV_SetChParam(handle, slot, "Pw", 1, &channel, &val);
    }

    CAEN_ShowError("HV Channel Set Power", err);
    if (err == CAENHV_OK)
        params_["Pw"] = ParamValue::fromUInt(val);
}

// ── SetName ──────────────────────────────────────────────────────────────────

void CAEN_Channel::SetName(const string &n)
{
    int handle = mother->GetHandle();
    unsigned short slot = mother->GetSlot();
    int err = CAENHV_SetChName(handle, slot, 1, &channel, n.c_str());
    CAEN_ShowError("HV Channel Set Name", err);
    if (err == CAENHV_OK) name = n;
}

// ── OVL evaluation (called after all params read) ────────────────────────────

void CAEN_Channel::EvaluateOVL()
{
    unsigned int st = GetUInt("Status");
    bool on = IsTurnedOn();
    float vmon = GetVMon();

    if (on && !std::isnan(vmon) && vmon > limit) {
        if (!(st & (1u << OVL_BIT)))
            cerr << "Channel " << name << " VMon " << vmon
                 << " V exceeds software limit " << limit << " V!" << endl;
        st |= (1u << OVL_BIT);
    } else {
        st &= ~(1u << OVL_BIT);
    }
    // Write back to cache (not to hardware)
    params_["Status"] = ParamValue::fromUInt(st);

    // Also check for hardware errors and print them
    unsigned int hw_status = st & 0xFFFF;  // lower 16 bits = hardware
    CAEN_ShowChError(name, hw_status);
}

// ── GetStatusString ──────────────────────────────────────────────────────────

string CAEN_Channel::GetStatusString() const
{
    unsigned int st = GetStatus();
    if (st == 0) return "OFF|channel is off";

    static const struct { int bit; const char *abbr; const char *full; } flags[] = {
        {  0, "ON",   "channel is on"          },
        {  1, "RUP",  "ramping up"             },
        {  2, "RDN",  "ramping down"           },
        {  3, "OC",   "overcurrent"            },
        {  4, "OV",   "overvoltage"            },
        {  5, "UV",   "undervoltage"           },
        {  6, "EXT",  "external trip"          },
        {  7, "MAXV", "max voltage"            },
        {  8, "DIS",  "external disable"       },
        {  9, "ITRP", "internal trip"          },
        { 10, "CAL",  "calibration error"      },
        { 11, "UNPLG","unplugged"              },
        { 13, "OVP",  "overvoltage protection" },
        { 14, "PWRF", "power fail"             },
        { 15, "TEMP", "temperature error"      },
        { static_cast<int>(OVL_BIT), "OVL", "VMon over software limit" },
    };

    string abbrs, detail;
    for (const auto &f : flags) {
        if (st & (1u << f.bit)) {
            if (!abbrs.empty())  abbrs  += ' ';
            if (!detail.empty()) detail += ", ";
            abbrs  += f.abbr;
            detail += f.full;
        }
    }
    return abbrs + '|' + detail;
}


//============================================================================//
// CAEN_Board
//============================================================================//

CAEN_Board::~CAEN_Board()
{
    for (auto *ch : channelList) delete ch;
}

int CAEN_Board::GetHandle() { return mother->GetHandle(); }

CAEN_Channel *CAEN_Board::GetPrimaryChannel() { return GetChannel(primary); }

CAEN_Channel *CAEN_Board::GetChannel(int i)
{
    if (static_cast<unsigned>(i) >= channelList.size()) return nullptr;
    return channelList[i];
}

bool CAEN_Board::HasChParam(const string &name) const
{
    for (const auto &pi : ch_param_info_)
        if (pi.name == name) return true;
    return false;
}

unsigned short CAEN_Board::GetFirmware()
{
    return (static_cast<unsigned short>(fmwLSB) << 8) | fmwMSB;
}

// ── Board-level generic getters ──────────────────────────────────────────────

float CAEN_Board::GetBdFloat(const string &pname) const
{
    auto it = bd_params_.find(pname);
    if (it == bd_params_.end() || it->second.tag != ParamValue::Float) return NAN;
    return it->second.f;
}

unsigned int CAEN_Board::GetBdUInt(const string &pname) const
{
    auto it = bd_params_.find(pname);
    if (it == bd_params_.end() || it->second.tag != ParamValue::UInt) return 0;
    return it->second.u;
}

bool CAEN_Board::HasBdParam(const string &pname) const
{
    return bd_params_.count(pname) > 0;
}

// ── BdStatusString (unchanged logic) ─────────────────────────────────────────

string CAEN_Board::GetBdStatusString() const
{
    unsigned int bds = GetBdStatus();
    if (bds == 0) return "OK|normal";

    static const struct { int bit; const char *abbr; const char *full; } flags[] = {
        { 0, "PWRF",  "power-fail"              },
        { 1, "FWCK",  "firmware checksum error"  },
        { 2, "HVCAL", "HV calibration error"     },
        { 3, "TCAL",  "temperature cal error"    },
        { 4, "UNDRT", "under-temperature"        },
        { 5, "OVERT", "over-temperature"         },
    };

    string abbrs, detail;
    for (const auto &f : flags) {
        if (bds & (1u << f.bit)) {
            if (!abbrs.empty())  abbrs  += ' ';
            if (!detail.empty()) detail += ", ";
            abbrs  += f.abbr;
            detail += f.full;
        }
    }
    return abbrs + '|' + detail;
}

// ── Parameter Discovery ──────────────────────────────────────────────────────

// Helper: check if a string looks like a valid CAEN param name
// (non-empty, all printable ASCII, no longer than MAX_PARAM_NAME)
static bool isValidParamName(const char *s)
{
    if (!s || !*s) return false;
    int len = 0;
    for (const char *p = s; *p; ++p, ++len) {
        if (len >= MAX_PARAM_NAME) return false;
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c > 0x7e) return false;   // non-printable
    }
    return len > 0;
}

// Parse name list with a given stride, return vector of valid names
static std::vector<std::string> parseNameList(const char *buf, int maxEntries, bool fixedWidth)
{
    std::vector<std::string> names;
    const char *p = buf;
    for (int i = 0; i < maxEntries; ++i) {
        if (!isValidParamName(p)) break;
        names.emplace_back(p);
        if (fixedWidth)
            p += MAX_PARAM_NAME;
        else
            p += names.back().size() + 1;
    }
    return names;
}

void CAEN_Board::DiscoverChParams()
{
    ch_param_info_.clear();
    if (nChan < 1) return;

    int handle = mother->GetHandle();
    char *nameList = nullptr;
    int parNum = 0;
    unsigned short ch0 = 0;

    int err = CAENHV_GetChParamInfo(handle, slot, ch0, &nameList, &parNum);
    if (err != CAENHV_OK || !nameList || parNum == 0) {
        if (err != CAENHV_OK)
            CAEN_ShowError("DiscoverChParams", err);
        return;
    }

    // Try both fixed-width and variable-length parsing (names only, no CAEN calls)
    auto fixed    = parseNameList(nameList, parNum, true);
    auto variable = parseNameList(nameList, parNum, false);

    // Pick whichever found more valid names; favor variable on tie
    const auto &names = (fixed.size() > variable.size()) ? fixed : variable;

    // Now query properties for the winning name list
    for (const auto &pname : names) {
        ParamInfo pi;
        pi.name = pname;

        CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Type", &pi.type);
        CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Mode", &pi.mode);

        if (pi.isFloat()) {
            CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Minval", &pi.minval);
            CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Maxval", &pi.maxval);
            CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Unit",   &pi.unit);
            CAENHV_GetChParamProp(handle, slot, ch0, pi.name.c_str(), "Exp",    &pi.exp);
        }

        ch_param_info_.push_back(pi);
    }
    free(nameList);

    cout << "  Discovered " << ch_param_info_.size()
         << " channel params for " << model << " slot " << slot << ":";
    for (const auto &pi : ch_param_info_) cout << " " << pi.name;
    cout << endl;
}

void CAEN_Board::DiscoverBdParams()
{
    bd_param_info_.clear();

    int handle = mother->GetHandle();
    char *nameList = nullptr;

    int err = CAENHV_GetBdParamInfo(handle, slot, &nameList);
    if (err != CAENHV_OK || !nameList) {
        if (err != CAENHV_OK)
            CAEN_ShowError("DiscoverBdParams", err);
        return;
    }

    // Board params don't return a count — use generous upper bound
    const int maxBdParams = 64;
    auto fixed    = parseNameList(nameList, maxBdParams, true);
    auto variable = parseNameList(nameList, maxBdParams, false);

    const auto &names = (fixed.size() > variable.size()) ? fixed : variable;

    for (const auto &pname : names) {
        ParamInfo pi;
        pi.name = pname;

        CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Type", &pi.type);
        CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Mode", &pi.mode);

        if (pi.isFloat()) {
            CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Minval", &pi.minval);
            CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Maxval", &pi.maxval);
            CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Unit",   &pi.unit);
            CAENHV_GetBdParamProp(handle, slot, pi.name.c_str(), "Exp",    &pi.exp);
        }

        bd_param_info_.push_back(pi);
    }
    free(nameList);

    cout << "  Discovered " << bd_param_info_.size()
         << " board params for " << model << " slot " << slot << ":";
    for (const auto &pi : bd_param_info_) cout << " " << pi.name;
    cout << endl;
}

// ── ReadBoardMap (init: discover + first read) ───────────────────────────────

void CAEN_Board::ReadBoardMap()
{
    channelList.clear();
    if (nChan < 1) return;

    // 1. Discover available parameters
    DiscoverBdParams();
    DiscoverChParams();

    // 2. Read channel names
    int handle = mother->GetHandle();
    unsigned short list[nChan];
    char nameList[nChan][MAX_CH_NAME];
    for (int k = 0; k < nChan; ++k) list[k] = k;

    int err = CAENHV_GetChName(handle, slot, nChan, list, nameList);
    CAEN_ShowError("HV Board Read Name", err);

    // 3. Create channels
    for (int k = 0; k < nChan; ++k) {
        auto *ch = new CAEN_Channel(this, k, string(nameList[k]));
        channelList.push_back(ch);
    }

    // 4. Detect primary channel (A1932 boards)
    if (model.find("1932") != string::npos)
        primary = 0;

    // 5. Initial read of all params
    ReadAllParams();
}

// ── ReadAllParams (polling: read all board + channel params generically) ─────

void CAEN_Board::ReadAllParams()
{
    int handle = mother->GetHandle();
    unsigned short list[nChan];
    for (int k = 0; k < nChan; ++k) list[k] = k;

    // ── Board-level params ───────────────────────────────────────────────
    unsigned short slotList[1] = { slot };
    for (const auto &pi : bd_param_info_) {
        if (!pi.isReadable()) continue;

        if (pi.isFloat()) {
            float val;
            int err = CAENHV_GetBdParam(handle, 1, slotList, pi.name.c_str(), &val);
            if (err == CAENHV_OK)
                bd_params_[pi.name] = ParamValue::fromFloat(val);
            else if (!CAEN_IsUnsupportedParam(err))
                CAEN_ShowError("HV Board Read " + pi.name, err);
        } else if (pi.isUInt()) {
            unsigned int val;
            int err = CAENHV_GetBdParam(handle, 1, slotList, pi.name.c_str(), &val);
            if (err == CAENHV_OK)
                bd_params_[pi.name] = ParamValue::fromUInt(val);
            else if (!CAEN_IsUnsupportedParam(err))
                CAEN_ShowError("HV Board Read " + pi.name, err);
        }
    }

    // ── Channel-level params (bulk read per param) ───────────────────────
    for (const auto &pi : ch_param_info_) {
        if (!pi.isReadable()) continue;

        if (pi.isFloat()) {
            float vals[nChan];
            int err = CAENHV_GetChParam(handle, slot, pi.name.c_str(), nChan, list, vals);
            if (err == CAENHV_OK) {
                for (int k = 0; k < nChan; ++k)
                    channelList[k]->SetParamDirect(pi.name, vals[k]);
            } else if (!CAEN_IsUnsupportedParam(err)) {
                CAEN_ShowError("HV Board Read " + pi.name, err);
            }
        } else if (pi.isUInt()) {
            unsigned int vals[nChan];
            int err = CAENHV_GetChParam(handle, slot, pi.name.c_str(), nChan, list, vals);
            if (err == CAENHV_OK) {
                for (int k = 0; k < nChan; ++k)
                    channelList[k]->SetParamDirect(pi.name, vals[k]);
            } else if (!CAEN_IsUnsupportedParam(err)) {
                CAEN_ShowError("HV Board Read " + pi.name, err);
            }
        }
    }

    // ── Post-read: check names, evaluate OVL ─────────────────────────────
    for (auto *ch : channelList)
        ch->EvaluateOVL();
}

// ── Bulk write methods (backward compat) ─────────────────────────────────────

void CAEN_Board::SetPower(const bool &on_off)
{
    int handle = mother->GetHandle();
    unsigned short list[nChan];
    unsigned int val[nChan];
    for (int k = 0; k < nChan; ++k) { list[k] = k; val[k] = on_off ? 1 : 0; }
    int err = CAENHV_SetChParam(handle, slot, "Pw", nChan, list, val);
    CAEN_ShowError("HV Board Set Power", err);
}

void CAEN_Board::SetPower(const vector<unsigned int> &on_off)
{
    if (on_off.size() != nChan) {
        cerr << "HV Board Set Power Warning: size mismatch (" << on_off.size()
             << " vs " << nChan << ")" << endl;
        return;
    }
    int handle = mother->GetHandle();
    unsigned short list[nChan];
    unsigned int val[nChan];
    for (int k = 0; k < nChan; ++k) { list[k] = k; val[k] = on_off[k]; }
    int err = CAENHV_SetChParam(handle, slot, "Pw", nChan, list, val);
    CAEN_ShowError("HV Board Set Power", err);
}

void CAEN_Board::SetVoltage(const vector<float> &Vset)
{
    if (Vset.size() != nChan) {
        cerr << "HV Board Set Voltage Warning: size mismatch (" << Vset.size()
             << " vs " << nChan << ")" << endl;
        return;
    }
    int handle = mother->GetHandle();
    unsigned short list[nChan];
    float val[nChan];
    for (int k = 0; k < nChan; ++k) { list[k] = k; val[k] = Vset[k]; }
    int err = CAENHV_SetChParam(handle, slot, "V0Set", nChan, list, val);
    CAEN_ShowError("HV Board Set Voltage", err);
}


//============================================================================//
// CAEN_Crate
//============================================================================//

CAEN_Crate::~CAEN_Crate()
{
    for (auto *bd : boardList) delete bd;
    DeInitialize();
}

bool CAEN_Crate::Initialize()
{
    char arg[32];
    strcpy(arg, ip.c_str());
    int err = CAENHV_InitSystem(sys_type, link_type, arg,
                                username.c_str(), password.c_str(), &handle);
    if (err != CAENHV_OK) {
        CAEN_ShowError("HV Crate Initialize", err);
        return false;
    }
    if (!mapped) ReadCrateMap();
    return true;
}

bool CAEN_Crate::DeInitialize()
{
    if (handle != -1) {
        int err = CAENHV_DeinitSystem(handle);
        if (err != CAENHV_OK) {
            CAEN_ShowError("HV Crate DeInitialize", err);
            return false;
        }
    }
    Clear();
    return true;
}

void CAEN_Crate::Clear()
{
    handle = -1;
    boardList.clear();
    mapped = false;
    for (auto &i : slot_map) i = 0;
}

void CAEN_Crate::ReadCrateMap()
{
    if (handle < 0) {
        cerr << "HV Crate Read Map Error: crate " << name << " is not initialized!" << endl;
        return;
    }

    boardList.clear();
    for (auto &i : slot_map) i = -1;

    unsigned short NbofSlot;
    unsigned short *NbofChList;
    char *modelList, *descList;
    unsigned short *serNumList;
    unsigned char *fmwMinList, *fmwMaxList;

    int err = CAENHV_GetCrateMap(handle, &NbofSlot, &NbofChList, &modelList,
                                 &descList, &serNumList, &fmwMinList, &fmwMaxList);

    char *m = modelList, *d = descList;
    if (err == CAENHV_OK) {
        for (int sl = 0; sl < NbofSlot; ++sl, m += strlen(m) + 1, d += strlen(d) + 1) {
            if (!NbofChList[sl]) continue;

            // TODO: get rid of this hard-coded exception
            if ((id == 5 && sl == 14) || (id == 4 && sl == 12) || (id == 4 && sl == 14))
                continue;

            auto *newBoard = new CAEN_Board(this, m, d, sl, NbofChList[sl],
                                            serNumList[sl], fmwMinList[sl], fmwMaxList[sl]);
            newBoard->ReadBoardMap();   // discovers params + initial read

            slot_map[sl] = boardList.size();
            boardList.push_back(newBoard);
        }
        mapped = true;
    } else {
        CAEN_ShowError("HV Crate Read Map", err);
    }

    free(NbofChList);
    free(modelList);
    free(descList);
    free(serNumList);
    free(fmwMinList);
    free(fmwMaxList);
}

void CAEN_Crate::PrintCrateMap()
{
    cout << "Slot map is:" << endl;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (slot_map[i] >= 0)
            cout << slot_map[i] << ": " << i << endl;
    }
}

CAEN_Board *CAEN_Crate::GetBoard(const unsigned short &slot)
{
    if (slot >= MAX_SLOTS) {
        cerr << "Crate does not have slot " << slot << endl;
        return nullptr;
    }
    size_t index = slot_map[slot];
    if (index >= boardList.size()) return nullptr;
    return boardList[index];
}

void CAEN_Crate::HeartBeat()
{
    char sw[30];
    int err = CAENHV_GetSysProp(handle, "SwRelease", sw);
    CAEN_ShowError("HV Crate Heartbeat", err);
}

void CAEN_Crate::ReadAllParams()
{
    for (auto *bd : boardList)
        bd->ReadAllParams();
}

void CAEN_Crate::SetPower(const bool &on_off)
{
    for (auto *bd : boardList) bd->SetPower(on_off);
}


//============================================================================//
// CAEN HV General
//============================================================================//

ostream &operator<<(ostream &os, CAEN_Board &b)
{
    return os << b.GetModel() << ", " << b.GetDescription() << ", "
              << b.GetSlot() << ", " << b.GetSize() << ", "
              << b.GetSerialNum() << ", " << b.GetFirmware() << ".";
}

void CAEN_ShowError(const string &prefix, const int &err, const bool &ShowSuccess)
{
    if (err == CAENHV_OK && !ShowSuccess) return;
    if (err == CAENHV_OK) {
        cout << prefix << ": Command successfully executed." << endl;
        return;
    }
    string result = prefix + " ERROR: ";
    if (err < 0) {
        result += "Power supply error (code " + to_string(err) + ")";
        cerr << result << endl;
        return;
    }
    switch (err) {
    case 0x01: result += "Operating system error"; break;
    case 0x02: result += "Write error in communication channel"; break;
    case 0x03: result += "Read error in communication channel"; break;
    case 0x04: result += "Timeout in server communication"; break;
    case 0x05: result += "Command Front End application is down"; break;
    case 0x06: result += "Communication with system not yet connected by a Login command"; break;
    case 0x07: result += "Execute command not yet implemented"; break;
    case 0x08: result += "Get property not yet implemented"; break;
    case 0x09: result += "Set property not yet implemented"; break;
    case 0x0a: result += "Communication with RS232 not yet implemented"; break;
    case 0x0b: result += "User memory not sufficient"; break;
    case 0x0c: result += "Value out of range"; break;
    case 0x0d: result += "Property not yet implemented"; break;
    case 0x0e: result += "Property not found"; break;
    case 0x0f: result += "Command not found"; break;
    case 0x10: result += "Not a property"; break;
    case 0x11: result += "Not a reading property"; break;
    case 0x12: result += "Not a writing property"; break;
    case 0x13: result += "Not a command"; break;
    case 0x14: result += "Configuration change"; break;
    case 0x15: result += "Parameter's property not found"; break;
    case 0x16: result += "Parameter not found"; break;
    case 0x17: result += "No data present"; break;
    case 0x18: result += "Device already open"; break;
    case 0x19: result += "Too many devices opened"; break;
    case 0x1a: result += "Function parameter not valid"; break;
    case 0x1b: result += "Function not available for the connected device"; break;
    case 0x1c: result += "Socket error"; break;
    case 0x1d: result += "Communication error"; break;
    case 0x1e: result += "Not yet implemented"; break;
    case 0x1001: result += "Device already connected"; break;
    case 0x1002: result += "Device not connected"; break;
    case 0x1003: result += "Operating system error"; break;
    case 0x1004: result += "Login failed"; break;
    case 0x1005: result += "Logout failed"; break;
    case 0x1006: result += "Link type not supported"; break;
    default: result += "Unknown error code (" + to_string(err) + ")"; break;
    }
    cerr << result << endl;
}


// ── Configurable voltage limits ──────────────────────────────────────────────

static vector<CAEN_Channel::VoltageLimitRule> voltage_limit_rules;

void CAEN_Channel::SetVoltageLimit(const string &pattern, float limit)
{
    for (auto &rule : voltage_limit_rules) {
        if (rule.pattern == pattern) { rule.limit = limit; return; }
    }
    voltage_limit_rules.push_back({pattern, limit});
}

void CAEN_Channel::ClearVoltageLimits() { voltage_limit_rules.clear(); }

const vector<CAEN_Channel::VoltageLimitRule> &CAEN_Channel::GetVoltageLimitRules()
{
    return voltage_limit_rules;
}

static bool matchPattern(const string &pattern, const string &name)
{
    if (pattern.empty()) return false;
    if (pattern == "*") return true;
    if (pattern.back() == '*')
        return name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    return name == pattern;
}

float CAEN_VoltageLimit(const string &name)
{
    for (const auto &rule : voltage_limit_rules) {
        if (matchPattern(rule.pattern, name)) return rule.limit;
    }
    if (!name.empty() && name[0] == 'G') return 1950;
    if (!name.empty() && name[0] == 'W') return 1450;
    if (!name.empty() && name[0] == 'L') return 2000;
    if (!name.empty() && name[0] == 'S') return 2000;
    if (!name.empty() && name[0] == 'P') return 3000;
    if (!name.empty() && name[0] == 'H') return 2000;
    return 1500;
}

// ── Error ignore list ────────────────────────────────────────────────────────

vector<string> CAEN_Channel::error_ignore_list;

void CAEN_Channel::SetErrorIgnoreList(const vector<string> &names) { error_ignore_list = names; }
const vector<string> &CAEN_Channel::GetErrorIgnoreList() { return error_ignore_list; }

void CAEN_ShowChError(const string &n, const unsigned int &err_bit)
{
    for (const auto &ignored : CAEN_Channel::GetErrorIgnoreList()) {
        if (n == ignored) return;
    }
    if (err_bit & (1 << 3))  cerr << "Channel " << n << " is in overcurrent!" << endl;
    if (err_bit & (1 << 4))  cerr << "Channel " << n << " is in overvoltage!" << endl;
    if (err_bit & (1 << 5))  cerr << "Channel " << n << " is in undervoltage!" << endl;
    if (err_bit & (1 << 6))  cerr << "Channel " << n << " is in external trip!" << endl;
    if (err_bit & (1 << 7))  cerr << "Channel " << n << " is in max voltage!" << endl;
    if (err_bit & (1 << 8))  cerr << "Channel " << n << " is in external disable!" << endl;
    if (err_bit & (1 << 9))  cerr << "Channel " << n << " is in internal trip!" << endl;
    if (err_bit & (1 << 10)) cerr << "Channel " << n << " is in calibration error!" << endl;
    if (err_bit & (1 << 11)) cerr << "Channel " << n << " is unplugged!" << endl;
    if (err_bit & (1 << 13)) cerr << "Channel " << n << " is in overvoltage protection!" << endl;
    if (err_bit & (1 << 14)) cerr << "Channel " << n << " is in power fail!" << endl;
    if (err_bit & (1 << 15)) cerr << "Channel " << n << " is in temperature error!" << endl;
}
