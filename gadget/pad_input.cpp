// proton-ds gadget — DualSense evdev reader implementation.
//
// Transport-agnostic by design: hid-playstation normalizes USB and BT pads
// into the same evdev codes, so the pad may be plugged either way. Mapping
// is copied from the daemon's device_manager.cpp translate_events() (PR#3
// heritage): BTN_SOUTH=Cross, BTN_EAST=Circle, BTN_NORTH=Triangle,
// BTN_WEST=Square, ABS_Z/ABS_RZ = analog triggers, ABS_HAT0* = D-pad.
//
// Degradation contract: a vanished pad never kills the gadget — we log WARN,
// emit neutral reports, and rescan for hot-plug every 2 s.

#include "pad_input.h"
#include "log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace pds::gadget {

namespace {
constexpr const char* kTag = "pad";
constexpr std::uint16_t kVidSony = 0x054c;
constexpr std::uint16_t kPidDualSense = 0x0ce6;
constexpr std::uint16_t kPidDualSenseEdge = 0x0df2;

bool id_match(const input_id& id) {
    return id.vendor == kVidSony
        && (id.product == kPidDualSense || id.product == kPidDualSenseEdge);
}

// Capability probe via EVIOCGBIT for a single bit (long index into bitmap).
bool has_bit(int fd, unsigned int type, unsigned int code) {
    unsigned long bits[(KEY_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
    if (::ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0) return false;
    return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1;
}

bool open_node(const std::string& path, int& fd_out, bool grab) {
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    input_id id{};
    if (::ioctl(fd, EVIOCGID, &id) < 0 || !id_match(id)) {
        ::close(fd);
        return false;
    }
    if (grab && ::ioctl(fd, EVIOCGRAB, 1) < 0) {
        LOGW(kTag, "EVIOCGRAB failed on %s: %s (another reader holds it?)",
             path.c_str(), std::strerror(errno));
        ::close(fd);
        return false;
    }
    fd_out = fd;
    LOGD(kTag, "opened %s (grab=%d, id=%04x:%04x)", path.c_str(), grab ? 1 : 0,
         id.vendor, id.product);
    return true;
}

} // namespace

void PadInput::init(ReportCb on_report) {
    on_report_ = std::move(on_report);
}

bool PadInput::open_pads() {
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/dev/input", ec)) {
        std::string p = e.path().string();
        if (p.rfind("/dev/input/event") != 0) continue;

        int probe = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (probe < 0) continue;
        input_id id{};
        bool ok = ::ioctl(probe, EVIOCGID, &id) >= 0 && id_match(id);
        bool is_pad = ok && has_bit(probe, EV_KEY, BTN_SOUTH);
        bool is_touch = ok && has_bit(probe, EV_ABS, ABS_MT_POSITION_X);
        ::close(probe);
        if (!ok) continue;

        if (is_pad && pad_fd_ < 0) {
            if (open_node(p, pad_fd_, /*grab=*/true)) {
                pad_path_ = p;
                // Transport probe: bustype is authoritative (hid-playstation
                // fills uniq with the MAC on USB too — daemon heuristic wrong
                // on modern kernels; T5 smoke caught it).
                input_id id2{};
                ::ioctl(pad_fd_, EVIOCGID, &id2);
                usb_transport_ = (id2.bustype == BUS_USB);
                LOGD(kTag, "transport: %s (bustype=0x%04x)",
                     usb_transport_ ? "USB" : "BT", id2.bustype);
            }
        } else if (is_touch && touch_fd_ < 0) {
            if (open_node(p, touch_fd_, /*grab=*/true)) touch_path_ = p;
        }
    }
    connected_ = (pad_fd_ >= 0);
    if (connected_) {
        LOGI(kTag, "pad connected: %s%s%s", pad_path_.c_str(),
             touch_fd_ >= 0 ? " + touchpad " : "",
             touch_fd_ >= 0 ? touch_path_.c_str() : "");
    }
    return connected_;
}

bool PadInput::ensure_hidraw() {
    if (hidraw_fd_ >= 0) return true;
    std::error_code ec;
    for (auto& hr : fs::directory_iterator("/sys/class/hidraw", ec)) {
        std::string sys = hr.path().string();
        std::ifstream uevent(sys + "/device/uevent");
        std::string line;
        bool match = false;
        while (std::getline(uevent, line)) {
            if (line.find("0003:0000054C:00000CE6") != std::string::npos ||
                line.find("0003:0000054C:00000DF2") != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        std::string dev = "/dev/" + hr.path().filename().string();
        int fd = ::open(dev.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            LOGW(kTag, "open %s for output: %s", dev.c_str(), std::strerror(errno));
            continue;
        }
        hidraw_fd_ = fd;
        LOGI(kTag, "hidraw output path: %s", dev.c_str());
        return true;
    }
    return false;
}

void PadInput::apply_ds4_output(const std::uint8_t* data, std::size_t len) {
    // DS4 USB Output Report 0x05 layout (daemon-proven decode):
    //   [0]=0x05  [1]=valid_flag0  [2]=valid_flag1
    //   [4]=right/weak motor  [5]=left/strong motor  [6..8]=LED RGB
    if (len < 9 || data[0] != 0x05) {
        LOGD(kTag, "ignore output report (len=%zu id=0x%02x)", len, len ? data[0] : 0);
        return;
    }
    const std::uint8_t flags = data[1];

    if (!usb_transport_) {
        // Phase 1 scope: BT pads need report 0x31 + CRC32 — that path arrives
        // with the daemon bridge (Phase 2). Warn once per process, not per frame.
        static bool warned = false;
        if (!warned) {
            LOGW(kTag, "output report on BT pad — rumble/lightbar disabled until Phase 2");
            warned = true;
        }
        return;
    }
    if (!ensure_hidraw()) {
        LOGW(kTag, "no hidraw for output — rumble/lightbar dropped");
        return;
    }

    // DualSense USB output report 0x02, 63 B — encoding verified against the
    // live kernel (usbmon + hid-playstation.c dualsense_output_worker):
    //   [1] valid_flag0: 0x02 HAPTICS_SELECT (set for rumble, always)
    //   [2] valid_flag1: 0x04 LIGHTBAR_CONTROL_ENABLE
    //   [3] motor_right (weak)   [4] motor_left (strong)
    //   [39] valid_flag2: 0x04 COMPATIBLE_VIBRATION2 — the actual rumble
    //        switch on vibration_v2 pads; usbmon truncates at 32 B so this
    //        byte is invisible in captures (cost a day to find)
    //   [45]R [46]G [47]B
    // NB: 63 bytes total — a 48-byte write lights the lightbar partially
    //     and never rumbles (2026-08-18 lesson, physically verified).
    // NB: ONE report for rumble+lightbar — hid-sony sends DS4 output with
    //     BOTH flags set; two separate writes make the lightbar report
    //     zero the motors 4 ms later (E2E smoke caught exactly that).
    const bool want_rumble = flags & 0x01;
    const bool want_light = flags & 0x02;
    std::uint8_t buf[63]{};
    buf[0] = 0x02;
    if (want_rumble) {
        buf[1] = 0x02;  // HAPTICS_SELECT
        buf[39] = 0x04; // COMPATIBLE_VIBRATION2 (vibration_v2 pads)
        buf[3] = data[4]; // motor_right = DS4 right/weak
        buf[4] = data[5]; // motor_left = DS4 left/strong
    }
    if (want_light) {
        buf[2] = 0x04; // valid_flag1: lightbar control enable
        buf[45] = data[6];
        buf[46] = data[7];
        buf[47] = data[8];
    }
    if (!want_rumble && !want_light) return; // neither flag: nothing to apply
    if (::write(hidraw_fd_, buf, sizeof(buf)) < 0)
        LOGW(kTag, "output write: %s", std::strerror(errno));
    else
        LOGD(kTag, "output: rumble=%s(w=%u s=%u) light=%s(#%02x%02x%02x)",
             want_rumble ? "on" : "off", data[4], data[5],
             want_light ? "on" : "off", data[6], data[7], data[8]);
}

void PadInput::close_pads() {
    if (pad_fd_ >= 0) {
        ::ioctl(pad_fd_, EVIOCGRAB, 0);
        ::close(pad_fd_);
        pad_fd_ = -1;
    }
    if (touch_fd_ >= 0) {
        ::ioctl(touch_fd_, EVIOCGRAB, 0);
        ::close(touch_fd_);
        touch_fd_ = -1;
    }
    if (hidraw_fd_ >= 0) {
        ::close(hidraw_fd_);
        hidraw_fd_ = -1;
    }
    if (connected_.exchange(false)) {
        LOGW(kTag, "pad disconnected — gadget stays up, reports go neutral");
    }
}

bool PadInput::start() {
    if (running_.exchange(true)) return true;
    if (!open_pads()) {
        LOGW(kTag, "no DualSense evdev node found yet — will rescan");
    }
    thread_ = std::thread(&PadInput::loop, this);
    return true;
}

void PadInput::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    close_pads();
}

void PadInput::handle_gamepad_event(std::uint16_t type, std::uint16_t code, std::int32_t value) {
    if (type == EV_ABS) {
        switch (code) {
        case ABS_X:     state_.lx = static_cast<std::uint8_t>(value); break;
        case ABS_Y:     state_.ly = static_cast<std::uint8_t>(value); break;
        case ABS_RX:    state_.rx = static_cast<std::uint8_t>(value); break;
        case ABS_RY:    state_.ry = static_cast<std::uint8_t>(value); break;
        case ABS_Z:     state_.l2 = static_cast<std::uint8_t>(value); break;
        case ABS_RZ:    state_.r2 = static_cast<std::uint8_t>(value); break;
        case ABS_HAT0X: state_.hat_x = value; break;
        case ABS_HAT0Y: state_.hat_y = value; break;
        default: break;
        }
    } else if (type == EV_KEY) {
        bool pressed = (value != 0);
        switch (code) {
        case BTN_WEST:   state_.square   = pressed; break;
        case BTN_SOUTH:  state_.cross    = pressed; break;
        case BTN_EAST:   state_.circle   = pressed; break;
        case BTN_NORTH:  state_.triangle = pressed; break;
        case BTN_TL:     state_.l1       = pressed; break;
        case BTN_TR:     state_.r1       = pressed; break;
        case BTN_TL2:    state_.l2_btn   = pressed; break;
        case BTN_TR2:    state_.r2_btn   = pressed; break;
        case BTN_SELECT: state_.share    = pressed; break;
        case BTN_START:  state_.options  = pressed; break;
        case BTN_THUMBL: state_.l3       = pressed; break;
        case BTN_THUMBR: state_.r3       = pressed; break;
        case BTN_MODE:   state_.ps       = pressed; break;
        default: break;
        }
    }
}

void PadInput::handle_touchpad_event(std::uint16_t type, std::uint16_t code, std::int32_t value) {
    if (type == EV_ABS) {
        switch (code) {
        case ABS_MT_POSITION_X:
            state_.touch_x = static_cast<std::uint16_t>(value & 0x0FFF);
            break;
        case ABS_MT_POSITION_Y:
            state_.touch_y = static_cast<std::uint16_t>(value & 0x0FFF);
            break;
        case ABS_MT_TRACKING_ID:
            state_.touch_active = (value >= 0);
            if (value >= 0) state_.touch_id = static_cast<std::uint8_t>(value & 0x7F);
            break;
        default: break;
        }
    } else if (type == EV_KEY && code == BTN_TOUCH) {
        // Press-down edge also marks the click bit (byte 7 bit 1) — the
        // daemon wires the physical touchpad click the same way.
        state_.touchpad = (value != 0);
    }
}

void PadInput::loop() {
    LOGI(kTag, "reader thread started");
    std::uint8_t rpt[64];

    while (running_) {
        if (!connected_ && !open_pads()) {
            ::sleep(2); // hot-plug rescan cadence
            continue;
        }

        struct pollfd pfds[2];
        int n = 0;
        if (pad_fd_ >= 0)  pfds[n++] = {pad_fd_, POLLIN, 0};
        if (touch_fd_ >= 0) pfds[n++] = {touch_fd_, POLLIN, 0};
        if (n == 0) { ::sleep(1); continue; }

        int rc = ::poll(pfds, n, 500);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOGE(kTag, "poll: %s", std::strerror(errno));
            close_pads();
            continue;
        }
        if (rc == 0) continue;

        bool err_pad = false, err_touch = false;
        for (int i = 0; i < n; ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            bool is_touch = (pfds[i].fd == touch_fd_);

            struct input_event ev{};
            for (;;) { // drain buffered events
                ssize_t r = ::read(pfds[i].fd, &ev, sizeof(ev));
                if (r == static_cast<ssize_t>(sizeof(ev))) {
                    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        // Report on SYN from EITHER node — touchpad motion
                        // must flow even when the gamepad is idle (delta vs
                        // the daemon, which only flushed on gamepad SYN).
                        ser_.pack(state_, rpt);
                        ++reports_;
                        if (on_report_) on_report_(rpt);
                    } else if (is_touch) {
                        handle_touchpad_event(ev.type, ev.code, ev.value);
                    } else {
                        handle_gamepad_event(ev.type, ev.code, ev.value);
                    }
                    continue;
                }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (r < 0 && errno == EINTR) continue;
                // EOF (r==0) or hard error — the pad is gone.
                LOGW(kTag, "read %s: %zd (%s) — pad lost",
                     is_touch ? touch_path_.c_str() : pad_path_.c_str(),
                     r, r < 0 ? std::strerror(errno) : "eof");
                if (is_touch) err_touch = true; else err_pad = true;
                break;
            }
        }

        if (err_pad) { close_pads(); continue; } // drop both, rescan
        if (err_touch && touch_fd_ >= 0) {
            ::ioctl(touch_fd_, EVIOCGRAB, 0);
            ::close(touch_fd_);
            touch_fd_ = -1; // gamepad half stays; touch block goes inactive
            LOGW(kTag, "touchpad node lost — touch reports inactive");
        }
    }
    LOGI(kTag, "reader thread stopped (reports=%llu)",
         static_cast<unsigned long long>(reports_.load()));
}

} // namespace pds::gadget
