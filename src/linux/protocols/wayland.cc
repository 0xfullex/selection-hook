/**
 * Wayland Protocol Implementation for Linux Selection Hook
 *
 * This file contains Wayland-specific implementations for text selection,
 * clipboard operations, and window management.
 *
 * Uses ext-data-control-v1 (preferred) or wlr-data-control-unstable-v1 v2+
 * (fallback) protocol for PRIMARY selection monitoring.
 */

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// libevdev headers (for input monitoring)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

// Wayland client headers
#include <wayland-client.h>

// Data control protocol headers (pre-generated)
#include "wayland/ext-data-control-v1-client.h"
#include "wayland/wlr-data-control-unstable-v1-client.h"

// Include common definitions
#include "../common.h"

// Input device structure for libevdev
struct InputDevice
{
    int fd;
    struct libevdev *dev;
    std::string path;
    bool is_mouse;
    bool is_keyboard;
};

// Data control protocol type
enum class DataControlType
{
    None,
    Ext,
    Wlr
};

// Read request for proxying receive() through the monitoring thread
struct ReadRequest
{
    void *offer;    // ext or wlr offer pointer
    int write_fd;   // pipe write end
    bool pending;   // whether there is a pending request
    bool done;      // whether the request is completed
};

/**
 * Wayland Protocol Class Implementation
 */
class WaylandProtocol : public ProtocolBase
{
  private:
    // Protocol initialization
    bool initialized;

    // Input monitoring related
    std::vector<InputDevice> input_devices;
    Point current_mouse_pos;
    int epoll_fd;

    // Thread management
    std::atomic<bool> input_monitoring_running{false};
    std::thread input_monitoring_thread;

    // Callback functions
    MouseEventCallback mouse_callback;
    KeyboardEventCallback keyboard_callback;
    SelectionEventCallback selection_callback;
    void *callback_context;

    // Modifier key state tracking
    struct {
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        bool super = false;
    } modifier_state;

    // === Wayland connection ===
    struct wl_display *wl_display_monitor;
    struct wl_registry *wl_registry_monitor;
    struct wl_seat *wl_seat_monitor;

    // Data control protocol (one of two)
    DataControlType dc_type;
    struct ext_data_control_manager_v1 *ext_dc_manager;
    struct zwlr_data_control_manager_v1 *wlr_dc_manager;
    struct ext_data_control_device_v1 *ext_dc_device;
    struct zwlr_data_control_device_v1 *wlr_dc_device;

    // Current PRIMARY selection offer (mutex protected)
    std::mutex primary_offer_mutex;
    struct ext_data_control_offer_v1 *current_ext_offer;
    struct zwlr_data_control_offer_v1 *current_wlr_offer;
    bool has_text_mime;

    // Pending offer (temporary, before primary_selection event confirms it)
    void *pending_offer;
    bool pending_has_text;

    // Wayland monitoring thread
    std::atomic<bool> wayland_monitoring_running{false};
    std::thread wayland_monitoring_thread;

    // Main thread → monitoring thread receive request proxy
    std::mutex read_request_mutex;
    std::condition_variable read_request_cv;
    ReadRequest read_request;

    // libevdev helper methods
    bool InitializeInputDevices();
    void CleanupInputDevices();
    bool IsInputDevice(const std::string &device_path);
    bool SetupInputDevice(const std::string &device_path);
    void ProcessLibevdevEvent(const struct input_event &ev, const InputDevice &device);
    void InputMonitoringThreadProc();

    // Wayland helper methods
    bool InitializeWaylandConnection();
    void CleanupWaylandConnection();
    void WaylandMonitoringThreadProc();

    // MIME type matching helper
    static bool IsTextMimeType(const char *mime_type);

    // Handle primary selection change (common for both protocols)
    void HandlePrimarySelectionChange();

  public:
    // Static callbacks (public for listener table access)
    // Registry callbacks
    static void RegistryGlobal(void *data, struct wl_registry *registry, uint32_t name,
                               const char *interface, uint32_t version);
    static void RegistryGlobalRemove(void *data, struct wl_registry *registry, uint32_t name);

    // ext-data-control device callbacks
    static void ExtDeviceDataOffer(void *data, struct ext_data_control_device_v1 *device,
                                   struct ext_data_control_offer_v1 *offer);
    static void ExtDeviceSelection(void *data, struct ext_data_control_device_v1 *device,
                                   struct ext_data_control_offer_v1 *offer);
    static void ExtDeviceFinished(void *data, struct ext_data_control_device_v1 *device);
    static void ExtDevicePrimarySelection(void *data, struct ext_data_control_device_v1 *device,
                                          struct ext_data_control_offer_v1 *offer);

