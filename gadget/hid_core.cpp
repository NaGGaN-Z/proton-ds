// proton-ds gadget — HID core implementation.
//
// ep0 dispatch semantics follow the SenseShock-proven pattern (their
// handle_setup_request), with answers sourced from the ds4linux daemon:
//   GET_DESCRIPTOR(report) → 507B real / 467B ViGEm (--descriptor)
//   GET_REPORT 0x02 → 37B synthesized calibration (daemon semantics)
//   GET_REPORT 0x12 → 16B paired-MAC    (serial-derived, LE order)
//   GET_REPORT 0x81 → 7B  bdaddr         (serial-derived, LE order)
//   GET_REPORT 0xA3 → 49B firmware info (version 0x3100 @ +0x23 — Detroit gate)
//   SET_REPORT     → data read + on_output callback (T5 consumes)
//   SET_IDLE       → SenseShock ack hack (0-len read + 1-byte write)

#include "hid_core.h"

#include "descriptors.h"
#include "log.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace pds::gadget {

namespace {
constexpr const char* kTag = "hid";
} // namespace

void HidCore::init(bool use_vigem_descriptor, const std::string& serial_hex,
                   Counters* counters, OutputCb on_output) {
    if (use_vigem_descriptor) {
        desc_ = kDs4ReportDescVigem;
        desc_size_ = sizeof(kDs4ReportDescVigem);
    } else {
        desc_ = kDs4ReportDescReal;
        desc_size_ = sizeof(kDs4ReportDescReal);
    }
    for (int i = 0; i < 6; ++i)
        mac_[i] = static_cast<std::uint8_t>(std::stoul(serial_hex.substr(i * 2, 2), nullptr, 16));
    counters_ = counters;
    on_output_ = std::move(on_output);
    LOGI(kTag, "init: descriptor=%s (%zu B) mac=%02x:%02x:%02x:%02x:%02x:%02x",
         use_vigem_descriptor ? "vigem" : "real", desc_size_,
         mac_[0], mac_[1], mac_[2], mac_[3], mac_[4], mac_[5]);
}

std::vector<std::uint8_t> HidCore::build_feature(std::uint8_t rnum) const {
    std::vector<std::uint8_t> out;
    switch (rnum) {
        case 0x02: {
            // Calibration (37 B) — neutral biases, unity scale: hid-sony probes
            // this at bind time and refuses to initialise on garbage.
            out.resize(37, 0);
            out[0] = 0x02;
            auto put = [&out](std::size_t off, std::int16_t v) {
                out[off] = static_cast<std::uint8_t>(v & 0xFF);
                out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            };
            constexpr std::int16_t pos = 8192, neg = -8192;
            put(7, pos);   put(9, neg);
            put(11, pos);  put(13, neg);
            put(15, pos);  put(17, neg);
            put(19, pos);  put(21, pos);
            put(23, pos);  put(25, neg);
            put(27, pos);  put(29, neg);
            put(31, pos);  put(33, neg);
            break;
        }
        case 0x12: { // paired-device/link info (16 B)
            // ZERO MAC, deliberately. Bluez's sixaxis plugin reads this
            // report for USB-paired DS4s and feeds the address into the
            // bluetooth adapter — with a real-looking MAC it starts a
            // pairing session against the gadget, which deadlocks the
            // kernel BT stack on repeated stack cycles (freeze incident
            // 2026-08-18, journal-proven: "sixaxis: setting up new device"
            // seconds before each lockup). An all-zero address makes the
            // plugin bail out harmlessly ("failed to read device address").
            // Wine/libScePad gates on the 0x12 transfer SUCCEEDING, not on
            // the MAC value — verified: Detroit + gameless verify passed
            // with a MAC here, and zero is equally opaque to them.
            out.assign(16, 0);
            out[0] = 0x12;
            break;
        }
        case 0x81: { // Bluetooth bdaddr (7 B): ID + MAC in LE order
            out.assign(7, 0);
            out[0] = 0x81;
            for (int i = 0; i < 6; ++i) out[1 + i] = mac_[5 - i];
            break;
        }
        case 0xA3: { // firmware info (49 B) — ver 0x3100 @ +0x23
            out.assign(kA3FirmwareInfo, kA3FirmwareInfo + sizeof(kA3FirmwareInfo));
            break;
        }
        default: { // unknown: minimal zeroed answer carrying the report ID
            out.assign(64, 0);
            out[0] = rnum;
            break;
        }
    }
    return out;
}

