#include <caen_channel.h>
#include <fmt/format.h>
#include <iostream>
#include <string>
#include <vector>


int main()
{
    // crate list
    std::vector<std::pair<std::string, std::string>> crate_list = {
        {"PRadHV_1", "129.57.160.67"},
        {"PRadHV_2", "129.57.160.68"},
        {"PRadHV_3", "129.57.160.69"},
        {"PRadHV_4", "129.57.160.70"},
        {"PRadHV_5", "129.57.160.71"},
    };

    std::vector<CAEN_Crate*> crates;
    int crid = 0;
    for (const auto& [name, ip] : crate_list) {
        CAEN_Crate *new_crate = new CAEN_Crate(crid, name, ip, CAENHV::SY1527, LINKTYPE_TCPIP, "admin", "admin");
        crid ++;
        crates.push_back(new_crate);
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


    // read hv settings
    for(auto &crate : crates)
    {
        for(auto &board : crate->GetBoardList())
        {
            for(auto &channel : board->GetChannelList())
            {
                auto hvinfo = fmt::format("{:8s} {:4d} {:4d}   {:8s}: {:.2f} / {:.2f}",
                                          crate->GetName(),
                                          board->GetSlot(),
                                          channel->GetChannel(),
                                          channel->GetName(),
                                          channel->GetVMon(),
                                          channel->GetVSet());
                std::cout << hvinfo << std::endl;
            }
        }
    }
    return 0;
}
