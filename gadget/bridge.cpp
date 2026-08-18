// proton-ds gadget — bridge client implementation.
//
// Reconnect policy (spec §5): retry with backoff 250 ms → 2 s; the daemon
// keeps reading the pad regardless, the gadget stays up with neutral reports.

#include "bridge.h"
#include "log.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace pds::gadget {

namespace {
constexpr const char* kTag = "bridge";

// Frame codec (spec §3): [u32 LE length][u8 type][data], length = type+data.
enum FrameType : std::uint8_t {
    kInput = 0x01, kOutput = 0x02, kPadInfo = 0x03,
    kPadGone = 0x04, kGetCalib = 0x05, kCalib = 0x06,
};

std::vector<std::uint8_t> encode(FrameType t, const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> out(5 + len);
    const std::uint32_t l = static_cast<std::uint32_t>(1 + len);
    out[0] = l & 0xFF; out[1] = (l >> 8) & 0xFF;
    out[2] = (l >> 16) & 0xFF; out[3] = (l >> 24) & 0xFF;
    out[4] = static_cast<std::uint8_t>(t);
    if (len) std::memcpy(out.data() + 5, data, len);
    return out;
}

} // namespace

void BridgeClient::init(std::string socket_path, InputCb on_input,
                        PadInfoFn on_pad_info, PadGoneFn on_pad_gone) {
    path_ = std::move(socket_path);
    on_input_ = std::move(on_input);
    on_pad_info_ = std::move(on_pad_info);
    on_pad_gone_ = std::move(on_pad_gone);
}

bool BridgeClient::connect_once() {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    connected_ = true;
    LOGI(kTag, "connected to daemon at %s (fd=%d)", path_.c_str(), fd_);
    return true;
}

void BridgeClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&BridgeClient::run, this);
}

void BridgeClient::stop() {
    running_ = false;
    connected_ = false;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR); // wake the parked recv()
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void BridgeClient::run() {
    std::vector<std::uint8_t> buf;
    int backoff_ms = 250;
    std::uint64_t frames = 0;

    while (running_) {
        if (!connected_ && !connect_once()) {
            ::usleep(backoff_ms * 1000);
            backoff_ms = backoff_ms < 2000 ? backoff_ms * 2 : 2000;
            continue;
        }
        backoff_ms = 250;

        std::uint8_t chunk[4096];
        ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGW(kTag, "recv: %s", std::strerror(errno));
        } else if (n == 0) {
            LOGW(kTag, "daemon closed the connection");
        } else {
            buf.insert(buf.end(), chunk, chunk + n);
            // decode all complete frames
            while (buf.size() >= 5 && running_) {
                std::uint32_t len = buf[0] | (buf[1] << 8) |
                                    (static_cast<std::uint32_t>(buf[2]) << 16) |
                                    (static_cast<std::uint32_t>(buf[3]) << 24);
                if (len < 1 || len > 4096) { // garbage: resync by dropping
                    LOGW(kTag, "frame length %u out of range — dropping buffer", len);
                    buf.clear();
                    break;
                }
                if (buf.size() < 4u + len) break;
                std::uint8_t type = buf[4];
                const std::uint8_t* data = buf.data() + 5;
                std::size_t dlen = len - 1;
                ++frames;
                switch (type) {
                    case kInput:
                        if (dlen == 64 && on_input_) on_input_(data);
                        else LOGW(kTag, "INPUT frame with bad length %zu", dlen);
                        break;
                    case kPadInfo:
                        if (dlen >= 7 && on_pad_info_)
                            on_pad_info_(data[0] == 0, data + 1);
                        break;
                    case kPadGone:
                        if (on_pad_gone_) on_pad_gone_();
                        break;
                    case kCalib:
                        break; // bonus (spec 0x06); MVP synthesizes 0x02 itself
                    default:
                        LOGD(kTag, "ignored frame type 0x%02x (%zu B)", type, dlen);
                        break;
                }
                buf.erase(buf.begin(), buf.begin() + 4 + len);
            }
            if (frames % 1000 == 0 && frames)
                LOGD(kTag, "frames so far: %llu", static_cast<unsigned long long>(frames));
            continue; // stay in recv loop
        }

        // fall-through: connection lost
        connected_ = false;
        ::close(fd_);
        fd_ = -1;
        buf.clear();
        if (on_pad_gone_) on_pad_gone_(); // gadget goes neutral until reconnect
    }
    LOGI(kTag, "bridge thread stopped (frames=%llu)",
         static_cast<unsigned long long>(frames));
}

void BridgeClient::send_output(const std::uint8_t* data, std::size_t len) {
    int fd = fd_;
    if (!connected_.load() || fd < 0) return; // offline: caller's local sink applies
    auto frame = encode(kOutput, data, len);
    size_t off = 0;
    while (off < frame.size()) {
        ssize_t w = ::send(fd, frame.data() + off, frame.size() - off, MSG_NOSIGNAL);
        if (w > 0) { off += static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        LOGW(kTag, "output send failed: %s", std::strerror(errno));
        break;
    }
}

} // namespace pds::gadget
