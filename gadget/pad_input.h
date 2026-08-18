#pragma once
// proton-ds gadget — DualSense evdev reader (transport-agnostic: USB & BT).
// Pattern follows the daemon's input_device.cpp (open O_NONBLOCK + EVIOCGRAB)
// and device_manager.cpp translate_events() mapping.

#include "ds_translate.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace pds::gadget {

class PadInput {
public:
    // Called with a fully serialized 64-byte DS4 report on every SYN_REPORT.
    using ReportCb = std::function<void(const std::uint8_t[64])>;

    void init(ReportCb on_report);
    bool start();          // scans for pads; false if none found yet
    void stop();

    bool pad_connected() const { return connected_; }
    std::uint64_t reports() const { return reports_; }

    // T5: apply a DS4 output report (id 0x05, from ep2 / ep0 SET_REPORT)
    // to the physical pad: rumble motors + lightbar. USB pads only in
    // Phase 1 (BT output = report 0x31+CRC, Phase 2 via the daemon).
    void apply_ds4_output(const std::uint8_t* data, std::size_t len);

private:
    void loop();
    bool open_pads();      // find + open + grab gamepad & touchpad nodes
    void close_pads();
    bool ensure_hidraw();  // open the physical pad's hidraw (output path)
    void handle_gamepad_event(std::uint16_t type, std::uint16_t code, std::int32_t value);
    void handle_touchpad_event(std::uint16_t type, std::uint16_t code, std::int32_t value);

    int pad_fd_ = -1;      // DualSense gamepad node
    int touch_fd_ = -1;    // DualSense touchpad node
    int hidraw_fd_ = -1;   // physical pad hidraw (output reports)
    bool usb_transport_ = true; // BT pads get output only in Phase 2
    std::string pad_path_, touch_path_;

    PadState state_{};
    Ds4Serializer ser_;
    ReportCb on_report_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> reports_{0};
};

} // namespace pds::gadget
