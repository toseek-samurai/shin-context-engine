#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include <iostream>
using namespace shin::observer;
int main(int argc,char** argv) try {
    if(argc==3 && std::string_view(argv[1])=="archive")verify_updater_archive(read_text(argv[2],updater_archive_limit,false));
    else if((argc==3 || argc==4) && std::string_view(argv[1])=="bundle")
        std::cout<<verify_updater_bundle(fs::canonical(argv[2]),argc==4).dump()<<'\n';
    else return 2;
    return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
