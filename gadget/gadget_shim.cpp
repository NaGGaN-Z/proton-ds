// proton-ds gadget-shim — USB gadget lifecycle owner (E3 path).
// start|stop|status; configfs + FunctionFS + UDC bind; ordered teardown.
// T2 skeleton: ep0 event loop is a stub (no SETUP answers yet — T3).

#include "log.h"
#include "descriptors.h"
#include "hid_core.h"
#include "pad_input.h"
#include "bridge.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTag = "shim";
constexpr const char* kGadgetName = "pds4";
constexpr const char* kConfigfsRoot = "/sys/kernel/config/usb_gadget";
constexpr const char* kFfsMount = "/dev/ffs-pds4";
constexpr const char* kPidFile = "/run/gadget-shim.pid";
constexpr const char* kStatusFile = "/run/gadget-shim.status";
constexpr const char* kLogFile = "/var/log/gadget-shim.log";

constexpr const char* kVid = "0x054c";
constexpr const char* kPid09cc = "0x09cc";
constexpr const char* kBcdDevice = "0x0100";
constexpr const char* kBcdUsb = "0x0200";
constexpr const char* kManufacturer = "Sony Interactive Entertainment";
constexpr const char* kProduct = "Wireless Controller";

constexpr int kExitOk = 0;
constexpr int kExitUsageIo = 2;
constexpr int kExitAlreadyStarted = 10;
constexpr int kExitAlreadyStopped = 11;

// include/uapi/linux/usb/functionfs.h: FUNCTIONFS_DESCRIPTORS_MAGIC_V2 = 3,
// FUNCTIONFS_STRINGS_MAGIC = 2
constexpr std::uint32_t kDescsMagicV2 = 3;
constexpr std::uint32_t kStringsMagic = 2;
constexpr std::uint32_t kHasFsDesc = 1; // FUNCTIONFS_HAS_FS_DESC
constexpr std::uint32_t kHasHsDesc = 2; // FUNCTIONFS_HAS_HS_DESC

using pds::gadget::Counters;

// struct usb_functionfs_event: usb_ctrlrequest u (8 B) + type + pad[3]
struct FfsEvent {
    std::uint8_t setup[8];
    std::uint8_t type;
    std::uint8_t pad[3];
};
static_assert(sizeof(FfsEvent) == 12);

// enum usb_functionfs_event_type
constexpr std::uint8_t kEvBind = 0, kEvUnbind = 1, kEvEnable = 2, kEvDisable = 3,
                          kEvSetup = 4, kEvSuspend = 5, kEvResume = 6;

const char* ev_name(std::uint8_t t) {
    switch (t) {
        case kEvBind: return "BIND";
        case kEvUnbind: return "UNBIND";
        case kEvEnable: return "ENABLE";
        case kEvDisable: return "DISABLE";
        case kEvSetup: return "SETUP";
        case kEvSuspend: return "SUSPEND";
        case kEvResume: return "RESUME";
    }
    return "?";
}

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
    std::string serial = "aabbccddeeff";
    std::string descriptor = "real";
    std::string udc;
    bool bridge = false;
    std::string bridge_socket = "/run/ds4linux-bridge.sock";
};

std::string err_str(int e) { return std::string(std::strerror(e)); }

bool write_file(const fs::path& p, const std::string& data, std::string& why) {
    std::ofstream f(p, std::ios::binary);
    if (!f) { why = "open failed: " + p.string() + ": " + err_str(errno); return false; }
    f << data;
    f.flush();
    if (!f) {
        int e = errno; // capture before any other call clobbers it
        why = "write failed: " + p.string() + ": errno=" + std::to_string(e)
            + " (" + err_str(e) + ")";
        return false;
    }
    LOGD(kTag, "configfs write %s <= '%s'", p.c_str(), data.c_str());
    return true;
}

bool read_first_line(const fs::path& p, std::string& out) {
    std::ifstream f(p);
    if (!f) return false;
    std::getline(f, out);
    return true;
}

std::optional<pid_t> running_pid() {
    std::string s;
    if (!read_first_line(kPidFile, s) || s.empty()) return std::nullopt;
    pid_t pid = std::atoi(s.c_str());
    if (pid <= 0) return std::nullopt;
    if (::kill(pid, 0) < 0 && errno == ESRCH) return std::nullopt;
    return pid;
}

