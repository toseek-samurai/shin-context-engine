#include "shin/observer/resident_retention.h"
#include <stdexcept>

int main() try {
    using namespace shin::observer;
    const auto expect = [](bool value) { if (!value) throw std::runtime_error("resident retention policy"); };
    expect(!resident_memory_victim({{40, true, 3000}, {60, true, 5000}}, 100));
    expect(resident_memory_victim({{40, true, 3000}, {61, true, 5000}}, 100) == 1);
    expect(resident_memory_victim({{90, false, 9000}, {20, true, 3000}}, 100) == 1);
    expect(!resident_memory_victim({{110, false, 9000}}, 100));
    expect(!resident_memory_victim({{110, true, 1999}}, 100));
    expect(!resident_memory_victim({{110, true, 5000}, {std::nullopt, true, 9000}}, 100));
    expect(resident_memory_victim({{std::numeric_limits<std::uint64_t>::max(), true, 3000}, {1, true, 5000}}, 100) == 1);
    return 0;
} catch (...) { return 1; }
