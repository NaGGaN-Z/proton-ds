#pragma once
// proton-ds gadget — DS4 input-report serializer (64 B, report ID 0x01).
//
// Byte layout mirrors the ds4linux daemon's uhid-path send_report() exactly
// (virtual_device.cpp) — the proven v0.1 wire format Detroit/W3/BG3/DSDC
// accepted. Phase 2 bridge mode forwards bytes produced by the daemon itself
// with the same semantics (bridge spec §1).

#include <cstdint>

namespace pds::gadget {

struct PadState {
    // Sticks / triggers (0–255, 0x80 centre)
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    std::uint8_t l2 = 0, r2 = 0;

    // D-pad as hat components (-1/0/1) — encoded to 4-bit hat in pack()
    int hat_x = 0, hat_y = 0;

    // Buttons
    bool square = false, cross = false, circle = false, triangle = false;
    bool l1 = false, r1 = false, l2_btn = false, r2_btn = false;
    bool share = false, options = false, l3 = false, r3 = false;
    bool ps = false, touchpad = false;

    // Touchpad (12-bit coords, hid-playstation scale)
    std::uint16_t touch_x = 0, touch_y = 0;
    std::uint8_t touch_id = 0;
    bool touch_active = false;
};

// Serialize state into a 64-byte DS4 USB input report (report ID 0x01).
// Internal counters (frame counter, timestamp, touch counter) live inside
// the serializer state and advance per call — construct one per session.
class Ds4Serializer {
public:
    void pack(const PadState& s, std::uint8_t out[64]);

private:
    std::uint8_t frame_counter_ = 0;
    std::uint8_t touch_counter_ = 0;
    std::uint16_t timestamp_ = 0;
};

} // namespace pds::gadget