std::string normalize_serial(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == ':' || c == '-') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void usage(std::FILE* out) {
    std::fprintf(out,
        "usage: gadget-shim start [--serial <MAC>] [--descriptor real|vigem] [--udc <name>]\n"
        "                        [--bridge [--bridge-socket <path>]]\n"
        "       gadget-shim stop\n"
        "       gadget-shim status\n"
        "--bridge: daemon owns the pad; input streams from /run/ds4linux-bridge.sock\n"
        "exit codes: 0 ok, 2 usage/IO, 10 already started, 11 already stopped\n");
}

bool gate_modules(std::string& why) {
    for (const char* mod : {"dummy_hcd", "libcomposite", "usb_f_fs"}) {
        LOGD(kTag, "modprobe %s", mod);
        int rc = ::system((std::string("/usr/bin/modprobe ") + mod + " >/dev/null 2>&1").c_str());
        (void)rc;
        fs::path sysmod = std::string("/sys/module/") + mod;
        std::error_code ec;
        if (!fs::exists(sysmod, ec)) {
            why = std::string("kernel module '") + mod + "' not available";
            return false;
        }
        LOGD(kTag, "module gate ok: %s", mod);
    }
    return true;
}

bool ensure_configfs(std::string& why) {
    std::error_code ec;
    if (fs::exists("/sys/kernel/config", ec)) return true;
    LOGD(kTag, "configfs absent; mounting");
    if (::mkdir("/sys/kernel/config", 0755) < 0 && errno != EEXIST) {
        why = "mkdir /sys/kernel/config: " + err_str(errno);
        return false;
    }
    if (::mount("none", "/sys/kernel/config", "configfs", 0, nullptr) < 0) {
        why = std::string("mount configfs: ") + err_str(errno);
        return false;
    }
    return true;
}

std::optional<std::string> pick_udc(const std::string& override_name) {
    if (!override_name.empty()) {
        std::error_code ec;
        if (!fs::exists(fs::path("/sys/class/udc") / override_name, ec)) {
            return std::nullopt;
        }
        return override_name;
    }
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/sys/class/udc", ec)) {
        return e.path().filename().string();
    }
    return std::nullopt;
}

std::vector<std::uint8_t> placeholder_fs_descriptors(std::size_t report_desc_size) {
    // Function list: interface -> HID class -> ep1 -> ep2 (no config descriptor —
    // functionfs generates it; kernel Documentation/usb/functionfs.c).
    // HID class descriptor carries wDescriptorLength of the chosen report
    // descriptor (real 507 / ViGEm 467) — that length is what hid-sony parses.
    const std::uint8_t lo = report_desc_size & 0xFF, hi = report_desc_size >> 8;
    static std::vector<std::uint8_t> cache; // rebuilt per call; sizes differ
    cache = {
        // interface: HID class, 2 endpoints
        0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
        // HID class: bcdHID 1.11, 1 report descriptor of REPORT type
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, lo, hi,
        // ep1 IN interrupt 64B, interval 4 (SenseShock-proven timing)
        0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x04,
        // ep2 OUT interrupt 64B
        0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x04,
    };
    return cache;
}

bool write_ffs_descriptors(int ep0_fd, bool vigem, std::string& why) {
    const std::size_t rd_size = vigem ? sizeof(pds::gadget::kDs4ReportDescVigem)
                                      : sizeof(pds::gadget::kDs4ReportDescReal);
    auto blob = placeholder_fs_descriptors(rd_size);

    // v2 frame (SenseShock-proven): [magic][length][flags][fs_count][hs_count]
    //                                [fs descriptors...][hs descriptors...]
    struct Head {
        std::uint32_t magic;
        std::uint32_t length;
        std::uint32_t flags;
        std::uint32_t fs_count;
        std::uint32_t hs_count;
    } head{};
    head.magic = kDescsMagicV2;
    head.flags = kHasFsDesc | kHasHsDesc;
    head.fs_count = 4; // iface + HID class + ep1 + ep2
    head.hs_count = 4;
    head.length = static_cast<std::uint32_t>(sizeof(Head) + 2 * blob.size());

    std::vector<std::uint8_t> frame;
    frame.insert(frame.end(), reinterpret_cast<std::uint8_t*>(&head),
                 reinterpret_cast<std::uint8_t*>(&head) + sizeof(head));
    frame.insert(frame.end(), blob.begin(), blob.end()); // FS set
    frame.insert(frame.end(), blob.begin(), blob.end()); // HS set (same shape)

    ssize_t n = ::write(ep0_fd, frame.data(), frame.size());
    if (n != static_cast<ssize_t>(frame.size())) {
        why = "ffs descriptors write: " + err_str(errno);
        return false;
    }
    LOGD(kTag, "ffs descriptors written: %zu bytes (placeholder; real blob in T3)", frame.size());
    return true;
}

