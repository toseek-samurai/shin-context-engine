#include <mimalloc.h>
#include <cstdlib>
#include <new>
#include <thread>

int main() {
    if (mi_version() != 30503) return 1;
    void* small = std::malloc(37);
    void* aligned = ::operator new(8192, std::align_val_t{4096});
    const bool owned = small && mi_is_in_heap_region(small) && mi_is_in_heap_region(aligned);
    std::thread release([&] {
        std::free(small);
        ::operator delete(aligned, std::align_val_t{4096});
    });
    release.join();
    return owned ? 0 : 1;
}