    // ext-data-control offer callbacks
    static void ExtOfferOffer(void *data, struct ext_data_control_offer_v1 *offer,
                              const char *mime_type);

    // wlr-data-control device callbacks
    static void WlrDeviceDataOffer(void *data, struct zwlr_data_control_device_v1 *device,
                                   struct zwlr_data_control_offer_v1 *offer);
    static void WlrDeviceSelection(void *data, struct zwlr_data_control_device_v1 *device,
                                   struct zwlr_data_control_offer_v1 *offer);
    static void WlrDeviceFinished(void *data, struct zwlr_data_control_device_v1 *device);
    static void WlrDevicePrimarySelection(void *data, struct zwlr_data_control_device_v1 *device,
                                          struct zwlr_data_control_offer_v1 *offer);

    // wlr-data-control offer callbacks
    static void WlrOfferOffer(void *data, struct zwlr_data_control_offer_v1 *offer,
                              const char *mime_type);

    WaylandProtocol()
        : initialized(false),
          epoll_fd(-1),
          mouse_callback(nullptr),
          keyboard_callback(nullptr),
          selection_callback(nullptr),
          callback_context(nullptr),
          wl_display_monitor(nullptr),
          wl_registry_monitor(nullptr),
          wl_seat_monitor(nullptr),
          dc_type(DataControlType::None),
          ext_dc_manager(nullptr),
          wlr_dc_manager(nullptr),
          ext_dc_device(nullptr),
          wlr_dc_device(nullptr),
          current_ext_offer(nullptr),
          current_wlr_offer(nullptr),
          has_text_mime(false),
          pending_offer(nullptr),
          pending_has_text(false)
    {
        current_mouse_pos = Point(0, 0);
        read_request = {nullptr, -1, false, false};
    }

    ~WaylandProtocol() override { Cleanup(); }

    // Protocol identification
    DisplayProtocol GetProtocol() const override { return DisplayProtocol::Wayland; }

    // Modifier key state query
    int GetModifierFlags() override
    {
        int flags = 0;
        if (modifier_state.shift) flags |= MODIFIER_SHIFT;
        if (modifier_state.ctrl)  flags |= MODIFIER_CTRL;
        if (modifier_state.alt)   flags |= MODIFIER_ALT;
        if (modifier_state.super) flags |= MODIFIER_META;
        return flags;
    }

    // Initialization and cleanup
    bool Initialize() override
    {
        initialized = true;

        if (!InitializeWaylandConnection())
        {
            printf("[Wayland] WARNING: Failed to initialize Wayland connection. "
                   "Selection monitoring will not be available.\n");
            // Don't fail - input monitoring via libevdev can still work
        }

        return true;
    }

    void Cleanup() override
    {
        // Stop input monitoring first
        StopInputMonitoring();
        CleanupInputMonitoring();

        // Cleanup Wayland resources
        CleanupWaylandConnection();

        initialized = false;
    }

    // Window management
    uint64_t GetActiveWindow() override
    {
        if (!initialized)
            return 0;

        // Wayland security model does not expose window information.
        // Return a sentinel value to avoid checks in selection_hook.cc blocking events.
        return 1;
    }

    bool GetWindowRect(uint64_t window, WindowRect &rect) override
    {
        // Wayland doesn't expose global window coordinates by design
        return false;
    }

    bool GetProgramNameFromWindow(uint64_t window, std::string &programName) override
    {
        if (!initialized || !window)
            return false;

        // Wayland security model does not expose program names
        return false;
    }

    // Text selection
    bool GetTextViaPrimary(std::string &text) override;

    // Clipboard operations
    bool WriteClipboard(const std::string &text) override
    {
        if (!initialized)
            return false;

        // TODO: Implement Wayland clipboard writing
        return false;
    }

    bool ReadClipboard(std::string &text) override
    {
        if (!initialized)
            return false;

        // TODO: Implement Wayland clipboard reading
        return false;
    }