bool write_ffs_strings(int ep0_fd, std::string& why) {
    // Mandatory for ffs readiness (SenseShock-proven): str_count=1, lang 0x0409.
    static const char kIfaceStr[] = "Source/Sink";
    struct StringsFrame {
        std::uint32_t magic;
        std::uint32_t length;
        std::uint32_t str_count;
        std::uint32_t lang_count;
        std::uint16_t lang;
        char str[sizeof(kIfaceStr)];
    } f{};
    f.magic = kStringsMagic;
    f.str_count = 1;
    f.lang_count = 1;
    f.lang = 0x0409; // en-us
    ::memcpy(f.str, kIfaceStr, sizeof(kIfaceStr));
    f.length = sizeof(f);

    ssize_t n = ::write(ep0_fd, &f, sizeof(f));
    if (n != static_cast<ssize_t>(sizeof(f))) {
        why = "ffs strings write: " + err_str(errno);
        return false;
    }
    LOGD(kTag, "ffs strings written: %zu bytes", sizeof(f));
    return true;
}

struct CountersRemoved {}; // Counters lives in hid_core.h now

void write_status_file(const Counters& c, const std::string& udc, std::int64_t start_ms,
                       const pds::gadget::PadInput* pad) {
    std::ofstream f(kStatusFile, std::ios::trunc);
    if (!f) return;
    std::int64_t up_s = 0;
    std::timespec now{};
    ::timespec_get(&now, TIME_UTC);
    up_s = (now.tv_sec * 1000 + now.tv_nsec / 1000000 - start_ms) / 1000;
    f << "pid=" << ::getpid() << " uptime_s=" << up_s << " udc=" << udc << "\n";
    f << "ep0_events=" << c.ep0_events << " ep0_setup=" << c.ep0_setup
      << " ep1_writes=" << c.ep1_writes << " ep2_reads=" << c.ep2_reads << "\n";
    if (pad) {
        f << "pad=" << (pad->pad_connected() ? "connected" : "absent")
          << " reports=" << pad->reports() << "\n";
    }
}

