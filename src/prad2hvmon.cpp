#include "caen_channel.h"
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
    int try_cnt = 0, fail_cnt = 0;
    for(auto &crate : crates)
    {
        ++ try_cnt;
        if(crate->Initialize()) {
            std::cout << "Connected to high voltage system "
                      << crate->GetName() << "@" << crate->GetIP()
                      << std::endl;
        } else {
            ++ fail_cnt;
            std::cerr << "Cannot connect to "
                      << crate->GetName() << "@" << crate->GetIP()
                      << std::endl;
        }
    }

    std::cout << "HV crates initialize DONE, tried "
              << try_cnt << " crates,  failed on "
              << fail_cnt << " crates" << std::endl;

    return 0;
}
