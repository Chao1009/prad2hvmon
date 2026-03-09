// A simple program to read/write the CAEN high voltage crates
//
// Author: Chao Peng (Argonne National Laboratory)
// Date: 03/09/2026


#include <ConfigParser.h>
#include <ConfigOption.h>
#include <caen_channel.h>
#include <fmt/format.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>


// global
// crate list
std::vector<std::pair<std::string, std::string>> crate_list = {
    {"PRadHV_1", "129.57.160.67"},
    {"PRadHV_2", "129.57.160.68"},
    {"PRadHV_3", "129.57.160.69"},
    {"PRadHV_4", "129.57.160.70"},
    {"PRadHV_5", "129.57.160.71"},
};
std::vector<CAEN_Crate*> crates;
std::map<std::string, CAEN_Crate*> crate_map;

bool init_crates(const std::vector<std::pair<std::string, std::string>> &list);
void print_channels(const std::string &save_path);
void write_channels(const std::string &setting_path);


int main(int argc, char *argv[])
{
    ConfigOption co;
    co.SetDesc("usage: %0 <mode> [read, write]");
    co.SetDesc('f', "path to the list of channel's voltage setting, needed by write mode.");
    co.SetDesc('s', "path to save the list of channel's voltage setting, optional by read mode.");
    co.SetDesc('h', "show help messages.");

    co.AddOpts(ConfigOption::arg_require, 'f', "file");
    co.AddOpts(ConfigOption::arg_require, 's', "save");
    co.AddOpts(ConfigOption::help_message, 'h', "help");

    // one positional argument required (mode)
    if(!co.ParseArgs(argc, argv) || co.NbofArgs() != 1) {
        std::cout << co.GetInstruction() << std::endl;
        return -1;
    }

    std::string setting_file = "", save_file = "";
    // parse optional arguments
    for(auto &opt : co.GetOptions())
    {
        switch(opt.mark)
        {
            case 'f':
                setting_file = opt.var.String();
                break;
            case 's':
                save_file = opt.var.String();
                break;
        }
    }

    // try to connect to the crates
    if ( !init_crates(crate_list) ) {
        std::cerr << "Aborted! Crates initialization failed!" << std::endl;
        return -1;
    }

    // get mode
    auto mode = co.GetArgument(0).String();
    if ( mode == "read" ) {
        print_channels(save_file);
    } else if ( mode == "write" ) {
        write_channels(setting_file);
    } else {
        std::cerr << "Aborted! Unknown mode: " << mode << std::endl;
        return -1;
    }

    return 0;
}


bool init_crates(const std::vector<std::pair<std::string, std::string>> &list)
{
    int crid = 0;
    for (const auto& [name, ip] : list) {
        CAEN_Crate *new_crate = new CAEN_Crate(crid, name, ip, CAENHV::SY1527, LINKTYPE_TCPIP, "admin", "admin");
        crid ++;
        crates.push_back(new_crate);
        crate_map[name] = new_crate;
    }

    // connect to and initialize crates
    int init_cnt = 0;
    for(auto &crate : crates)
    {
        if(crate->Initialize()) {
            std::cout << fmt::format("Connected to high voltage system {:s} @ {:s}", crate->GetName(), crate->GetIP())
                      << std::endl;
            ++ init_cnt;
        } else {
            std::cerr << fmt::format("Cannot connect to {:s} @ {:s}", crate->GetName(), crate->GetIP())
                      << std::endl;
        }
    }

    std::cout << fmt::format("HV crates initialize DONE, successful on {:d}/{:d} crates", init_cnt, crates.size())
              << std::endl;

    return ( init_cnt == crates.size() );
}


// helper function to write info to iostream
inline void write_lines(std::ostream& output, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        output << line << std::endl;
    }
}


void print_channels(const std::string &save_path)
{
    // read hv settings
    std::vector<std::string> hvinfos;
    hvinfos.push_back(fmt::format("# {:10s} {:4s} {:8s} {:16s} {:8s} {:8s}",
                "crate", "slot", "channel", "name", "VMon", "VSet"
                ));
    for(auto &crate : crates)
    {
        crate->ReadVoltage();
        for(auto &board : crate->GetBoardList())
        {
            for(auto &channel : board->GetChannelList())
            {
                auto hvinfo = fmt::format("{:12s} {:4d} {:8d} {:16s} {:8.2f} {:8.2f}",
                                          crate->GetName(),
                                          board->GetSlot(),
                                          channel->GetChannel(),
                                          channel->GetName(),
                                          channel->GetVMon(),
                                          channel->GetVSet());
                hvinfos.push_back(hvinfo);
            }
        }
    }

    // print out
    write_lines(std::cout, hvinfos);

    // save to file
    if (!save_path.empty()) {
        std::ofstream output_file(save_path);
        write_lines(output_file, hvinfos);
        output_file.close();
    }
}

void write_channels(const std::string &setting_path)
{
    ConfigParser c_parser;

    c_parser.ReadFile(setting_path);

    while(c_parser.ParseLine())
    {
        std::string crate_name, channel_name;
        int slot;
        unsigned short channel;
        float VMon, VSet;

        // VMon is optional and not used
        if ( c_parser.NbofElements() == 5) {
            c_parser >> crate_name >> slot >> channel >> channel_name >> VSet;
        } else if ( c_parser.NbofElements() == 6) {
            c_parser >> crate_name >> slot >> channel >> channel_name >> VMon >> VSet;
        }

        auto crate = crate_map[crate_name];
        auto board = crate->GetBoard(slot);
        auto ch = board->GetChannel(channel);

        if(ch != nullptr) {
            ch->SetName(channel_name);
            ch->SetVoltage(VSet);
            std::cout << fmt::format("crate: {:8s} slot: {:4d} channel: {:4d} set to {:12s} {:.82f}V",
                                     crate_name, slot, channel, channel_name, VSet) << std::endl;
        } else {
            std::cout << fmt::format("crate: {:8s} slot: {:4d} channel: {:4d} not found!",
                                     crate_name, slot, channel) << std::endl;
        }
    }

    std::cout << "Restore the High Voltage Setting from " << setting_path << std::endl;
}