bool bring_up(const Options& opt, std::string& udc_out, int& ep0_out, std::string& why) {
    fs::path g = fs::path(kConfigfsRoot) / kGadgetName;

    if (fs::exists(g)) {
        why = "gadget configfs dir already exists: " + g.string();
        return false;
    }
    std::error_code ec;
    if (!fs::create_directories(g, ec)) {
        why = "mkdir " + g.string() + ": " + ec.message();
        return false;
    }

    for (auto& [name, val] : std::vector<std::pair<const char*, std::string>>{
             {"idVendor", kVid}, {"idProduct", kPid09cc},
             {"bcdDevice", kBcdDevice}, {"bcdUSB", kBcdUsb}}) {
        if (!write_file(g / name, val + "\n", why)) return false;
    }

    fs::path strings = g / "strings" / "0x409";
    if (!fs::create_directories(strings, ec)) {
        why = "mkdir " + strings.string() + ": " + ec.message();
        return false;
    }
    if (!write_file(strings / "manufacturer", std::string(kManufacturer) + "\n", why)) return false;
    if (!write_file(strings / "product", std::string(kProduct) + "\n", why)) return false;
    if (!write_file(strings / "serialnumber", opt.serial + "\n", why)) return false;

    fs::path func = g / "functions" / "ffs.pds4";
    if (!fs::create_directories(func, ec)) {
        why = "mkdir " + func.string() + ": " + ec.message();
        return false;
    }

    fs::path cfg = g / "configs" / "c.1";
    if (!fs::create_directories(cfg, ec)) {
        why = "mkdir " + cfg.string() + ": " + ec.message();
        return false;
    }
    if (!write_file(cfg / "MaxPower", "250\n", why)) return false;
    if (!write_file(cfg / "bmAttributes", "0x80\n", why)) return false;

    fs::path link = cfg / "ffs.pds4";
    fs::create_symlink(func, link, ec);
    if (ec) {
        why = "symlink " + link.string() + " -> " + func.string() + ": " + ec.message();
        return false;
    }

    // Stale-mount guard: a leftover functionfs mount (from a crashed run)
    // would make mount() stack a second layer on top of a dead instance —
    // the exact g99 failure mode. Refuse instead.
    {
        std::error_code e2;
        if (fs::exists(fs::path(kFfsMount) / "ep0", e2)) {
            LOGW(kTag, "stale ffs mount detected at %s — attempting cleanup", kFfsMount);
            for (int i = 0; i < 5; ++i) {
                if (::umount(kFfsMount) == 0 || errno == EINVAL) break;
                ::usleep(200000);
            }
            if (fs::exists(fs::path(kFfsMount) / "ep0", e2)) {
                why = std::string("stale functionfs mount at ") + kFfsMount
                    + " (ep0 present, umount failed)";
                return false;
            }
            fs::remove(fs::path(kFfsMount), e2); // drop empty mountpoint dir
        }
    }

    if (!fs::exists(kFfsMount, ec) && ::mkdir(kFfsMount, 0755) < 0 && errno != EEXIST) {
        why = std::string("mkdir ") + kFfsMount + ": " + err_str(errno);
        return false;
    }
    if (::mount(kGadgetName, kFfsMount, "functionfs", 0, nullptr) < 0) {
        if (errno != EBUSY) {
            why = std::string("mount functionfs: ") + err_str(errno);
            return false;
        }
        LOGW(kTag, "functionfs already mounted at %s (continuing)", kFfsMount);
    } else {
        LOGD(kTag, "functionfs mounted at %s", kFfsMount);
    }

    fs::path ep0p = fs::path(kFfsMount) / "ep0";
    for (int i = 0; i < 50 && !fs::exists(ep0p, ec); ++i) {
        ::usleep(100000);
    }
    int ep0 = ::open(ep0p.c_str(), O_RDWR);
    if (ep0 < 0) {
        why = "open " + ep0p.string() + ": " + err_str(errno);
        return false;
    }
    LOGD(kTag, "ep0 opened (fd=%d)", ep0);

    if (!write_ffs_descriptors(ep0, opt.descriptor == "vigem", why)) return false;
    if (!write_ffs_strings(ep0, why)) return false;

    auto udc = pick_udc(opt.udc);
    if (!udc) {
        why = "no free UDC in /sys/class/udc (load dummy_hcd?)";
        return false;
    }
    // ffs readiness races the UDC bind: retry a few times (EINVAL until ready)
    bool bound = false;
    for (int i = 0; i < 10 && !bound; ++i) {
        if (write_file(g / "UDC", *udc + "\n", why)) { bound = true; break; }
        LOGD(kTag, "UDC bind attempt %d failed (%s), retrying", i + 1, why.c_str());
        ::usleep(200000);
    }
    if (!bound) return false;
    LOGI(kTag, "gadget UP: %s:%s bcdDevice=%s serial=%s udc=%s descriptor=%s",
         kVid, kPid09cc, kBcdDevice, opt.serial.c_str(), udc->c_str(), opt.descriptor.c_str());

    udc_out = *udc;
    ep0_out = ep0;
    return true;
}

int teardown_unbind() {
    // UDC unbind FIRST, before joining IO threads: unbind kills all pending
    // endpoint transfers in the kernel, which unblocks a reader parked inside
    // ffs_epfile_io (T3 smoke: join hung exactly there). Returns failures.
    fs::path g = fs::path(kConfigfsRoot) / kGadgetName;
    std::error_code ec;
    int fails = 0;
    if (fs::exists(g / "UDC", ec)) {
        std::ofstream f(g / "UDC");
        f << "\n";
        if (!f) { ++fails; LOGE(kTag, "teardown: UDC unbind write failed"); }
        else LOGD(kTag, "teardown step ok: udc unbind");
    }
    return fails;
}

