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
#include <cmath>
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

// X11 headers for XWayland cursor position fallback
#include <X11/Xlib.h>
#include <X11/Xresource.h>

// Unix socket headers for Hyprland IPC
#include <sys/socket.h>
#include <sys/un.h>

// Dynamic loading for KDE DBus cursor position query
#include <dlfcn.h>

// Character classification for tolower
#include <cctype>

// Undefine X11 None macro that conflicts with our SelectionDetectType::None enum
#ifdef None
#undef None
#endif

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

// DBus ABI-stable types for dlopen-based KDE cursor position query
// These match the libdbus-1 public ABI and are safe to use with dlsym'd functions
struct DBusError_ABI
{
    const char *name;
    const char *message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void *padding1;
};

struct DBusMessageIter_ABI
{
    void *dummy1;
    void *dummy2;
    uint32_t dummy3;
    int dummy4, dummy5, dummy6, dummy7, dummy8, dummy9, dummy10, dummy11;
    int pad1;
    void *pad2;
    void *pad3;
};

// DBus function pointers (loaded via dlopen at runtime to avoid build dependency)
struct DBusFunctions
{
    void *lib_handle = nullptr;
    void *session_bus = nullptr;  // DBusConnection*

    void (*error_init)(DBusError_ABI *) = nullptr;
    void (*error_free)(DBusError_ABI *) = nullptr;
    void *(*bus_get)(int, DBusError_ABI *) = nullptr;
    void *(*message_new_method_call)(const char *, const char *, const char *, const char *) = nullptr;
    void *(*connection_send_with_reply_and_block)(void *, void *, int, DBusError_ABI *) = nullptr;
    int (*message_iter_init)(void *, DBusMessageIter_ABI *) = nullptr;
    void (*message_iter_recurse)(DBusMessageIter_ABI *, DBusMessageIter_ABI *) = nullptr;
    int (*message_iter_get_arg_type)(DBusMessageIter_ABI *) = nullptr;
    void (*message_iter_get_basic)(DBusMessageIter_ABI *, void *) = nullptr;
    int (*message_iter_next)(DBusMessageIter_ABI *) = nullptr;
    void (*message_unref)(void *) = nullptr;
    const char *(*bus_get_unique_name)(void *) = nullptr;    // Get our DBus unique name
    int (*message_append_args)(void *, int, ...) = nullptr;  // Append method arguments
    int (*connection_read_write)(void *, int) = nullptr;     // Read/write poll
    void *(*connection_pop_message)(void *) = nullptr;       // Pop pending message
    const char *(*message_get_member)(void *) = nullptr;     // Get message method name
};

// Read request for proxying receive() through the monitoring thread
struct ReadRequest
{
    void *offer;   // ext or wlr offer pointer
    int write_fd;  // pipe write end
    bool pending;  // whether there is a pending request
    bool done;     // whether the request is completed
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
    ModifierState modifier_state;

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

    // Offer use-after-free protection: when GetTextViaPrimary holds a reference to the
    // current offer (between copying it and submitting the read request), the destruction
    // of the offer is deferred until the monitoring thread can safely destroy it.
    // All Wayland protocol calls (including _destroy) must happen on the monitoring thread.
    bool offer_in_use = false;
    std::vector<struct ext_data_control_offer_v1 *> deferred_destroy_ext;
    std::vector<struct zwlr_data_control_offer_v1 *> deferred_destroy_wlr;

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

    // Environment info (set by top-level detection via SetEnvInfo)
    LinuxEnvInfo env_info;

    // XWayland fallback for cursor position
    Display *xwayland_display = nullptr;
    bool xwayland_tried = false;
    double xwayland_scale = 1.0;  // Scale factor for converting compositor IPC logical coords to X11 screen coords

    // KDE DBus for cursor position (dlopen'd libdbus-1.so.3)
    DBusFunctions dbus_fn;
    bool dbus_tried = false;
    std::string kde_script_path;  // Temporary KWin script file path
    std::string kde_bus_name;     // Our DBus unique name (e.g. ":1.234")

    // KWin script execution method, auto-detected on first call:
    //   PerScript  — standard KWin: run() on /Scripting/Script{id}
    //                (Plasma 5 & 6 source code both register per-script DBus objects)
    //   ManagerStart — fallback: start() on /Scripting manager
    //                (some Plasma 6 builds don't expose per-script objects)
    //   Unknown    — not yet probed
    enum class KWinRunMethod
    {
        Unknown,
        PerScript,
        ManagerStart
    };
    KWinRunMethod kde_run_method = KWinRunMethod::Unknown;

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

