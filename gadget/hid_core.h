#pragma once
// proton-ds gadget — HID core: ep0 SETUP dispatcher + ep1/ep2 endpoint threads.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pds::gadget {

struct Counters {
    std::uint64_t ep0_events = 0, ep0_setup = 0, ep1_writes = 0, ep2_reads = 0;
};

class HidCore {
public:
    // ep2 OUT reports (rumble/lightbar and feature SET_REPORT payloads).
    // Called from the reader thread — must be cheap or re-post.
    using OutputCb = std::function<void(const std::uint8_t*, std::size_t)>;

    void init(bool use_vigem_descriptor, const std::string& serial_hex,
              Counters* counters, OutputCb on_output);

    // Dispatch one FUNCTIONFS_SETUP event (setup[8] = struct usb_ctrlrequest).
    // Completes the control transfer on ep0_fd (write for IN, read for OUT).
    void handle_setup(int ep0_fd, const std::uint8_t setup[8]);

    // Open ep1/ep2 after ffs descriptors are accepted. Retries briefly.
    bool open_endpoints(const std::string& ffs_mount);

    void start_threads();
    void stop_threads(); // joins; closes endpoint fds (must run before ffs umount)

    // Queue one 64-byte input report (report ID 0x01) for ep1.
    // Latest-wins semantics: drops the oldest when the queue overflows.
    void queue_report(const std::uint8_t rpt[64]);

    const std::uint8_t* report_descriptor() const { return desc_; }
    std::size_t report_descriptor_size() const { return desc_size_; }

private:
    void writer_loop();
    void reader_loop();
    std::vector<std::uint8_t> build_feature(std::uint8_t rnum) const;

    const std::uint8_t* desc_ = nullptr;
    std::size_t desc_size_ = 0;
    std::uint8_t mac_[6]{};
    Counters* counters_ = nullptr;
    OutputCb on_output_;

    int ep1_fd_ = -1, ep2_fd_ = -1;
    std::thread writer_, reader_;
    std::atomic<bool> io_running_{false};
    std::atomic<bool> writer_done_{false}, reader_done_{false};
    std::mutex jm_;
    std::condition_variable jcv_;

    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::uint64_t dropped_ = 0;
};

} // namespace pds::gadget