int teardown(bool report_ok) {
    fs::path g = fs::path(kConfigfsRoot) / kGadgetName;
    int fails = 0;
    std::error_code ec;

    auto step = [&](const char* name, auto&& fn) {
        try {
            fn();
            LOGD(kTag, "teardown step ok: %s", name);
        } catch (const std::exception& e) {
            ++fails;
            LOGE(kTag, "teardown step FAILED: %s: %s", name, e.what());
        }
    };

    if (fs::exists(g / "UDC", ec)) {
        step("udc unbind", [&] {
            std::ofstream f(g / "UDC");
            f << "\n";
            if (!f) throw std::runtime_error("write UDC unbind failed");
        });
    } else {
        LOGD(kTag, "teardown step skipped: udc (already unbound)");
    }
    step("umount ffs", [&] {
        std::string err;
        bool ok = false;
        for (int i = 0; i < 10; ++i) {
            if (::umount(kFfsMount) == 0) { ok = true; break; }
            err = err_str(errno);
            // EINVAL/ENOENT = nothing mounted at that path (the daemon's own
            // teardown may have beaten us to it) — treat as success.
            if (errno == EINVAL || errno == ENOENT) { ok = true; break; }
            // ffs keeps the mount busy while the function instance exists:
            // drop the config link + function dir and retry (lesson: g99).
            std::error_code e2;
            fs::remove(g / "configs" / "c.1" / "ffs.pds4", e2);
            fs::remove(g / "functions" / "ffs.pds4", e2);
            ::usleep(300000);
        }
        if (!ok) throw std::runtime_error(err);
    });
    step("rmdir ffs mountpoint", [&] {
        std::error_code e2;
        fs::remove(fs::path(kFfsMount), e2);
        if (e2) throw std::runtime_error("remove: " + e2.message());
        if (fs::exists(fs::path(kFfsMount))) throw std::runtime_error("still exists after remove");
    });
    step("rm config symlink", [&] {
        std::error_code e2;
        fs::remove(g / "configs" / "c.1" / "ffs.pds4", e2);
        if (e2) throw std::runtime_error(e2.message());
    });
    for (const auto& p : {g / "functions" / "ffs.pds4", g / "configs" / "c.1",
                          g / "strings" / "0x409", g}) {
        step(("rmdir " + p.string()).c_str(), [&] {
            std::error_code e2;
            fs::remove(p, e2);
            std::string msg = e2.message();
            if (fs::exists(p)) throw std::runtime_error(msg.empty() ? "still exists" : msg);
        });
    }
    ::unlink(kPidFile);
    ::unlink(kStatusFile);

    if (report_ok) {
        if (fails == 0) LOGI(kTag, "TEARDOWN REPORT: clean (0 failures)");
        else LOGE(kTag, "TEARDOWN REPORT: %d failures — inspect configfs manually", fails);
    }
    return fails;
}

void serve_loop(int ep0, const std::string& udc, pds::gadget::HidCore& hid, Counters& c,
                pds::gadget::PadInput& pad) {
    std::timespec st{}    ;
    ::timespec_get(&st, TIME_UTC);
    std::int64_t start_ms = st.tv_sec * 1000 + st.tv_nsec / 1000000;
    std::int64_t last_status = 0;

    while (!g_stop) {
        struct pollfd pfd{ep0, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOGE(kTag, "poll(ep0): %s", err_str(errno).c_str());
            break;
        }
        if (rc > 0 && (pfd.revents & POLLIN)) {
            std::uint8_t buf[512];
            ssize_t n = ::read(ep0, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                LOGE(kTag, "read(ep0): %s — exiting loop", err_str(errno).c_str());
                break;
            }
            if (n == 0) {
                LOGW(kTag, "read(ep0)=0 — functionfs closed");
                break;
            }
            for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(FfsEvent)) <= n;
                 off += sizeof(FfsEvent)) {
                auto* ev = reinterpret_cast<const FfsEvent*>(buf + off);
                ++c.ep0_events;
                if (ev->type == kEvSetup) {
                    ++c.ep0_setup;
                    hid.handle_setup(ep0, ev->setup); // completes the control transfer
                } else {
                    LOGI(kTag, "ep0 event %s", ev_name(ev->type));
                }
            }
        }
        std::timespec now{};
        ::timespec_get(&now, TIME_UTC);
        std::int64_t now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms - last_status > 5000) {
            last_status = now_ms;
            write_status_file(c, udc, start_ms, &pad);
            LOGD(kTag, "status: ep0_events=%llu ep0_setup=%llu",
                 static_cast<unsigned long long>(c.ep0_events),
                 static_cast<unsigned long long>(c.ep0_setup));
        }
    }
    LOGI(kTag, "serve loop stopped (signal=%d), counters: ep0_events=%llu ep0_setup=%llu",
         static_cast<int>(g_stop), static_cast<unsigned long long>(c.ep0_events),
         static_cast<unsigned long long>(c.ep0_setup));
}