    // Cursor position methods
    void EnsureXWaylandInitialized();
    bool GetCursorPositionHyprland(Point &pos);
    bool LoadDBusFunctions();
    bool GetCursorPositionKDE(Point &pos);
    bool GetCursorPositionXWayland(Point &pos);

    // Handle primary selection change (common for both protocols)
    void HandlePrimarySelectionChange();

  public:
    // Static callbacks (public for listener table access)
    // Registry callbacks
    static void RegistryGlobal(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                               uint32_t version);
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
    static void ExtOfferOffer(void *data, struct ext_data_control_offer_v1 *offer, const char *mime_type);

    // wlr-data-control device callbacks
    static void WlrDeviceDataOffer(void *data, struct zwlr_data_control_device_v1 *device,
                                   struct zwlr_data_control_offer_v1 *offer);
    static void WlrDeviceSelection(void *data, struct zwlr_data_control_device_v1 *device,
                                   struct zwlr_data_control_offer_v1 *offer);
    static void WlrDeviceFinished(void *data, struct zwlr_data_control_device_v1 *device);
    static void WlrDevicePrimarySelection(void *data, struct zwlr_data_control_device_v1 *device,
                                          struct zwlr_data_control_offer_v1 *offer);

    // wlr-data-control offer callbacks
    static void WlrOfferOffer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime_type);

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
        // Hardware-accumulated position from libevdev (REL/ABS events).
        // Not a real screen coordinate on Wayland; valid stays false.
        current_mouse_pos = Point();
        read_request = {nullptr, -1, false, false};
    }

    ~WaylandProtocol() override { Cleanup(); }

    // Protocol identification
    DisplayProtocol GetProtocol() const override { return DisplayProtocol::Wayland; }

    // Modifier key state query
    int GetModifierFlags() override { return modifier_state.GetFlags(); }

    // Set environment info from top-level detection
    void SetEnvInfo(const LinuxEnvInfo &info) override { env_info = info; }

    // Get accurate cursor position from compositor
    Point GetCurrentMousePosition() override;

    // Initialization and cleanup
    bool Initialize() override
    {
        initialized = true;

        if (!InitializeWaylandConnection())
        {
            fprintf(stderr,
                    "[Wayland] WARNING: Failed to initialize Wayland connection. "
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

        // Cleanup XWayland connection
        if (xwayland_display)
        {
            XCloseDisplay(xwayland_display);
            xwayland_display = nullptr;
        }

        // Unload KWin script from compositor and delete temporary file
        if (!kde_script_path.empty())
        {
            if (dbus_fn.session_bus && dbus_fn.message_new_method_call)
            {
                const char *sname = "selectionhook_cursor";
                void *umsg = dbus_fn.message_new_method_call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                                                             "unloadScript");
                if (umsg)
                {
                    dbus_fn.message_append_args(umsg, 's', &sname, '\0');
                    DBusError_ABI uerr;
                    dbus_fn.error_init(&uerr);
                    void *ureply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, umsg, 200, &uerr);
                    dbus_fn.message_unref(umsg);
                    if (ureply)
                        dbus_fn.message_unref(ureply);
                    dbus_fn.error_free(&uerr);
                }
            }
            unlink(kde_script_path.c_str());
            kde_script_path.clear();
        }

        // Cleanup DBus (session bus is shared, don't close it)
        if (dbus_fn.lib_handle)
        {
            dlclose(dbus_fn.lib_handle);
            dbus_fn = DBusFunctions{};
        }

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

        if (env_info.hasInputDeviceAccess)
        {
            if (!InitializeInputDevices())
            {
                fprintf(stderr,
                        "[Wayland] WARNING: Failed to initialize input devices. "
                        "Mouse/keyboard events will not be available.\n");
            }
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
    return (strcmp(mime_type, "text/plain;charset=utf-8") == 0 || strcmp(mime_type, "text/plain") == 0 ||
            strcmp(mime_type, "UTF8_STRING") == 0 || strcmp(mime_type, "TEXT") == 0);
}

bool WaylandProtocol::InitializeWaylandConnection()
{
    wl_display_monitor = wl_display_connect(nullptr);
    if (!wl_display_monitor)
    {
        fprintf(stderr, "[Wayland] Failed to connect to Wayland display\n");
        return false;
    }

    wl_registry_monitor = wl_display_get_registry(wl_display_monitor);
    if (!wl_registry_monitor)
    {
        fprintf(stderr, "[Wayland] Failed to get registry\n");
        wl_display_disconnect(wl_display_monitor);
        wl_display_monitor = nullptr;
        return false;
    }

    wl_registry_add_listener(wl_registry_monitor, &registry_listener, this);

    // Roundtrip to receive registry globals
    wl_display_roundtrip(wl_display_monitor);

    if (!wl_seat_monitor)
    {
        fprintf(stderr, "[Wayland] WARNING: No wl_seat found\n");
        CleanupWaylandConnection();
        return false;
    }

    if (dc_type == DataControlType::None)
    {
        fprintf(stderr,
                "[Wayland] WARNING: No data-control protocol available "
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
        }
    }
    else if (dc_type == DataControlType::Wlr)
    {
        wlr_dc_device = zwlr_data_control_manager_v1_get_data_device(wlr_dc_manager, wl_seat_monitor);
        if (wlr_dc_device)
        {
            zwlr_data_control_device_v1_add_listener(wlr_dc_device, &wlr_device_listener, this);
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

    // Destroy current offers and any deferred destroys
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
        for (auto *offer : deferred_destroy_ext) ext_data_control_offer_v1_destroy(offer);
        deferred_destroy_ext.clear();
        for (auto *offer : deferred_destroy_wlr) zwlr_data_control_offer_v1_destroy(offer);
        deferred_destroy_wlr.clear();
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

void WaylandProtocol::RegistryGlobal(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                                     uint32_t version)
{
    WaylandProtocol *self = static_cast<WaylandProtocol *>(data);

    if (strcmp(interface, "wl_seat") == 0)
    {
        // Bind the first seat
        if (!self->wl_seat_monitor)
        {
            self->wl_seat_monitor =
                static_cast<struct wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
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

        // Destroy previous offer (defer if GetTextViaPrimary holds a reference)
        if (self->current_ext_offer)
        {
            if (self->offer_in_use)
            {
                // GetTextViaPrimary is reading from this offer — defer destruction
                // to the monitoring thread (Wayland protocol calls are not thread-safe)
                self->deferred_destroy_ext.push_back(self->current_ext_offer);
            }
            else
            {
                ext_data_control_offer_v1_destroy(self->current_ext_offer);
            }
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

void WaylandProtocol::ExtOfferOffer(void *data, struct ext_data_control_offer_v1 *offer, const char *mime_type)
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

        // Destroy previous offer (defer if GetTextViaPrimary holds a reference)
        if (self->current_wlr_offer)
        {
            if (self->offer_in_use)
            {
                // GetTextViaPrimary is reading from this offer — defer destruction
                // to the monitoring thread (Wayland protocol calls are not thread-safe)
                self->deferred_destroy_wlr.push_back(self->current_wlr_offer);
            }
            else
            {
                zwlr_data_control_offer_v1_destroy(self->current_wlr_offer);
            }
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

void WaylandProtocol::WlrOfferOffer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime_type)
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

    auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
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
        // Flush deferred offer destroys (must happen on monitoring thread)
        {
            std::lock_guard<std::mutex> lock(primary_offer_mutex);
            if (!offer_in_use)
            {
                for (auto *offer : deferred_destroy_ext) ext_data_control_offer_v1_destroy(offer);
                deferred_destroy_ext.clear();
                for (auto *offer : deferred_destroy_wlr) zwlr_data_control_offer_v1_destroy(offer);
                deferred_destroy_wlr.clear();
            }
        }

        // Check for pending read requests from main thread
        {
            std::lock_guard<std::mutex> lock(read_request_mutex);
            if (read_request.pending)
            {
                // Process the receive request
                const char *mime = "text/plain;charset=utf-8";

                if (dc_type == DataControlType::Ext && read_request.offer)
                {
                    ext_data_control_offer_v1_receive((struct ext_data_control_offer_v1 *)read_request.offer, mime,
                                                      read_request.write_fd);
                }
                else if (dc_type == DataControlType::Wlr && read_request.offer)
                {
                    zwlr_data_control_offer_v1_receive((struct zwlr_data_control_offer_v1 *)read_request.offer, mime,
                                                       read_request.write_fd);
                }

                wl_display_flush(wl_display_monitor);
                close(read_request.write_fd);
                read_request.write_fd = -1;
                read_request.pending = false;
                read_request.done = true;
            }
        }
        read_request_cv.notify_all();

        // Prepare for reading from Wayland fd.
        // wl_display_prepare_read returns -1 when there are pending events to dispatch.
        // After a fatal error (e.g. server disconnect), dispatch_pending returns -1 without
        // draining the queue, so we must check its return value to avoid an infinite loop.
        {
            bool ready = false;
            while (true)
            {
                if (wl_display_prepare_read(wl_display_monitor) == 0)
                {
                    ready = true;
                    break;
                }
                if (!wayland_monitoring_running || wl_display_dispatch_pending(wl_display_monitor) < 0)
                    break;
            }
            if (!ready)
            {
                // Shutdown requested or fatal Wayland error — no read lock held, safe to exit
                if (wayland_monitoring_running)
                    fprintf(stderr, "[Wayland] wl_display_dispatch_pending() failed, exiting monitoring thread\n");
                break;
            }
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
            fprintf(stderr, "[Wayland] select() error: %s\n", strerror(errno));
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
            fprintf(stderr, "[Wayland] wl_display_read_events() failed: %s\n", strerror(errno));
            break;
        }

        // Dispatch pending events
        if (wl_display_dispatch_pending(wl_display_monitor) < 0)
        {
            fprintf(stderr, "[Wayland] wl_display_dispatch_pending() failed: %s\n", strerror(errno));
            break;
        }
    }

    // Thread is exiting — flush any remaining deferred destroys
    {
        std::lock_guard<std::mutex> lock(primary_offer_mutex);
        for (auto *offer : deferred_destroy_ext) ext_data_control_offer_v1_destroy(offer);
        deferred_destroy_ext.clear();
        for (auto *offer : deferred_destroy_wlr) zwlr_data_control_offer_v1_destroy(offer);
        deferred_destroy_wlr.clear();
    }

    // Thread is exiting — unblock any pending GetTextViaPrimary that is waiting on read_request_cv.
    // Without this, GetTextViaPrimary would wait for its full 1s timeout if the thread exits
    // due to a fatal Wayland error while a read request is in flight.
    {
        std::lock_guard<std::mutex> lock(read_request_mutex);
        if (read_request.pending)
        {
            if (read_request.write_fd >= 0)
            {
                close(read_request.write_fd);
                read_request.write_fd = -1;
            }
            read_request.pending = false;
            read_request.done = true;
        }
    }
    read_request_cv.notify_all();
}

// ============================================================================
// GetTextViaPrimary - read text from PRIMARY selection via pipe
// ============================================================================

bool WaylandProtocol::GetTextViaPrimary(std::string &text)
{
    if (!initialized || dc_type == DataControlType::None)
        return false;

    void *offer_to_read = nullptr;

    // Step 1: Lock and check if we have a valid offer with text MIME.
    // Mark offer_in_use to prevent ExtDevicePrimarySelection/WlrDevicePrimarySelection
    // from destroying the offer while we are setting up the read request.
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

        offer_in_use = true;
    }

    // RAII guard to clear offer_in_use on all exit paths.
    // Deferred destroys are NOT flushed here — Wayland protocol calls (_destroy) must
    // happen on the monitoring thread.  The monitoring thread flushes them at the top of
    // its loop when it sees !offer_in_use and non-empty deferred vectors.
    struct OfferGuard
    {
        WaylandProtocol *self;
        ~OfferGuard()
        {
            std::lock_guard<std::mutex> lock(self->primary_offer_mutex);
            self->offer_in_use = false;
        }
    } offer_guard{this};

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
        bool ok = read_request_cv.wait_for(lock, std::chrono::seconds(1), [this] { return read_request.done; });
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
    bool is_mouse = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) || libevdev_has_event_code(dev, EV_REL, REL_X) ||
                    libevdev_has_event_code(dev, EV_REL, REL_Y) || libevdev_has_event_code(dev, EV_ABS, ABS_X) ||
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
        ev.events = EPOLLIN;  // Level-triggered mode (not EPOLLET) — avoids event loss from partial drain
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

            if (ev.code == ABS_X || ev.code == ABS_Y || ev.code == ABS_MT_POSITION_X || ev.code == ABS_MT_POSITION_Y)
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
        modifier_state.UpdateFromKeyCode(ev.code, is_press);

        // Build modifier flags bitmask
        int flags = modifier_state.GetFlags();

        KeyboardEventContext *keyboardEvent = new KeyboardEventContext();
        keyboardEvent->type = ev.type;
        keyboardEvent->code = ev.code;
        keyboardEvent->value = ev.value;
        keyboardEvent->flags = flags;

        keyboard_callback(callback_context, keyboardEvent);
    }
}

// ============================================================================
// Cursor position query
// ============================================================================

/**
 * Get cursor position via Hyprland IPC socket.
 * Sends "j/cursorpos" command and parses JSON response {"x":N,"y":N}.
 * Socket must be closed immediately to avoid Hyprland 5-second freeze.
 */
bool WaylandProtocol::GetCursorPositionHyprland(Point &pos)
{
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his)
        return false;

    // Build socket path: try XDG_RUNTIME_DIR first, then /tmp
    std::string socket_path;
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime)
    {
        socket_path = std::string(xdg_runtime) + "/hypr/" + his + "/.socket.sock";
    }

    // Check if socket exists, fall back to /tmp
    struct stat st;
    if (socket_path.empty() || stat(socket_path.c_str(), &st) != 0)
    {
        socket_path = std::string("/tmp/hypr/") + his + "/.socket.sock";
        if (stat(socket_path.c_str(), &st) != 0)
            return false;
    }

    // Create and connect unix domain socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return false;
    }

    // Set read timeout to avoid blocking the main thread if Hyprland IPC hangs
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000;  // 200ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send command: "j/cursorpos" (j flag = JSON output)
    const char *cmd = "j/cursorpos";
    if (write(sock, cmd, strlen(cmd)) < 0)
    {
        close(sock);
        return false;
    }

    // Read response (small JSON like {"x":123,"y":456})
    char buf[256];
    int total = 0;
    while (total < (int)sizeof(buf) - 1)
    {
        int n = read(sock, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += n;
    }
    buf[total] = '\0';

    // Close socket immediately to avoid Hyprland 5-second freeze
    close(sock);

    if (total == 0)
        return false;

    // Parse JSON: {"x":123,"y":456} - simple manual parsing
    int x = 0, y = 0;
    bool got_x = false, got_y = false;
    const char *p = buf;
    while (*p)
    {
        if (*p == '"')
        {
            p++;
            if (*p == 'x' && *(p + 1) == '"')
            {
                p += 2;  // skip x"
                while (*p == ' ' || *p == ':') p++;
                x = atoi(p);
                got_x = true;
            }
            else if (*p == 'y' && *(p + 1) == '"')
            {
                p += 2;  // skip y"
                while (*p == ' ' || *p == ':') p++;
                y = atoi(p);
                got_y = true;
            }
        }
        if (got_x && got_y)
            break;
        p++;
    }

    if (got_x && got_y)
    {
        pos = Point(x, y);
        return true;
    }
    return false;
}

/**
 * Load DBus functions via dlopen for KDE cursor position query.
 * Only attempts loading once; subsequent calls return cached result.
 */
bool WaylandProtocol::LoadDBusFunctions()
{
    if (dbus_tried)
        return dbus_fn.lib_handle != nullptr;
    dbus_tried = true;

    void *lib = dlopen("libdbus-1.so.3", RTLD_LAZY);
    if (!lib)
    {
        fprintf(stderr, "[Wayland] KDE: Failed to load libdbus-1.so.3: %s\n", dlerror());
        return false;
    }

    dbus_fn.lib_handle = lib;

// Load all required function pointers
#define LOAD_DBUS_FN(name, field)                                                         \
    dbus_fn.field = reinterpret_cast<decltype(dbus_fn.field)>(dlsym(lib, "dbus_" #name)); \
    if (!dbus_fn.field)                                                                   \
    {                                                                                     \
        fprintf(stderr, "[Wayland] KDE: Missing dbus_%s\n", #name);                       \
        goto fail;                                                                        \
    }

    LOAD_DBUS_FN(error_init, error_init);
    LOAD_DBUS_FN(error_free, error_free);
    LOAD_DBUS_FN(bus_get, bus_get);
    LOAD_DBUS_FN(message_new_method_call, message_new_method_call);
    LOAD_DBUS_FN(connection_send_with_reply_and_block, connection_send_with_reply_and_block);
    LOAD_DBUS_FN(message_iter_init, message_iter_init);
    LOAD_DBUS_FN(message_iter_recurse, message_iter_recurse);
    LOAD_DBUS_FN(message_iter_get_arg_type, message_iter_get_arg_type);
    LOAD_DBUS_FN(message_iter_get_basic, message_iter_get_basic);
    LOAD_DBUS_FN(message_iter_next, message_iter_next);
    LOAD_DBUS_FN(message_unref, message_unref);
    LOAD_DBUS_FN(bus_get_unique_name, bus_get_unique_name);
    LOAD_DBUS_FN(message_append_args, message_append_args);
    LOAD_DBUS_FN(connection_read_write, connection_read_write);
    LOAD_DBUS_FN(connection_pop_message, connection_pop_message);
    LOAD_DBUS_FN(message_get_member, message_get_member);

#undef LOAD_DBUS_FN

    // Get session bus connection
    {
        DBusError_ABI err;
        dbus_fn.error_init(&err);
        dbus_fn.session_bus = dbus_fn.bus_get(0 /* DBUS_BUS_SESSION */, &err);
        if (!dbus_fn.session_bus)
        {
            fprintf(stderr, "[Wayland] KDE: Failed to connect to session bus: %s\n", err.name ? err.name : "unknown");
            dbus_fn.error_free(&err);
            goto fail;
        }
        dbus_fn.error_free(&err);
    }

    // Get our unique bus name for KWin script callback
    {
        const char *name = dbus_fn.bus_get_unique_name(dbus_fn.session_bus);
        if (!name)
        {
            fprintf(stderr, "[Wayland] KDE: Failed to get unique bus name\n");
            goto fail;
        }
        kde_bus_name = name;
    }

    // Write temporary KWin script (reused until Cleanup)
    kde_script_path = "/tmp/selectionhook_kwin_cursor.js";
    {
        FILE *f = fopen(kde_script_path.c_str(), "w");
        if (!f)
        {
            fprintf(stderr, "[Wayland] KDE: Failed to write KWin script to %s\n", kde_script_path.c_str());
            kde_script_path.clear();
            goto fail;
        }
        fprintf(f,
                "let p = workspace.cursorPos;\n"
                "callDBus(\"%s\", \"/\", \"\", \"cursorpos\", String(p.x) + \",\" + String(p.y));\n",
                kde_bus_name.c_str());
        fclose(f);
    }

    return true;

fail:
    dlclose(lib);
    dbus_fn = DBusFunctions{};
    return false;
}

/**
 * Get cursor position via KDE KWin Scripting DBus API.
 *
 * Approach (inspired by kdotool): load a JS script into KWin that reads
 * workspace.cursorPos and calls back to our bus address via callDBus.
 *
 * Flow per call:
 * 1. loadScript(path, name) → script_id
 * 2. Run the script (auto-detected method, see kde_run_method):
 *    a. Per-script: /Scripting/Script{id} → org.kde.kwin.Script.run()
 *    b. Manager-level: /Scripting → org.kde.kwin.Scripting.start()
 * 3. Poll for incoming "cursorpos" method call (100ms timeout)
 * 4. unloadScript(name) to clean up
 * 5. Parse "x,y" string → Point
 */
bool WaylandProtocol::GetCursorPositionKDE(Point &pos)
{
    if (!LoadDBusFunctions())
        return false;

    if (kde_script_path.empty())
        return false;

    DBusError_ABI err;
    bool success = false;

    const char *script_name = "selectionhook_cursor";

    // Step 0: Unload any previously loaded script with the same name
    // (KWin keeps scripts across process restarts; loadScript returns -1 if name exists)
    {
        void *umsg =
            dbus_fn.message_new_method_call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
        if (umsg)
        {
            dbus_fn.message_append_args(umsg, 's', &script_name, '\0');
            dbus_fn.error_init(&err);
            void *ureply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, umsg, 200, &err);
            dbus_fn.message_unref(umsg);
            if (ureply)
                dbus_fn.message_unref(ureply);
            dbus_fn.error_free(&err);
        }
    }

    // Step 1: loadScript(String path, String name) → Int32 id
    void *msg = dbus_fn.message_new_method_call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript");
    if (!msg)
        return false;

    const char *script_path_cstr = kde_script_path.c_str();
    // DBUS_TYPE_STRING = 's' (0x73), DBUS_TYPE_INVALID = '\0'
    dbus_fn.message_append_args(msg, 's', &script_path_cstr, 's', &script_name, '\0');

    dbus_fn.error_init(&err);
    void *reply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, msg, 200, &err);
    dbus_fn.message_unref(msg);

    if (!reply)
    {
        dbus_fn.error_free(&err);
        return false;
    }
    dbus_fn.error_free(&err);

    // Parse script_id from reply (Int32)
    int32_t script_id = -1;
    {
        DBusMessageIter_ABI iter;
        if (dbus_fn.message_iter_init(reply, &iter) && dbus_fn.message_iter_get_arg_type(&iter) == 'i')
        {
            dbus_fn.message_iter_get_basic(&iter, &script_id);
        }
        dbus_fn.message_unref(reply);
    }

    if (script_id < 0)
        return false;

    // Step 2: Run the loaded script
    // On first call (kde_run_method == Unknown), probe both methods:
    //   1. Per-script: /Scripting/Script{id} → org.kde.kwin.Script.run()
    //      Standard KWin DBus interface. Each loaded script registers a DBus
    //      object at /Scripting/Script{id} via ScriptAdaptor, exposing run()/stop().
    //      This is how kdotool operates and matches KWin source for both Plasma 5 & 6.
    //   2. Manager-level: /Scripting → org.kde.kwin.Scripting.start()
    //      Runs all loaded-but-not-yet-started scripts. Used as fallback when
    //      per-script DBus objects are not reachable (observed on some Plasma 6
    //      distributions). Already-running scripts are protected by Script::run()'s
    //      internal `if (running()) return` guard, so this is safe even if other
    //      KWin scripts are loaded.
    // Once a method succeeds, kde_run_method is latched so subsequent calls skip
    // the probe — the KWin version won't change within a session.
    bool script_started = false;

    if (kde_run_method != KWinRunMethod::ManagerStart)
    {
        // Try per-script path: /Scripting/Script{id} → run()
        char script_obj_path[64];
        snprintf(script_obj_path, sizeof(script_obj_path), "/Scripting/Script%d", script_id);
        msg = dbus_fn.message_new_method_call("org.kde.KWin", script_obj_path, "org.kde.kwin.Script", "run");
        if (msg)
        {
            dbus_fn.error_init(&err);
            reply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, msg, 200, &err);
            dbus_fn.message_unref(msg);
            if (reply)
            {
                dbus_fn.message_unref(reply);
                script_started = true;
                kde_run_method = KWinRunMethod::PerScript;
            }
            dbus_fn.error_free(&err);
        }
    }

    if (!script_started && kde_run_method != KWinRunMethod::PerScript)
    {
        // Fallback: manager-level start() on /Scripting
        msg = dbus_fn.message_new_method_call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "start");
        if (msg)
        {
            dbus_fn.error_init(&err);
            reply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, msg, 200, &err);
            dbus_fn.message_unref(msg);
            if (reply)
            {
                dbus_fn.message_unref(reply);
                script_started = true;
                kde_run_method = KWinRunMethod::ManagerStart;
            }
            dbus_fn.error_free(&err);
        }
    }

    if (!script_started)
        goto cleanup;

    // Step 3: Poll for incoming "cursorpos" method call (100ms timeout)
    {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100))
        {
            dbus_fn.connection_read_write(dbus_fn.session_bus, 10);
            void *incoming = dbus_fn.connection_pop_message(dbus_fn.session_bus);
            if (!incoming)
                continue;

            const char *member = dbus_fn.message_get_member(incoming);
            if (member && strcmp(member, "cursorpos") == 0)
            {
                // Parse argument: single STRING "x,y"
                DBusMessageIter_ABI iter;
                if (dbus_fn.message_iter_init(incoming, &iter) && dbus_fn.message_iter_get_arg_type(&iter) == 's')
                {
                    const char *val = nullptr;
                    dbus_fn.message_iter_get_basic(&iter, &val);
                    if (val)
                    {
                        int x = 0, y = 0;
                        if (sscanf(val, "%d,%d", &x, &y) == 2)
                        {
                            pos = Point(x, y);
                            success = true;
                        }
                    }
                }
                dbus_fn.message_unref(incoming);
                break;
            }
            dbus_fn.message_unref(incoming);
        }
    }