    // Input monitoring implementation
    bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                   SelectionEventCallback selectionCb, void *context) override
    {
        if (!initialized)
            return false;

        mouse_callback = mouseCallback;
        keyboard_callback = keyboardCallback;
        selection_callback = selectionCb;
        callback_context = context;

        if (!InitializeInputDevices())
        {
            printf("[Wayland] WARNING: Failed to initialize input devices "
                   "(try adding user to 'input' group: sudo usermod -aG input $USER). "
                   "Mouse/keyboard events will not be available.\n");
            // Don't fail — Wayland data-control selection monitoring can still work
        }

        // Succeed if we have either input devices or data-control protocol
        return (!input_devices.empty()) || (dc_type != DataControlType::None);
    }

    void CleanupInputMonitoring() override
    {
        // Stop monitoring first
        StopInputMonitoring();

        CleanupInputDevices();
        mouse_callback = nullptr;
        keyboard_callback = nullptr;
        selection_callback = nullptr;
        callback_context = nullptr;
    }

    bool StartInputMonitoring() override
    {
        if (!initialized)
            return false;

        // Start libevdev input monitoring thread if devices are available
        if (!input_devices.empty() && epoll_fd >= 0 && !input_monitoring_running)
        {
            input_monitoring_running = true;
            input_monitoring_thread = std::thread(&WaylandProtocol::InputMonitoringThreadProc, this);
        }

        // Start Wayland monitoring thread if data-control is available
        if (dc_type != DataControlType::None && wl_display_monitor && !wayland_monitoring_running)
        {
            wayland_monitoring_running = true;
            wayland_monitoring_thread = std::thread(&WaylandProtocol::WaylandMonitoringThreadProc, this);
        }

        return input_monitoring_running || wayland_monitoring_running;
    }

    void StopInputMonitoring() override
    {
        // Stop Wayland monitoring thread
        wayland_monitoring_running = false;
        // Wake up any pending read request
        {
            std::lock_guard<std::mutex> lock(read_request_mutex);
            read_request.done = true;
        }
        read_request_cv.notify_all();
        if (wayland_monitoring_thread.joinable())
        {
            wayland_monitoring_thread.join();
        }

        // Stop libevdev monitoring thread
        input_monitoring_running = false;
        if (input_monitoring_thread.joinable())
        {
            input_monitoring_thread.join();
        }
    }
};

// ============================================================================
// Wayland connection and protocol binding
// ============================================================================

// Static listener tables
static const struct wl_registry_listener registry_listener = {
    WaylandProtocol::RegistryGlobal,
    WaylandProtocol::RegistryGlobalRemove,
};

static const struct ext_data_control_device_v1_listener ext_device_listener = {
    WaylandProtocol::ExtDeviceDataOffer,
    WaylandProtocol::ExtDeviceSelection,
    WaylandProtocol::ExtDeviceFinished,
    WaylandProtocol::ExtDevicePrimarySelection,
};

static const struct ext_data_control_offer_v1_listener ext_offer_listener = {
    WaylandProtocol::ExtOfferOffer,
};

static const struct zwlr_data_control_device_v1_listener wlr_device_listener = {
    WaylandProtocol::WlrDeviceDataOffer,
    WaylandProtocol::WlrDeviceSelection,
    WaylandProtocol::WlrDeviceFinished,
    WaylandProtocol::WlrDevicePrimarySelection,
};

static const struct zwlr_data_control_offer_v1_listener wlr_offer_listener = {
    WaylandProtocol::WlrOfferOffer,
};

bool WaylandProtocol::IsTextMimeType(const char *mime_type)
{
    return (strcmp(mime_type, "text/plain;charset=utf-8") == 0 ||
            strcmp(mime_type, "text/plain") == 0 ||
            strcmp(mime_type, "UTF8_STRING") == 0 ||
            strcmp(mime_type, "TEXT") == 0);
}

bool WaylandProtocol::InitializeWaylandConnection()
{
    wl_display_monitor = wl_display_connect(nullptr);
    if (!wl_display_monitor)
    {
        printf("[Wayland] Failed to connect to Wayland display\n");
        return false;
    }

    wl_registry_monitor = wl_display_get_registry(wl_display_monitor);
    if (!wl_registry_monitor)
    {
        printf("[Wayland] Failed to get registry\n");
        wl_display_disconnect(wl_display_monitor);
        wl_display_monitor = nullptr;
        return false;
    }

    wl_registry_add_listener(wl_registry_monitor, &registry_listener, this);

    // Roundtrip to receive registry globals
    wl_display_roundtrip(wl_display_monitor);

    if (!wl_seat_monitor)
    {
        printf("[Wayland] WARNING: No wl_seat found\n");
        CleanupWaylandConnection();
        return false;
    }

    if (dc_type == DataControlType::None)
    {
        printf("[Wayland] WARNING: No data-control protocol available "
               "(ext-data-control-v1 or wlr-data-control-unstable-v1 v2+). "
               "Selection monitoring will not work.\n");
        CleanupWaylandConnection();
        return false;
    }

    // Create data device for the seat
    if (dc_type == DataControlType::Ext)
    {
        ext_dc_device = ext_data_control_manager_v1_get_data_device(ext_dc_manager, wl_seat_monitor);
        if (ext_dc_device)
        {
            ext_data_control_device_v1_add_listener(ext_dc_device, &ext_device_listener, this);
            printf("[Wayland] Using ext-data-control-v1 protocol\n");
        }
    }
    else if (dc_type == DataControlType::Wlr)
    {
        wlr_dc_device = zwlr_data_control_manager_v1_get_data_device(wlr_dc_manager, wl_seat_monitor);
        if (wlr_dc_device)
        {
            zwlr_data_control_device_v1_add_listener(wlr_dc_device, &wlr_device_listener, this);
            printf("[Wayland] Using wlr-data-control-unstable-v1 protocol\n");
        }
    }

    // Roundtrip to receive initial selection events
    wl_display_roundtrip(wl_display_monitor);

    return true;
}