int child_run(const Options& opt, int ready_pipe_w) {
    pds::log::open_file_sink(kLogFile);
    LOGI(kTag, "=== gadget-shim start (pid=%d) serial=%s descriptor=%s udc=%s ===",
         ::getpid(), opt.serial.c_str(), opt.descriptor.c_str(),
         opt.udc.empty() ? "<auto>" : opt.udc.c_str());

    std::string why, udc;
    int ep0 = -1;
    if (!bring_up(opt, udc, ep0, why)) {
        LOGE(kTag, "bring-up FAILED: %s", why.c_str());
        LOGE(kTag, "action: see steps above; partial configfs state will be removed");
        if (ep0 >= 0) ::close(ep0); // umount fails EBUSY while ep0 is open
        teardown(false);
        char b = 'F';
        ssize_t rc = ::write(ready_pipe_w, &b, 1);
        (void)rc;
        ::close(ready_pipe_w);
        return kExitUsageIo;
    }

    {
        std::ofstream pf(kPidFile, std::ios::trunc);
        pf << ::getpid() << "\n";
    }
    char b = 'R';
    ssize_t rc = ::write(ready_pipe_w, &b, 1);
    (void)rc;
    ::close(ready_pipe_w);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    static pds::gadget::Counters counters; // outlives serve_loop for status
    pds::gadget::PadInput pad;             // standalone input source/output sink
    pds::gadget::BridgeClient bridge;      // daemon bridge (Phase 2, --bridge)
    pds::gadget::HidCore hid;

    if (opt.bridge) {
        // Bridge mode: the DAEMON owns the pad (input + output); the shim
        // only forwards. Serial/MAC arrives as PAD_INFO after connect — but
        // configfs strings need a serial NOW, so the gadget starts with the
        // given/default one (ds4ctl passes the pad MAC+1) and 0x12/0x81
        // answers already use it consistently.
        bridge.init(opt.bridge_socket,
                    /*on_input=*/[&hid](const std::uint8_t rpt[64]) { hid.queue_report(rpt); },
                    /*on_pad_info=*/[](bool usb, const std::uint8_t mac[6]) {
                        LOGI(kTag, "daemon pad: transport=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
                             usb ? "USB" : "BT", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    },
                    /*on_pad_gone=*/[]() {
                        LOGW(kTag, "daemon pad gone — gadget stays up, reports neutral");
                    });
        hid.init(opt.descriptor == "vigem", opt.serial, &counters,
                 [&bridge](const std::uint8_t* data, std::size_t len) {
                     bridge.send_output(data, len); // ep2 → daemon → pad
                 });
    } else {
        hid.init(opt.descriptor == "vigem", opt.serial, &counters,
                 [&pad](const std::uint8_t* data, std::size_t len) {
                     pad.apply_ds4_output(data, len); // ep2 → pad directly
                 });
        pad.init([&hid](const std::uint8_t rpt[64]) { hid.queue_report(rpt); });
    }

    if (!hid.open_endpoints(kFfsMount)) {
        LOGE(kTag, "endpoint open failed — stopping");
    } else {
        hid.start_threads();
        if (opt.bridge) {
            bridge.start(); // Phase 2: daemon streams INPUT frames
        } else {
            pad.start();    // Phase 1: local evdev reader
        }
        serve_loop(ep0, udc, hid, counters, pad);

        if (opt.bridge) {
            bridge.stop();
        } else {
            pad.stop(); // joins the reader before gadget teardown
        }
        // Shutdown order (kernel-lifecycle-correct): unbind the UDC first so
        // ffs endpoint IO dies and the reader thread unblocks, THEN join.
        teardown_unbind();
        hid.stop_threads(); // bounded join; closes ep1/ep2 after threads exit
    }
    ::close(ep0);
    int fails = teardown(true);
    LOGI(kTag, "=== gadget-shim exit (fails=%d) ===", fails);
    return fails == 0 ? kExitOk : kExitUsageIo;
}

int cmd_start(const Options& opt) {
    if (::geteuid() != 0) {
        std::fprintf(stderr, "ERROR: start requires root (configfs/mount/UDC)\n");
        return kExitUsageIo;
    }
    if (running_pid()) {
        std::fprintf(stderr,
            "ERROR: gadget-shim already started (pid file %s, process alive)\n"
            "  likely cause: previous start still running\n"
            "  action: run `gadget-shim stop` first\n", kPidFile);
        return kExitAlreadyStarted;
    }
    std::error_code ec;
    if (fs::exists(fs::path(kConfigfsRoot) / kGadgetName, ec)) {
        std::fprintf(stderr,
            "ERROR: orphan configfs gadget %s/%s exists but shim is not running\n"
            "  likely cause: previous shim died without teardown\n"
            "  action: run `gadget-shim stop` (it performs orphan cleanup)\n",
            kConfigfsRoot, kGadgetName);
        return kExitUsageIo;
    }

    std::string why;
    if (!gate_modules(why) || !ensure_configfs(why)) {
        std::fprintf(stderr, "ERROR: %s\n  action: install kernel modules (dummy_hcd, "
                             "libcomposite, usb_f_fs) or mount configfs manually\n",
                     why.c_str());
        return kExitUsageIo;
    }

    int pfd[2];
    if (::pipe(pfd) < 0) {
        perror("pipe");
        return kExitUsageIo;
    }
    pid_t pid = ::fork();
    if (pid < 0) { perror("fork"); return kExitUsageIo; }
    if (pid == 0) {
        ::setsid();
        ::close(pfd[0]);
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, 0); ::dup2(devnull, 1); ::dup2(devnull, 2); if (devnull > 2) ::close(devnull); }
        // All child output goes through file_sink() to /var/log/gadget-shim.log;
        // stderr must NOT stay on the invoker's pipe: the daemon outlives the
        // SSH session and a write to a dead pipe would SIGPIPE-kill it.
        ::_exit(child_run(opt, pfd[1]));
    }
    ::close(pfd[1]);
    char b = 0;
    ssize_t n = 0;
    {
        // Cap the wait: bring_up is bounded (~7 s of retries worst case);
        // a hung child must not hang the caller's session (smoke #4 lesson).
        struct pollfd pp{pfd[0], POLLIN, 0};
        int prc = ::poll(&pp, 1, 30000);
        if (prc > 0) n = ::read(pfd[0], &b, 1);
    }
    ::close(pfd[0]);
    if (n == 1 && b == 'R') {
        // Success: the child is a daemon now (setsid'd, serves forever).
        // NEVER waitpid here — it blocks until the daemon stops
        // (smoke #4 bug: start hung the caller's SSH session).
        std::fprintf(stderr, "gadget-shim started (pid %d)\n", pid);
        return kExitOk;
    }
    // Failed start: ensure the child is dead and reaped — its fds must be
    // closed before the parent's cleanup can umount ffs, and a hung child
    // must not outlive the failed start.
    ::kill(pid, SIGTERM);
    int status = 0;
    bool reaped = false;
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
        ::usleep(100000);
    }
    if (!reaped) {
        LOGW(kTag, "child %d ignored SIGTERM after failed start — SIGKILL", pid);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    // Final cleanup pass: the child's own teardown may have been unable to
    // umount ffs while still alive holding references.
    pds::log::open_file_sink(kLogFile);
    LOGI(kTag, "start failed — parent final cleanup pass");
    teardown(false);
    std::fprintf(stderr, "ERROR: gadget-shim start failed — see %s\n", kLogFile);
    return kExitUsageIo;
}

