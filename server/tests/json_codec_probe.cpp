#include "shin/server/model.h"

#include <iostream>
#include <string>

int main() {
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.size() > 1024 * 1024) return 2;
            std::cout << shin::server::Json::parse(line).dump() << '\n' << std::flush;
        }
        return std::cin.bad() ? 2 : 0;
    } catch (const std::exception&) {
        std::cerr << "Invalid JSON codec fixture input.\n";
        return 2;
    }
}