cleanup:
    // Step 4: unloadScript to clean up
    {
        void *umsg =
            dbus_fn.message_new_method_call("org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript");
        if (umsg)
        {
            dbus_fn.message_append_args(umsg, 's', &script_name, '\0');
            dbus_fn.error_init(&err);
            void *ureply = dbus_fn.connection_send_with_reply_and_block(dbus_fn.session_bus, umsg, 200, &err);
            dbus_fn.message_unref(umsg);
            if (ureply)
                dbus_fn.message_unref(ureply);
            dbus_fn.error_free(&err);
        }
    }

    return success;
}

/**
 * Initialize XWayland connection and detect scale factor.
 * Called lazily on first cursor position query. Detects the XWayland scale factor
 * by reading Xft.dpi and GDK_SCALE to match Electron/Chromium's screenToDipPoint() formula:
 *   scale = max(1, gdk_monitor_get_scale_factor) × (Xft.dpi / 96.0)
 * On non-GNOME XWayland, gdk_monitor_get_scale_factor comes from GDK_SCALE env var (default 1).
 */
void WaylandProtocol::EnsureXWaylandInitialized()
{
    if (xwayland_tried)
        return;
    xwayland_tried = true;

    const char *display_env = getenv("DISPLAY");
    if (!display_env)
    {
        fprintf(stderr, "[Wayland] XWayland: DISPLAY not set, fallback unavailable\n");
        return;
    }

    xwayland_display = XOpenDisplay(display_env);
    if (!xwayland_display)
    {
        fprintf(stderr, "[Wayland] XWayland: Failed to open display %s\n", display_env);
        return;
    }

    // Detect XWayland scale factor from Xft.dpi X resource
    double xft_dpi = 96.0;
    char *rms = XResourceManagerString(xwayland_display);
    if (rms)
    {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db)
        {
            char *type = nullptr;
            XrmValue value;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value))
            {
                double dpi = atof(value.addr);
                if (dpi > 0)
                {
                    xft_dpi = dpi;
                }
            }
            XrmDestroyDatabase(db);
        }
    }

    // Check GDK_SCALE env var (accounts for manual user configuration)
    int gdk_scale = 1;
    const char *gdk_scale_env = getenv("GDK_SCALE");
    if (gdk_scale_env)
    {
        int val = atoi(gdk_scale_env);
        if (val > 1)
            gdk_scale = val;
    }

    xwayland_scale = gdk_scale * (xft_dpi / 96.0);

    if (xwayland_scale != 1.0)
    {
        printf("[Wayland] XWayland scale factor: %.2f (Xft.dpi=%.0f, GDK_SCALE=%d)\n", xwayland_scale, xft_dpi,
               gdk_scale);
    }
}