int cmd_stop() {
    if (::geteuid() != 0) {
        std::fprintf(stderr, "ERROR: stop requires root\n");
        return kExitUsageIo;
    }
    pds::log::open_file_sink(kLogFile);
    auto pid = running_pid();
    bool had_something = pid.has_value();

    if (pid) {
        LOGI(kTag, "stopping shim pid=%d (SIGTERM)", *pid);
        if (::kill(*pid, SIGTERM) < 0 && errno == ESRCH) {
            LOGW(kTag, "pid %d vanished before signal", *pid);
        } else {
            for (int i = 0; i < 100; ++i) {
                if (::kill(*pid, 0) < 0 && errno == ESRCH) break;
                ::usleep(100000);
            }
            if (::kill(*pid, 0) == 0) {
                std::fprintf(stderr,
                    "ERROR: shim pid %d did not exit within 10s\n"
                    "  likely cause: stuck in teardown (busy ffs?)\n"
                    "  action: inspect %s and /proc/%d; then re-run stop\n",
                    *pid, kLogFile, *pid);
                return kExitUsageIo;
            }
        }
    }

    std::error_code ec;
    bool dirty = fs::exists(fs::path(kConfigfsRoot) / kGadgetName, ec)
              || fs::exists(kFfsMount, ec)
              || fs::exists(kPidFile, ec) || fs::exists(kStatusFile, ec);
    if (!had_something && !dirty) {
        std::fprintf(stderr, "gadget-shim already stopped (nothing to clean)\n");
        return kExitAlreadyStopped;
    }

    LOGI(kTag, "stop: orphan cleanup pass");
    int fails = teardown(true);
    if (fails == 0) {
        std::fprintf(stderr, "gadget-shim stopped, configfs clean\n");
        return kExitOk;
    }
    std::fprintf(stderr, "ERROR: teardown left %d failures — see %s\n", fails, kLogFile);
    return kExitUsageIo;
}

