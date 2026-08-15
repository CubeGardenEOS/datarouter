// C++ frontend binding test: direct (default) + routed planes via the C++
// RAII wrapper (include/datarouter_frontend.hpp), against a real switch.
//
// Build:
//   g++ -std=c++17 -O2 -Iinclude -Lbuild -Wl,-rpath,$PWD/build
//       -o /tmp/test_frontend_cpp tests/test_frontend_cpp.cpp -lblinkcms_datarouter -lpthread
// Run:
//   /tmp/test_frontend_cpp
#include "datarouter_frontend.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;
std::atomic<uint64_t> g_unique{0};

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                  \
        } else {                                                           \
            std::printf("ok: %s\n", msg);                                  \
        }                                                                  \
    } while (0)

std::string unique_shm() {
    return "/datarouter-cpp-" + std::to_string(::getpid()) + "-" +
           std::to_string(g_unique.fetch_add(1));
}

// Spawn the switch on a unique shm; returns its name and pid (for cleanup).
std::pair<std::string, pid_t> spawn_switch() {
    std::string shm = unique_shm();
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl("build/datarouter-switch", "datarouter-switch", "--name", shm.c_str(),
                "--ttl-ms", "30000", "--allow", "cpp_sender,cpp_receiver", nullptr);
        ::_exit(127);
    }
    usleep(200000);
    return {shm, pid};
}

std::vector<uint8_t> pack_request(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> wire(8 + body.size());
    wire[0] = 0x07; // MSG_BUSINESS_REQUEST
    wire[1] = 0;
    wire[2] = wire[3] = 0;
    wire[4] = static_cast<uint8_t>(body.size() & 0xFF);
    wire[5] = static_cast<uint8_t>((body.size() >> 8) & 0xFF);
    wire[6] = static_cast<uint8_t>((body.size() >> 16) & 0xFF);
    wire[7] = static_cast<uint8_t>((body.size() >> 24) & 0xFF);
    std::memcpy(wire.data() + 8, body.data(), body.size());
    return wire;
}

void test_direct_plane_round_trip(const std::string& shm) {
    datarouter::options sender_opts;
    sender_opts.shm_name = shm;
    sender_opts.service_name = "cpp_sender";
    datarouter::options receiver_opts = sender_opts;
    receiver_opts.service_name = "cpp_receiver";

    datarouter::frontend sender = datarouter::frontend::open(sender_opts);
    datarouter::frontend receiver = datarouter::frontend::open(receiver_opts);

    std::vector<uint8_t> body = {'c', '+', '+', ' ', 'd', 'i', 'r', 'e', 'c', 't'};
    std::vector<uint8_t> wire = pack_request(body);
    sender.send("cpp_receiver", wire, 2000);

    std::vector<uint8_t> buf(1 << 20);
    datarouter::message_info info = receiver.recv(buf, 3000);
    CHECK(info.len == wire.size(), "direct recv length");
    CHECK(info.source == "cpp_sender", "direct recv source");
    CHECK(std::memcmp(buf.data(), wire.data(), wire.size()) == 0, "direct recv bytes");
}

void test_direct_plane_frame_len_and_short_buffer(const std::string& shm) {
    datarouter::options sender_opts;
    sender_opts.shm_name = shm;
    sender_opts.service_name = "cpp_sender";
    datarouter::options receiver_opts = sender_opts;
    receiver_opts.service_name = "cpp_receiver";
    datarouter::frontend sender = datarouter::frontend::open(sender_opts);
    datarouter::frontend receiver = datarouter::frontend::open(receiver_opts);

    bool saw_again = false;
    try {
        receiver.frame_len();
    } catch (const datarouter::frontend_error& e) {
        saw_again = e.code() == DATAROUTER_ERR_AGAIN;
    }
    CHECK(saw_again, "frame_len empty is ERR_AGAIN");

    std::vector<uint8_t> wire = pack_request(std::vector<uint8_t>(200 * 1024, 0xE2));
    sender.send("cpp_receiver", wire, 2000);

    size_t peeked = receiver.frame_len();
    CHECK(peeked == wire.size(), "frame_len matches");

    bool saw_ptl = false;
    std::vector<uint8_t> small(1024);
    try {
        receiver.recv(small, 3000);
    } catch (const datarouter::frontend_error& e) {
        saw_ptl = e.code() == DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    CHECK(saw_ptl, "short buffer keeps frame with PAYLOAD_TOO_LARGE");

    std::vector<uint8_t> large(1 << 20);
    datarouter::message_info info = receiver.recv(large, 3000);
    CHECK(info.len == wire.size(), "retry with larger buffer");
    CHECK(std::memcmp(large.data(), wire.data(), wire.size()) == 0, "retried bytes match");
}

void test_routed_plane_round_trip(const std::string& shm) {
    datarouter::options sender_opts;
    sender_opts.shm_name = shm;
    sender_opts.service_name = "cpp_sender";
    sender_opts.data_plane = DATAROUTER_FRONTEND_DATA_PLANE_ROUTED;
    datarouter::options receiver_opts = sender_opts;
    receiver_opts.service_name = "cpp_receiver";
    datarouter::frontend sender = datarouter::frontend::open(sender_opts);
    datarouter::frontend receiver = datarouter::frontend::open(receiver_opts);

    std::vector<uint8_t> wire = pack_request({'r', 'o', 'u', 't', 'e', 'd'});
    sender.send("cpp_receiver", wire, 2000);

    std::vector<uint8_t> buf(1 << 20);
    datarouter::message_info info = receiver.recv(buf, 3000);
    CHECK(info.len == wire.size(), "routed recv length");
    CHECK(info.source == "cpp_sender", "routed recv source");
    CHECK(std::memcmp(buf.data(), wire.data(), wire.size()) == 0, "routed recv bytes");
}

} // namespace

int main() {
    auto [shm, switch_pid] = spawn_switch();
    std::printf("switch shm=%s\n", shm.c_str());
    try {
        test_direct_plane_round_trip(shm);
        test_direct_plane_frame_len_and_short_buffer(shm);
        test_routed_plane_round_trip(shm);
    } catch (const datarouter::frontend_error& e) {
        std::fprintf(stderr, "FAIL unexpected frontend error code=%d: %s\n", e.code(),
                     e.what());
        ++g_failures;
    }
    ::kill(switch_pid, SIGKILL);
    ::waitpid(switch_pid, nullptr, 0);
    std::printf(g_failures == 0 ? "ALL CPP TESTS PASSED\n" : "CPP TESTS FAILED: %d\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