void HidCore::handle_setup(int ep0_fd, const std::uint8_t setup[8]) {
    const std::uint8_t rt = setup[0], req = setup[1];
    const std::uint16_t wValue = setup[2] | (setup[3] << 8);
    const std::uint16_t wIndex = setup[4] | (setup[5] << 8);
    const std::uint16_t wLength = setup[6] | (setup[7] << 8);
    LOGD(kTag, "ep0 SETUP rt=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u",
         rt, req, wValue, wIndex, wLength);

    const bool dir_in = rt & 0x80;

    if (rt == 0x81 && req == 0x06 && (wValue >> 8) == 0x22) {
        // GET_DESCRIPTOR(report) — the make-or-break answer for hid-sony binding
        std::size_t n = desc_size_ < wLength ? desc_size_ : wLength;
        ssize_t w = ::write(ep0_fd, desc_, n);
        LOGD(kTag, "GET_DESCRIPTOR(report): sent %zd/%zu B", w, n);
        return;
    }

    if (rt == 0xA1 && req == 0x01) { // GET_REPORT (class|interface|IN)
        std::uint8_t rnum = wValue & 0xFF;
        auto blob = build_feature(rnum);
        std::size_t n = blob.size() < wLength ? blob.size() : wLength;
        ssize_t w = ::write(ep0_fd, blob.data(), n);
        LOGD(kTag, "GET_REPORT(0x%02x): built %zu B, sent %zd", rnum, blob.size(), w);
        return;
    }

    if (rt == 0x21 && req == 0x0A) { // SET_IDLE — SenseShock-proven ack
        std::uint8_t scratch[4];
        ssize_t r = ::read(ep0_fd, scratch, 0);
        std::uint8_t zero = 0;
        ssize_t w = ::write(ep0_fd, &zero, 1);
        LOGD(kTag, "SET_IDLE: ack (read=%zd write=%zd)", r, w);
        return;
    }

    if (rt == 0x21 && req == 0x01) { // SET_REPORT — OUT data stage
        std::vector<std::uint8_t> buf(wLength ? wLength : 1);
        ssize_t r = ::read(ep0_fd, buf.data(), wLength);
        if (r > 0) {
            LOGD(kTag, "SET_REPORT: %zd B (id=0x%02x)", r, buf[0]);
            if (on_output_) on_output_(buf.data(), static_cast<std::size_t>(r));
        } else {
            LOGW(kTag, "SET_REPORT: data read failed: %s", std::strerror(errno));
        }
        return;
    }

    if (rt == 0xA1 && req == 0x02) { // GET_IDLE
        std::uint8_t idle = 0;
        ::write(ep0_fd, &idle, 1);
        LOGD(kTag, "GET_IDLE: 0");
        return;
    }

    if (rt == 0xA1 && req == 0x03) { // GET_PROTOCOL → report protocol
        std::uint8_t proto = 1;
        ::write(ep0_fd, &proto, 1);
        LOGD(kTag, "GET_PROTOCOL: report(1)");
        return;
    }

    // Unknown request — complete the transfer to avoid a host-visible stall.
    LOGW(kTag, "unhandled SETUP rt=0x%02x req=0x%02x — zero answer", rt, req);
    if (dir_in) {
        std::uint8_t zero = 0;
        ::write(ep0_fd, &zero, 1);
    } else if (wLength) {
        std::vector<std::uint8_t> buf(wLength);
        ::read(ep0_fd, buf.data(), wLength);
    }
}

bool HidCore::open_endpoints(const std::string& ffs_mount) {
    std::string ep1 = ffs_mount + "/ep1", ep2 = ffs_mount + "/ep2";
    for (int i = 0; i < 20; ++i) {
        // O_NONBLOCK: a blocking read(ep2) cannot be interrupted by close()
        // from stop_threads() — the reader would hang the daemon in join()
        // forever (T3 smoke: stop timed out exactly this way).
        ep1_fd_ = ::open(ep1.c_str(), O_RDWR | O_NONBLOCK);
        if (ep1_fd_ >= 0) break;
        ::usleep(100000);
    }
    if (ep1_fd_ < 0) {
        LOGE(kTag, "open %s: %s", ep1.c_str(), std::strerror(errno));
        return false;
    }
    ep2_fd_ = ::open(ep2.c_str(), O_RDWR | O_NONBLOCK);
    if (ep2_fd_ < 0) {
        LOGE(kTag, "open %s: %s", ep2.c_str(), std::strerror(errno));
        ::close(ep1_fd_); ep1_fd_ = -1;
        return false;
    }
    LOGI(kTag, "endpoints open: ep1(fd=%d) ep2(fd=%d)", ep1_fd_, ep2_fd_);
    return true;
}