std::vector<std::string> gadget_hidraw_nodes() {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& hr : fs::directory_iterator("/sys/class/hidraw", ec)) {
        std::ifstream f(fs::path(hr.path()) / "device" / "uevent");
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("0003:0000054C:000009CC") != std::string::npos) out.push_back(hr.path().filename().string());
        }
    }
    return out;
}

int cmd_status() {
    auto pid = running_pid();
    std::string udc;
    read_first_line(fs::path(kConfigfsRoot) / kGadgetName / "UDC", udc);
    auto hid = gadget_hidraw_nodes();

    std::fprintf(stderr, "shim:      %s\n",
        pid ? std::string("RUNNING pid=" + std::to_string(*pid)).c_str() : "not running");
    std::fprintf(stderr, "gadget:    %s\n",
        fs::exists(fs::path(kConfigfsRoot) / kGadgetName) ? "configfs present" : "absent");
    std::fprintf(stderr, "udc bind:  %s\n", udc.empty() ? "-" : udc.c_str());
    std::fprintf(stderr, "ffs mount: %s\n", fs::exists(kFfsMount) ? kFfsMount : "-");
    std::fprintf(stderr, "hidraw:    %s\n",
        hid.empty() ? "-" : ([&]{ std::string s; for (auto& h : hid) s += h + " "; return s; })().c_str());

    std::ifstream sf(kStatusFile);
    if (sf) {
        std::string line;
        while (std::getline(sf, line)) std::fprintf(stderr, "  %s\n", line.c_str());
    }
    return pid ? kExitOk : kExitAlreadyStopped;
}

std::optional<Options> parse_opts(int argc, char** argv, int start_at, std::string& err) {
    Options o;
    for (int i = start_at; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--serial" && i + 1 < argc) o.serial = normalize_serial(argv[++i]);
        else if (a == "--descriptor" && i + 1 < argc) {
            o.descriptor = argv[++i];
            if (o.descriptor != "real" && o.descriptor != "vigem") {
                err = "--descriptor must be real|vigem";
                return std::nullopt;
            }
        }
        else if (a == "--udc" && i + 1 < argc) o.udc = argv[++i];
        else if (a == "--bridge") o.bridge = true;
        else if (a == "--bridge-socket" && i + 1 < argc) o.bridge_socket = argv[++i];
        else { err = "unknown option: " + a; return std::nullopt; }
    }
    if (o.serial.size() != 12 || o.serial.find_first_not_of("0123456789abcdef") != std::string::npos) {
        err = "--serial must be a MAC address (aa:bb:cc:dd:ee:ff)";
        return std::nullopt;
    }
    if (o.serial == "aabbccddeeff" && !o.bridge) {
        LOGW(kTag, "no --serial given: placeholder identity aabbccddeeff in use");
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(stderr); return kExitUsageIo; }
    std::string cmd = argv[1];

    if (cmd == "start") {
        std::string err;
        auto o = parse_opts(argc, argv, 2, err);
        if (!o) { std::fprintf(stderr, "ERROR: %s\n", err.c_str()); usage(stderr); return kExitUsageIo; }
        return cmd_start(*o);
    }
    if (cmd == "stop")   return cmd_stop();
    if (cmd == "status") return cmd_status();
    if (cmd == "-h" || cmd == "--help") { usage(stdout); return kExitOk; }
    usage(stderr);
    return kExitUsageIo;
}