void WaylandProtocol::CleanupWaylandConnection()
{
    // Destroy data control objects
    if (ext_dc_device)
    {
        ext_data_control_device_v1_destroy(ext_dc_device);
        ext_dc_device = nullptr;
    }
    if (wlr_dc_device)
    {
        zwlr_data_control_device_v1_destroy(wlr_dc_device);
        wlr_dc_device = nullptr;
    }

    // Destroy current offers
    {
        std::lock_guard<std::mutex> lock(primary_offer_mutex);
        if (current_ext_offer)
        {
            ext_data_control_offer_v1_destroy(current_ext_offer);
            current_ext_offer = nullptr;
        }
        if (current_wlr_offer)
        {
            zwlr_data_control_offer_v1_destroy(current_wlr_offer);
            current_wlr_offer = nullptr;
        }
        has_text_mime = false;
    }

    // Destroy pending offer
    if (pending_offer)
    {
        if (dc_type == DataControlType::Ext)
            ext_data_control_offer_v1_destroy((struct ext_data_control_offer_v1 *)pending_offer);
        else if (dc_type == DataControlType::Wlr)
            zwlr_data_control_offer_v1_destroy((struct zwlr_data_control_offer_v1 *)pending_offer);
        pending_offer = nullptr;
    }

    // Destroy managers
    if (ext_dc_manager)
    {
        ext_data_control_manager_v1_destroy(ext_dc_manager);
        ext_dc_manager = nullptr;
    }
    if (wlr_dc_manager)
    {
        zwlr_data_control_manager_v1_destroy(wlr_dc_manager);
        wlr_dc_manager = nullptr;
    }

    // Destroy seat and registry
    if (wl_seat_monitor)
    {
        wl_seat_destroy(wl_seat_monitor);
        wl_seat_monitor = nullptr;
    }
    if (wl_registry_monitor)
    {
        wl_registry_destroy(wl_registry_monitor);
        wl_registry_monitor = nullptr;
    }

    // Disconnect display
    if (wl_display_monitor)
    {
        wl_display_disconnect(wl_display_monitor);
        wl_display_monitor = nullptr;
    }

    dc_type = DataControlType::None;
}

// ============================================================================
// Registry callbacks
// ============================================================================

void WaylandProtocol::RegistryGlobal(void *data, struct wl_registry *registry, uint32_t name,
                                     const char *interface, uint32_t version)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    if (strcmp(interface, "wl_seat") == 0)
    {
        // Bind the first seat
        if (!self->wl_seat_monitor)
        {
            self->wl_seat_monitor = static_cast<struct wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, 1));
        }
    }
    else if (strcmp(interface, "ext_data_control_manager_v1") == 0)
    {
        // Prefer ext-data-control-v1
        self->ext_dc_manager = static_cast<struct ext_data_control_manager_v1 *>(
            wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
        self->dc_type = DataControlType::Ext;
    }
    else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0)
    {
        // Only use wlr if ext is not available, and version >= 2 for primary_selection
        if (self->dc_type != DataControlType::Ext && version >= 2)
        {
            self->wlr_dc_manager = static_cast<struct zwlr_data_control_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface, 2));
            self->dc_type = DataControlType::Wlr;
        }
    }
}

void WaylandProtocol::RegistryGlobalRemove(void *data, struct wl_registry *registry, uint32_t name)
{
    // Not handling global removal for now
}

// ============================================================================
// ext-data-control callbacks
// ============================================================================

void WaylandProtocol::ExtDeviceDataOffer(void *data, struct ext_data_control_device_v1 *device,
                                         struct ext_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    // Destroy previous pending offer if any
    if (self->pending_offer)
    {
        ext_data_control_offer_v1_destroy((struct ext_data_control_offer_v1 *)self->pending_offer);
    }

    // Store as pending, add offer listener to track MIME types
    self->pending_offer = offer;
    self->pending_has_text = false;
    ext_data_control_offer_v1_add_listener(offer, &ext_offer_listener, self);
}

void WaylandProtocol::ExtDeviceSelection(void *data, struct ext_data_control_device_v1 *device,
                                         struct ext_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    // CLIPBOARD selection changed - destroy the offer, we don't use it for now
    if (offer && offer == self->pending_offer)
    {
        self->pending_offer = nullptr;
        self->pending_has_text = false;
        ext_data_control_offer_v1_destroy(offer);
    }
    else if (offer)
    {
        ext_data_control_offer_v1_destroy(offer);
    }
    // offer == NULL means selection was cleared, nothing to do
}

