#pragma once
// proton-ds gadget — bridge client: daemon ↔ shim IPC (E3 Phase 2).
//
// Spec: proton-ds/docs/gadget-bridge-spec.md
// The shim connects to the daemon's bridge socket, forwards daemon-serialized
// 64-byte DS4 reports to ep1, and relays ep2 output reports back. In bridge
// mode the shim's own evdev reader and serializer are BYPASSED — the daemon
// owns the pad.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pds::gadget {

class BridgeClient {
public:
    // Called with every 64-byte input report from the daemon (feed ep1).
    using InputCb = std::function<void(const std::uint8_t[64])>;
    // PAD_INFO (transport, mac[6]) / PAD_GONE from the daemon.
    using PadInfoFn = std::function<void(bool usb, const std::uint8_t mac[6])>;
    using PadGoneFn = std::function<void()>;

    void init(std::string socket_path, InputCb on_input,
              PadInfoFn on_pad_info, PadGoneFn on_pad_gone);

    void start();
    void stop(); // joins; socket closed

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

    // Queue an output report (ep2 data). Safe from any thread; no-op offline.
    void send_output(const std::uint8_t* data, std::size_t len);

private:
    void run();
    bool connect_once();

    std::string path_;
    InputCb on_input_;
    PadInfoFn on_pad_info_;
    PadGoneFn on_pad_gone_;

    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

} // namespace pds::gadget