void HidCore::writer_loop() {
    LOGI(kTag, "ep1 writer thread started");
    // EAGAIN backoff: before the host configures the interface (ep0 ENABLE
    // can lag the first reports by ~150 ms) ep1 writes return EAGAIN. That
    // is NOT fatal — the writer must wait out the host, not exit (T4 smoke:
    // a report arrived pre-ENABLE, the writer died after 10 ms of retries,
    // and every subsequent report was dropped).
    int backoff_us = 1000;
    while (io_running_) {
        std::vector<std::uint8_t> rpt;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait_for(lk, std::chrono::milliseconds(200),
                          [&] { return !queue_.empty() || !io_running_; });
            if (!io_running_) break;
            if (queue_.empty()) continue;
            rpt = std::move(queue_.front());
            queue_.pop_front();
        }
        bool sent = false;
        while (io_running_ && !sent) {
            ssize_t w = ::write(ep1_fd_, rpt.data(), rpt.size());
            if (w > 0) {
                sent = true;
                if (counters_) ++counters_->ep1_writes;
                backoff_us = 1000; // reset after success
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ::usleep(backoff_us);
                backoff_us = backoff_us < 50000 ? backoff_us * 2 : 50000;
                continue;
            }
            // ESHUTDOWN/EIO/EBADF: the gadget is gone — nothing to write to.
            LOGE(kTag, "ep1 write failed: %s — writer exiting", std::strerror(errno));
            break;
        }
        if (!sent && io_running_) {
            // EAGAIN-wait was interrupted by shutdown, or hard error: drop the
            // report (the next SYN produces a fresher one anyway).
            LOGD(kTag, "ep1 report dropped (pre-ENABLE or error path)");
        }
    }
    {
        std::lock_guard<std::mutex> lk(jm_);
        writer_done_ = true;
    }
    jcv_.notify_all();
    LOGI(kTag, "ep1 writer thread stopped");
}

void HidCore::reader_loop() {
    LOGI(kTag, "ep2 reader thread started");
    std::uint8_t buf[64];
    while (io_running_) {
        struct pollfd pfd{ep2_fd_, POLLIN, 0};
        int prc = ::poll(&pfd, 1, 200);
        if (prc < 0) {
            if (errno == EINTR) continue;
            LOGW(kTag, "ep2 poll: %s — reader exiting", std::strerror(errno));
            break;
        }
        if (prc == 0) continue; // timeout: re-check io_running_
        ssize_t n = ::read(ep2_fd_, buf, sizeof(buf));
        if (n > 0) {
            if (counters_) ++counters_->ep2_reads;
            LOGD(kTag, "ep2 report: %zd B (id=0x%02x)", n, buf[0]);
            if (on_output_) on_output_(buf, static_cast<std::size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            LOGW(kTag, "ep2 read: %zd (%s) — reader exiting", n,
                 n < 0 ? std::strerror(errno) : "eof");
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(jm_);
        reader_done_ = true;
    }
    jcv_.notify_all();
    LOGI(kTag, "ep2 reader thread stopped");
}

void HidCore::start_threads() {
    io_running_ = true;
    writer_done_ = false;
    reader_done_ = false;
    writer_ = std::thread(&HidCore::writer_loop, this);
    reader_ = std::thread(&HidCore::reader_loop, this);
}

void HidCore::stop_threads() {
    io_running_ = false;
    qcv_.notify_all();

    // Bounded join: a thread parked in an uninterruptible kernel wait must
    // never hang the daemon's shutdown. After UDC unbind endpoint IO returns
    // errors, so both threads exit promptly; if one still refuses within the
    // deadline we detach it (process exit reclaims its fds).
    {
        std::unique_lock<std::mutex> lk(jm_);
        if (!jcv_.wait_for(lk, std::chrono::seconds(3), [&] {
                return writer_done_ && reader_done_;
            })) {
            LOGE(kTag, "io threads did not exit within 3s — detaching stuck thread(s)");
            if (writer_.joinable()) writer_.detach();
            if (reader_.joinable()) reader_.detach();
        } else {
            if (writer_.joinable()) writer_.join();
            if (reader_.joinable()) reader_.join();
        }
    }
    if (ep1_fd_ >= 0) ::close(ep1_fd_);
    if (ep2_fd_ >= 0) ::close(ep2_fd_);
    ep1_fd_ = ep2_fd_ = -1;
    LOGI(kTag, "io threads joined");
}

void HidCore::queue_report(const std::uint8_t rpt[64]) {
    {
        std::lock_guard<std::mutex> lk(qm_);
        if (queue_.size() >= 8) {
            queue_.pop_front(); // drop-oldest: latest state wins
            if (++dropped_ % 100 == 1)
                LOGW(kTag, "ep1 queue overflow: %llu reports dropped so far",
                     static_cast<unsigned long long>(dropped_));
        }
        queue_.emplace_back(rpt, rpt + 64);
    }
    qcv_.notify_one();
}

} // namespace pds::gadget