void WaylandProtocol::ExtDeviceFinished(void *data, struct ext_data_control_device_v1 *device)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);
    if (self->ext_dc_device == device)
    {
        ext_data_control_device_v1_destroy(device);
        self->ext_dc_device = nullptr;
    }
}

void WaylandProtocol::ExtDevicePrimarySelection(void *data, struct ext_data_control_device_v1 *device,
                                                struct ext_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    {
        std::lock_guard<std::mutex> lock(self->primary_offer_mutex);

        // Destroy previous offer
        if (self->current_ext_offer)
        {
            ext_data_control_offer_v1_destroy(self->current_ext_offer);
            self->current_ext_offer = nullptr;
        }

        if (offer && offer == self->pending_offer)
        {
            self->current_ext_offer = offer;
            self->has_text_mime = self->pending_has_text;
            self->pending_offer = nullptr;
            self->pending_has_text = false;
        }
        else if (offer)
        {
            // Unexpected offer (not from data_offer event), just store it
            self->current_ext_offer = offer;
            self->has_text_mime = false;
        }
        else
        {
            // NULL offer - primary selection cleared
            self->has_text_mime = false;
        }
    }

    // Trigger selection change callback
    if (offer)
    {
        self->HandlePrimarySelectionChange();
    }
}

void WaylandProtocol::ExtOfferOffer(void *data, struct ext_data_control_offer_v1 *offer,
                                    const char *mime_type)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    if (offer == self->pending_offer && IsTextMimeType(mime_type))
    {
        self->pending_has_text = true;
    }
}

// ============================================================================
// wlr-data-control callbacks
// ============================================================================

void WaylandProtocol::WlrDeviceDataOffer(void *data, struct zwlr_data_control_device_v1 *device,
                                         struct zwlr_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    // Destroy previous pending offer if any
    if (self->pending_offer)
    {
        zwlr_data_control_offer_v1_destroy((struct zwlr_data_control_offer_v1 *)self->pending_offer);
    }

    // Store as pending, add offer listener to track MIME types
    self->pending_offer = offer;
    self->pending_has_text = false;
    zwlr_data_control_offer_v1_add_listener(offer, &wlr_offer_listener, self);
}

void WaylandProtocol::WlrDeviceSelection(void *data, struct zwlr_data_control_device_v1 *device,
                                         struct zwlr_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    // CLIPBOARD selection changed - destroy the offer, we don't use it for now
    if (offer && offer == self->pending_offer)
    {
        self->pending_offer = nullptr;
        self->pending_has_text = false;
        zwlr_data_control_offer_v1_destroy(offer);
    }
    else if (offer)
    {
        zwlr_data_control_offer_v1_destroy(offer);
    }
}

void WaylandProtocol::WlrDeviceFinished(void *data, struct zwlr_data_control_device_v1 *device)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);
    if (self->wlr_dc_device == device)
    {
        zwlr_data_control_device_v1_destroy(device);
        self->wlr_dc_device = nullptr;
    }
}

void WaylandProtocol::WlrDevicePrimarySelection(void *data, struct zwlr_data_control_device_v1 *device,
                                                 struct zwlr_data_control_offer_v1 *offer)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    {
        std::lock_guard<std::mutex> lock(self->primary_offer_mutex);

        // Destroy previous offer
        if (self->current_wlr_offer)
        {
            zwlr_data_control_offer_v1_destroy(self->current_wlr_offer);
            self->current_wlr_offer = nullptr;
        }

        if (offer && offer == self->pending_offer)
        {
            self->current_wlr_offer = offer;
            self->has_text_mime = self->pending_has_text;
            self->pending_offer = nullptr;
            self->pending_has_text = false;
        }
        else if (offer)
        {
            self->current_wlr_offer = offer;
            self->has_text_mime = false;
        }
        else
        {
            self->has_text_mime = false;
        }
    }

    if (offer)
    {
        self->HandlePrimarySelectionChange();
    }
}

void WaylandProtocol::WlrOfferOffer(void *data, struct zwlr_data_control_offer_v1 *offer,
                                     const char *mime_type)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    if (offer == self->pending_offer && IsTextMimeType(mime_type))
    {
        self->pending_has_text = true;
    }
}

// ============================================================================
// Selection event handling
// ============================================================================

void WaylandProtocol::HandlePrimarySelectionChange()
{
    if (!selection_callback || !callback_context)
        return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    SelectionChangeContext *ctx = new SelectionChangeContext();
    ctx->timestamp_ms = static_cast<uint64_t>(now);

    selection_callback(callback_context, ctx);
}

// ============================================================================
// Wayland monitoring thread
// ============================================================================