/**
 * Get cursor position via XWayland (XQueryPointer on XWayland display).
 * Note: cursor position may freeze when cursor is over native Wayland windows.
 */
bool WaylandProtocol::GetCursorPositionXWayland(Point &pos)
{
    EnsureXWaylandInitialized();

    if (!xwayland_display)
        return false;

    Window root_return, child_return;
    int root_x, root_y, win_x, win_y;
    unsigned int mask_return;

    if (XQueryPointer(xwayland_display, DefaultRootWindow(xwayland_display), &root_return, &child_return, &root_x,
                      &root_y, &win_x, &win_y, &mask_return))
    {
        pos = Point(root_x, root_y);
        return true;
    }

    return false;
}

/**
 * Get current mouse position using the best available method.
 * Fallback chain: Compositor IPC → XWayland → libevdev accumulated value.
 *
 * Compositor IPC returns logical coordinates (DIP), while XWayland XQueryPointer
 * returns X11 screen coordinates. When XWayland uses app-driven scaling (e.g., KDE default),
 * these differ by xwayland_scale. We convert IPC coordinates to X11 screen coordinates
 * so that Electron's screenToDipPoint() can correctly convert back to DIP.
 */
Point WaylandProtocol::GetCurrentMousePosition()
{
    EnsureXWaylandInitialized();

    Point pos;

    // 1. Compositor-specific IPC (returns logical coordinates)
    bool from_compositor_ipc = false;
    switch (env_info.compositorType)
    {
        case CompositorType::Hyprland:
            from_compositor_ipc = GetCursorPositionHyprland(pos);
            break;
        case CompositorType::KWin:
            from_compositor_ipc = GetCursorPositionKDE(pos);
            break;
        // Sway, Mutter, Wlroots, CosmicComp, Unknown → fall through to XWayland
        default:
            break;
    }

    if (from_compositor_ipc)
    {
        // Convert logical coordinates to XWayland screen coordinates.
        // This matches Electron's screenToDipPoint() which divides by the same scale factor,
        // ensuring: logical × scale / scale = logical (correct DIP).
        // When compositor upscaling is used (Mode A), xwayland_scale is 1.0 (no-op).
        if (xwayland_scale != 1.0)
        {
            pos.x = static_cast<int>(std::round(pos.x * xwayland_scale));
            pos.y = static_cast<int>(std::round(pos.y * xwayland_scale));
        }
        return pos;
    }

    // 2. XWayland fallback (already in X11 screen coordinates)
    if (GetCursorPositionXWayland(pos))
        return pos;

    // 3. Last resort: libevdev accumulated value (inaccurate but non-zero)
    return current_mouse_pos;
}

// Factory function to create WaylandProtocol instance
std::unique_ptr<ProtocolBase> CreateWaylandProtocol()
{
    return std::make_unique<WaylandProtocol>();
}
