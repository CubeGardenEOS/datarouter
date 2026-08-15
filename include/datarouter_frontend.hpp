// C++ RAII wrapper over the datarouter Rust frontend C ABI
// (datarouter_frontend.h). The frontend opens the switch-owned shared memory
// through the control plane and uses the **direct (unsafe) data plane by
// default** (2 payload copies, single-frame messages ≤ MAX_APP_PAYLOAD);
// set options::data_plane = DATAROUTER_FRONTEND_DATA_PLANE_ROUTED for the
// switch-routed plane (queue + chunking/ACK/reassembly).
//
// Link against libblinkcms_datarouter (built from the Rust staticlib).

#ifndef DATAROUTER_FRONTEND_HPP
#define DATAROUTER_FRONTEND_HPP

#include "datarouter_frontend.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace datarouter {

/// Error carrying the datarouter error code (datarouter_strerror text).
class frontend_error : public std::runtime_error {
public:
    explicit frontend_error(int code)
        : std::runtime_error(datarouter_strerror(code) ? datarouter_strerror(code) : "datarouter error"),
          code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

/// Connection options; data_plane defaults to the direct (unsafe) plane.
struct options {
    std::string shm_name = "/datarouter";
    std::string service_name;
    uint32_t connect_timeout_ms = 3000;
    uint32_t recv_timeout_ms = 3000;
    std::string auth_token; // required when the switch runs with --token
    uint8_t data_plane = DATAROUTER_FRONTEND_DATA_PLANE_DIRECT;
};

struct message_info {
    std::string source;
    size_t len = 0;
};

class frontend {
public:
    frontend() = default;
    ~frontend() { close(); }
    frontend(const frontend&) = delete;
    frontend& operator=(const frontend&) = delete;
    frontend(frontend&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    frontend& operator=(frontend&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    static frontend open(const options& opts) {
        datarouter_frontend_options_v3 c{};
        std::vector<char> shm(opts.shm_name.begin(), opts.shm_name.end());
        shm.push_back('\0');
        std::vector<char> svc(opts.service_name.begin(), opts.service_name.end());
        svc.push_back('\0');
        std::vector<char> tok;
        if (!opts.auth_token.empty()) {
            tok.assign(opts.auth_token.begin(), opts.auth_token.end());
            tok.push_back('\0');
        }
        c.shm_name = shm.data();
        c.service_name = svc.data();
        c.connect_timeout_ms = opts.connect_timeout_ms;
        c.recv_timeout_ms = opts.recv_timeout_ms;
        c.auth_token = tok.empty() ? nullptr : tok.data();
        c.data_plane = opts.data_plane;

        datarouter_frontend* h = nullptr;
        int rc = datarouter_frontend_open_v3(&c, sizeof(c), DATAROUTER_FRONTEND_ABI_VERSION_V3, &h);
        if (rc != DATAROUTER_OK) {
            throw frontend_error(rc);
        }
        frontend f;
        f.handle_ = h;
        return f;
    }

    /// Send a packed wire-format message. On the direct plane the message
    /// must be a single frame ≤ MAX_APP_PAYLOAD; larger messages throw
    /// frontend_error with code DATAROUTER_ERR_PAYLOAD_TOO_LARGE.
    void send(const std::string& target, const void* data, size_t len, uint32_t timeout_ms) {
        int rc = datarouter_frontend_send(handle_, target.c_str(), data, len, timeout_ms);
        if (rc != DATAROUTER_OK) {
            throw frontend_error(rc);
        }
    }

    void send(const std::string& target, const std::vector<uint8_t>& data, uint32_t timeout_ms) {
        send(target, data.data(), data.size(), timeout_ms);
    }

    /// Receive one complete message. On a short buffer the message is not
    /// consumed: DATAROUTER_ERR_PAYLOAD_TOO_LARGE is thrown (frame kept);
    /// use frame_len() to size the buffer and retry.
    message_info recv(void* buf, size_t capacity, uint32_t timeout_ms) {
        datarouter_message_info info{};
        int rc = datarouter_frontend_recv(handle_, buf, capacity, &info, timeout_ms);
        if (rc != DATAROUTER_OK) {
            throw frontend_error(rc);
        }
        message_info out;
        out.len = info.len;
        out.source.assign(info.source, strnlen(info.source, DATAROUTER_SERVICE_NAME_MAX));
        return out;
    }

    message_info recv(std::vector<uint8_t>& buf, uint32_t timeout_ms) {
        return recv(buf.data(), buf.size(), timeout_ms);
    }

    /// Peek the next RX frame's payload length without consuming it.
    /// Throws frontend_error with code DATAROUTER_ERR_AGAIN when empty.
    size_t frame_len() {
        size_t n = 0;
        int rc = datarouter_frontend_frame_len(handle_, &n);
        if (rc != DATAROUTER_OK) {
            throw frontend_error(rc);
        }
        return n;
    }

    void close() {
        if (handle_) {
            datarouter_frontend_close(handle_);
            handle_ = nullptr;
        }
    }

private:
    datarouter_frontend* handle_ = nullptr;
};

} // namespace datarouter

#endif // DATAROUTER_FRONTEND_HPP
