// proton-ds gadget — DS4 input-report serializer implementation.
// Semantics copied 1:1 from the daemon's uhid-path send_report() (the
// 4/4-verified v0.1 format); see ds_translate.h for the layout contract.

#include "ds_translate.h"

#include <cstring>

namespace pds::gadget {

namespace {

// 4-bit hat from hat components; table mirrors daemon encode_dpad().
std::uint8_t encode_dpad(int hat_x, int hat_y) {
    static constexpr std::uint8_t table[3][3] = {
        // hat_x:  -1    0    +1
        /* hat_y=-1 */ { 7, 0, 1 }, // NW, N, NE
        /* hat_y= 0 */ { 6, 8, 2 }, // W, none, E
        /* hat_y=+1 */ { 5, 4, 3 }, // SW, S, SE
    };
    return table[hat_y + 1][hat_x + 1];
}

} // namespace

void Ds4Serializer::pack(const PadState& s, std::uint8_t out[64]) {
    std::memset(out, 0, 64);

    out[0] = 0x01; // report ID

    // Sticks
    out[1] = s.lx;
    out[2] = s.ly;
    out[3] = s.rx;
    out[4] = s.ry;

    // Byte 5: hat (low nibble) + face buttons
    out[5] = (encode_dpad(s.hat_x, s.hat_y) & 0x0F)
           | (s.square   ? (1u << 4) : 0)
           | (s.cross    ? (1u << 5) : 0)
           | (s.circle   ? (1u << 6) : 0)
           | (s.triangle ? (1u << 7) : 0);

    // Byte 6: shoulder / trigger buttons / share / options / stick clicks
    out[6] = (s.l1      ? (1u << 0) : 0)
           | (s.r1      ? (1u << 1) : 0)
           | (s.l2_btn  ? (1u << 2) : 0)
           | (s.r2_btn  ? (1u << 3) : 0)
           | (s.share   ? (1u << 4) : 0)
           | (s.options ? (1u << 5) : 0)
           | (s.l3      ? (1u << 6) : 0)
           | (s.r3      ? (1u << 7) : 0);

    // Byte 7: PS + touchpad click + 6-bit frame counter
    out[7] = (s.ps       ? (1u << 0) : 0)
           | (s.touchpad ? (1u << 1) : 0)
           | ((frame_counter_ & 0x3F) << 2);
    frame_counter_ = (frame_counter_ + 1) & 0x3F;

    // Triggers (analog)
    out[8] = s.l2;
    out[9] = s.r2;

    // Timestamp: ~5.33 ms per report at USB poll rate (hid-sony jitter filter)
    timestamp_ += 188;
    out[10] = static_cast<std::uint8_t>(timestamp_ & 0xFF);
    out[11] = static_cast<std::uint8_t>((timestamp_ >> 8) & 0xFF);

    // Battery: 0x0B = charged, cable connected (daemon-proven value)
    out[12] = 0x0B;
    // Byte 30: capacity 10/10 + USB wired (0x1A) — DS4-aware titles read
    // 0x00 as "0% battery" and may reject the pad.
    out[30] = 0x1A;

    // Touchpad block (DS4Windows DS4_TOUCHPAD_DATA_OFFSET semantics):
    //   [33] connection info, [34] packet counter, [35-38] point 1,
    //   [39] point 2 inactive.
    out[33] = 0x00;
    out[34] = touch_counter_++;
    if (s.touch_active) {
        out[35] = s.touch_id & 0x7F;
        out[36] = static_cast<std::uint8_t>(s.touch_x & 0xFF);
        out[37] = static_cast<std::uint8_t>(((s.touch_y & 0x0F) << 4)
                                           | ((s.touch_x >> 8) & 0x0F));
        out[38] = static_cast<std::uint8_t>((s.touch_y >> 4) & 0xFF);
    } else {
        out[35] = 0x80; // bit 7 = no touch
    }
    out[39] = 0x80; // touch point 2: inactive

    // Bytes 13-29, 31-32, 40-63: IMU and vendor data — zeroed, matching the
    // uhid path (motion sensors were a separate uinput device there).
}

} // namespace pds::gadget
