#include <httplib.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int peers[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, peers) != 0) return 1;
    int result = 0;
    try {
        const char byte = 'G';
        if (write(peers[0], &byte, 1) != 1) throw std::runtime_error("fixture write failed");
        std::atomic<socket_t> listener{peers[0]};
        if (!httplib::detail::keep_alive(listener, peers[1], 5))
            throw std::runtime_error("live readable peer was refused");
        listener = INVALID_SOCKET;
        if (httplib::detail::keep_alive(listener, peers[1], 5))
            throw std::runtime_error("stopped listener admitted a readable queued peer");
        char read_byte = 0;
        if (read(peers[1], &read_byte, 1) != 1 || read_byte != byte)
            throw std::runtime_error("admission check consumed request bytes");
        if (httplib::detail::keep_alive(listener, peers[1], 5))
            throw std::runtime_error("stopped listener admitted an idle peer");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    close(peers[0]);
    close(peers[1]);
    return result;
}