void WaylandProtocol::WaylandMonitoringThreadProc()
{
    if (!wl_display_monitor)
        return;

    int wl_fd = wl_display_get_fd(wl_display_monitor);

    while (wayland_monitoring_running)
    {
        // Check for pending read requests from main thread
        {
            std::lock_guard<std::mutex> lock(read_request_mutex);
            if (read_request.pending)
            {
                // Process the receive request
                const char *mime = "text/plain;charset=utf-8";

                if (dc_type == DataControlType::Ext && read_request.offer)
                {
                    ext_data_control_offer_v1_receive(
                        (struct ext_data_control_offer_v1 *)read_request.offer,
                        mime, read_request.write_fd);
                }
                else if (dc_type == DataControlType::Wlr && read_request.offer)
                {
                    zwlr_data_control_offer_v1_receive(
                        (struct zwlr_data_control_offer_v1 *)read_request.offer,
                        mime, read_request.write_fd);
                }

                wl_display_flush(wl_display_monitor);
                close(read_request.write_fd);
                read_request.write_fd = -1;
                read_request.pending = false;
                read_request.done = true;
            }
        }
        read_request_cv.notify_all();

        // Prepare for reading from Wayland fd
        // Flush pending requests before select
        while (wl_display_prepare_read(wl_display_monitor) != 0)
        {
            wl_display_dispatch_pending(wl_display_monitor);
        }
        wl_display_flush(wl_display_monitor);

        // Use select() with 200ms timeout for shutdown check
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(wl_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;  // 200ms

        int ret = select(wl_fd + 1, &read_fds, nullptr, nullptr, &timeout);

        if (ret < 0)
        {
            wl_display_cancel_read(wl_display_monitor);
            if (errno == EINTR)
                continue;
            printf("[Wayland] select() error: %s\n", strerror(errno));
            break;
        }

        if (ret == 0)
        {
            // Timeout, cancel read and check running flag
            wl_display_cancel_read(wl_display_monitor);
            continue;
        }

        // Data available, read events
        if (wl_display_read_events(wl_display_monitor) < 0)
        {
            printf("[Wayland] wl_display_read_events() failed: %s\n", strerror(errno));
            break;
        }

        // Dispatch pending events
        if (wl_display_dispatch_pending(wl_display_monitor) < 0)
        {
            printf("[Wayland] wl_display_dispatch_pending() failed: %s\n", strerror(errno));
            break;
        }
    }
}

// ============================================================================
// GetTextViaPrimary - read text from PRIMARY selection via pipe
// ============================================================================

bool WaylandProtocol::GetTextViaPrimary(std::string &text)
{
    if (!initialized || dc_type == DataControlType::None)
        return false;

    void *offer_to_read = nullptr;

    // Step 1: Lock and check if we have a valid offer with text MIME
    {
        std::lock_guard<std::mutex> lock(primary_offer_mutex);

        if (!has_text_mime)
            return false;

        if (dc_type == DataControlType::Ext)
            offer_to_read = current_ext_offer;
        else if (dc_type == DataControlType::Wlr)
            offer_to_read = current_wlr_offer;

        if (!offer_to_read)
            return false;
    }

    // Step 2: Create pipe
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return false;

    // Step 3: Submit read request to monitoring thread
    {
        std::lock_guard<std::mutex> lock(read_request_mutex);
        read_request.offer = offer_to_read;
        read_request.write_fd = fds[1];
        read_request.pending = true;
        read_request.done = false;
    }

    // Step 4: Wait for monitoring thread to process the request (1s timeout)
    {
        std::unique_lock<std::mutex> lock(read_request_mutex);
        bool ok = read_request_cv.wait_for(lock, std::chrono::seconds(1),
                                           [this] { return read_request.done; });
        if (!ok)
        {
            // Timeout - close pipe write end if still open
            if (read_request.write_fd >= 0)
            {
                close(read_request.write_fd);
                read_request.write_fd = -1;
            }
            read_request.pending = false;
            close(fds[0]);
            return false;
        }
    }

    // Step 5: Read data from pipe read end (select + read, 1s timeout, max 1MB)
    std::string result;
    const size_t MAX_SIZE = 1024 * 1024;  // 1MB limit
    char buf[4096];

    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(1))
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fds[0], &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms

        int ret = select(fds[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0)
        {
            result.append(buf, n);
            if (result.size() >= MAX_SIZE)
            {
                result.resize(MAX_SIZE);
                break;
            }
        }
        else if (n == 0)
        {
            // EOF - source closed the write end
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
    }

    close(fds[0]);

    if (!result.empty())
    {
        text = std::move(result);
        return true;
    }

    return false;
}

// ============================================================================
// libevdev input monitoring (unchanged from original)
// ============================================================================

/**
 * Initialize input devices using libevdev with epoll
 */
bool WaylandProtocol::InitializeInputDevices()
{
    // Create epoll instance
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
        return false;

    const char *input_dir = "/dev/input";
    DIR *dir = opendir(input_dir);
    if (!dir)
    {
        close(epoll_fd);
        epoll_fd = -1;
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (strncmp(entry->d_name, "event", 5) == 0)
        {
            std::string device_path = std::string(input_dir) + "/" + entry->d_name;
            if (IsInputDevice(device_path))
            {
                SetupInputDevice(device_path);
            }
        }
    }

    closedir(dir);

    if (input_devices.empty())
    {
        close(epoll_fd);
        epoll_fd = -1;
        return false;
    }

    return true;
}

/**
 * Cleanup input devices and epoll
 */
void WaylandProtocol::CleanupInputDevices()
{
    for (auto &device : input_devices)
    {
        if (device.fd >= 0)
        {
            // Remove from epoll before closing
            if (epoll_fd >= 0)
            {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, device.fd, nullptr);
            }
            close(device.fd);
            device.fd = -1;
        }
        if (device.dev)
        {
            libevdev_free(device.dev);
            device.dev = nullptr;
        }
    }
    input_devices.clear();

    // Close epoll instance
    if (epoll_fd >= 0)
    {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

/**
 * Check if a device path is a valid input device
 */
bool WaylandProtocol::IsInputDevice(const std::string &device_path)
{
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;

    struct libevdev *dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0)
    {
        close(fd);
        return false;
    }

    // Check if device has mouse/touchpad or keyboard capabilities
    bool is_mouse = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) ||
                    libevdev_has_event_code(dev, EV_REL, REL_X) ||
                    libevdev_has_event_code(dev, EV_REL, REL_Y) ||
                    libevdev_has_event_code(dev, EV_ABS, ABS_X) ||
                    libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);

    bool is_keyboard = libevdev_has_event_code(dev, EV_KEY, KEY_A) || libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);

    libevdev_free(dev);
    close(fd);

    return is_mouse || is_keyboard;
}

/**
 * Setup an input device for monitoring with epoll
 */
bool WaylandProtocol::SetupInputDevice(const std::string &device_path)
{
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;

    struct libevdev *dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0)
    {
        close(fd);
        return false;
    }

    InputDevice device;
    device.fd = fd;
    device.dev = dev;
    device.path = device_path;

    // Determine device capabilities
    device.is_mouse = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) || libevdev_has_event_code(dev, EV_REL, REL_X) ||
                      libevdev_has_event_code(dev, EV_REL, REL_Y);

    device.is_keyboard = libevdev_has_event_code(dev, EV_KEY, KEY_A) || libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);

    // Add to epoll for monitoring
    if (epoll_fd >= 0)
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;  // Edge-triggered mode
        ev.data.fd = fd;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        {
            libevdev_free(dev);
            close(fd);
            return false;
        }
    }

    input_devices.push_back(device);
    return true;
}

/**
 * Input monitoring thread function using libevdev with epoll
 */
void WaylandProtocol::InputMonitoringThreadProc()
{
    if (input_devices.empty() || epoll_fd < 0)
        return;

    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (input_monitoring_running)
    {
        // Wait for input events with timeout (10ms)
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, 10);

        if (num_events < 0)
        {
            // Error occurred
            if (errno == EINTR)
                continue;  // Interrupted by signal, continue
            break;         // Other errors, exit loop
        }

        if (num_events == 0)
            continue;  // Timeout, continue

        // Process events
        for (int i = 0; i < num_events; i++)
        {
            int fd = events[i].data.fd;

            // Find the corresponding device
            InputDevice *target_device = nullptr;
            for (auto &device : input_devices)
            {
                if (device.fd == fd)
                {
                    target_device = &device;
                    break;
                }
            }

            if (!target_device)
                continue;

            // Check for errors or hangup
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                // Device error or disconnected, skip this device
                continue;
            }

            // Process input events from this device
            if (events[i].events & EPOLLIN)
            {
                struct input_event ev;
                int rc = libevdev_next_event(target_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

                while (rc == LIBEVDEV_READ_STATUS_SUCCESS)
                {
                    ProcessLibevdevEvent(ev, *target_device);
                    rc = libevdev_next_event(target_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                }

                if (rc == LIBEVDEV_READ_STATUS_SYNC)
                {
                    // Handle sync events
                    while (rc == LIBEVDEV_READ_STATUS_SYNC)
                    {
                        ProcessLibevdevEvent(ev, *target_device);
                        rc = libevdev_next_event(target_device->dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
                    }
                }
            }
        }
    }
}

/**
 * Process a libevdev event and convert it to our event system
 */
void WaylandProtocol::ProcessLibevdevEvent(const struct input_event &ev, const InputDevice &device)
{
    if (ev.type == EV_SYN)
        return;  // Skip sync events

    // Handle mouse events
    if (device.is_mouse && mouse_callback)
    {
        if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE))
        {
            // Mouse button event
            MouseEventContext *mouseEvent = new MouseEventContext();
            mouseEvent->type = ev.type;
            mouseEvent->code = ev.code;
            mouseEvent->value = ev.value;
            mouseEvent->pos = current_mouse_pos;
            mouseEvent->button = (ev.code == BTN_LEFT)    ? static_cast<int>(MouseButton::Left)
                                 : (ev.code == BTN_RIGHT) ? static_cast<int>(MouseButton::Right)
                                                          : static_cast<int>(MouseButton::Middle);
            mouseEvent->flag = 0;

            mouse_callback(callback_context, mouseEvent);
        }
        else if (ev.type == EV_REL)
        {
            if (ev.code == REL_X)
            {
                current_mouse_pos.x += ev.value;
            }
            else if (ev.code == REL_Y)
            {
                current_mouse_pos.y += ev.value;
            }

            if (ev.code == REL_X || ev.code == REL_Y)
            {
                // Mouse move event
                MouseEventContext *mouseEvent = new MouseEventContext();
                mouseEvent->type = ev.type;
                mouseEvent->code = ev.code;
                mouseEvent->value = ev.value;
                mouseEvent->pos = current_mouse_pos;
                mouseEvent->button = static_cast<int>(MouseButton::None);
                mouseEvent->flag = 0;

                mouse_callback(callback_context, mouseEvent);
            }
            else if (ev.code == REL_WHEEL || ev.code == REL_HWHEEL)
            {
                // Mouse wheel event
                MouseEventContext *mouseEvent = new MouseEventContext();
                mouseEvent->type = ev.type;
                mouseEvent->code = ev.code;
                mouseEvent->value = ev.value;
                mouseEvent->pos = current_mouse_pos;
                mouseEvent->button = (ev.code == REL_WHEEL) ? static_cast<int>(MouseButton::WheelVertical)
                                                            : static_cast<int>(MouseButton::WheelHorizontal);
                mouseEvent->flag = ev.value > 0 ? 1 : -1;

                mouse_callback(callback_context, mouseEvent);
            }
        }
        else if (ev.type == EV_ABS)
        {
            // Touchpad / absolute input device support
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)
            {
                current_mouse_pos.x = ev.value;
            }
            else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)
            {
                current_mouse_pos.y = ev.value;
            }

            if (ev.code == ABS_X || ev.code == ABS_Y ||
                ev.code == ABS_MT_POSITION_X || ev.code == ABS_MT_POSITION_Y)
            {
                MouseEventContext *mouseEvent = new MouseEventContext();
                mouseEvent->type = EV_REL;  // Normalize to REL for upstream compatibility
                mouseEvent->code = (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) ? REL_X : REL_Y;
                mouseEvent->value = 0;
                mouseEvent->pos = current_mouse_pos;
                mouseEvent->button = static_cast<int>(MouseButton::None);
                mouseEvent->flag = 0;

                mouse_callback(callback_context, mouseEvent);
            }
        }
    }

    // Handle keyboard events
    if (device.is_keyboard && keyboard_callback && ev.type == EV_KEY)
    {
        bool is_press = (ev.value == 1);

        // Update modifier key state
        switch (ev.code)
        {
            case KEY_LEFTCTRL:
            case KEY_RIGHTCTRL:
                modifier_state.ctrl = is_press;
                break;
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT:
                modifier_state.shift = is_press;
                break;
            case KEY_LEFTALT:
            case KEY_RIGHTALT:
                modifier_state.alt = is_press;
                break;
            case KEY_LEFTMETA:
            case KEY_RIGHTMETA:
                modifier_state.super = is_press;
                break;
        }

        // Build modifier flags bitmask
        int flags = 0;
        if (modifier_state.shift) flags |= MODIFIER_SHIFT;
        if (modifier_state.ctrl)  flags |= MODIFIER_CTRL;
        if (modifier_state.alt)   flags |= MODIFIER_ALT;
        if (modifier_state.super) flags |= MODIFIER_META;

        KeyboardEventContext *keyboardEvent = new KeyboardEventContext();
        keyboardEvent->type = ev.type;
        keyboardEvent->code = ev.code;
        keyboardEvent->value = ev.value;
        keyboardEvent->flags = flags;

        keyboard_callback(callback_context, keyboardEvent);
    }
}

// Factory function to create WaylandProtocol instance
std::unique_ptr<ProtocolBase> CreateWaylandProtocol()
{
    return std::make_unique<WaylandProtocol>();
}
