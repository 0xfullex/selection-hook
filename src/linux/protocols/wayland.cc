/**
 * Wayland Protocol Implementation for Linux Selection Hook
 *
 * This file contains Wayland-specific implementations for text selection,
 * clipboard operations, and window management.
 */

#include <atomic>
#include <climits>
#include <cstring>
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
#include <sys/stat.h>
#include <unistd.h>

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
    void *callback_context;

    // libevdev helper methods
    bool InitializeInputDevices();
    void CleanupInputDevices();
    bool IsInputDevice(const std::string &device_path);
    bool SetupInputDevice(const std::string &device_path);
    void ProcessLibevdevEvent(const struct input_event &ev, const InputDevice &device);
    void InputMonitoringThreadProc();

  public:
    WaylandProtocol()
        : initialized(false),
          epoll_fd(-1),
          mouse_callback(nullptr),
          keyboard_callback(nullptr),
          callback_context(nullptr)
    {
        current_mouse_pos = Point(0, 0);
    }

    ~WaylandProtocol() override { Cleanup(); }

    // Protocol identification
    DisplayProtocol GetProtocol() const override { return DisplayProtocol::Wayland; }

    // Initialization and cleanup
    bool Initialize() override
    {
        // TODO: Initialize Wayland connection
        // This would involve connecting to the Wayland display and setting up protocols
        // For now, we'll just mark it as initialized
        initialized = true;
        return true;
    }

    void Cleanup() override
    {
        // Stop input monitoring first
        StopInputMonitoring();
        CleanupInputMonitoring();

        // TODO: Cleanup Wayland resources
        initialized = false;
    }

    // Window management
    uint64_t GetActiveWindow() override
    {
        if (!initialized)
            return 0;

        // TODO: Implement Wayland-specific active window retrieval
        // This is more complex in Wayland as there's no global window concept
        // Would need to use compositor-specific protocols or other methods
        return 0;
    }

    bool GetProgramNameFromWindow(uint64_t window, std::string &programName) override
    {
        if (!initialized || !window)
            return false;

        // TODO: Implement Wayland-specific program name retrieval
        // This might involve using app_id from toplevel surfaces
        programName = "wayland-app";  // Placeholder
        return false;
    }

    // Text selection
    bool GetTextViaPrimary(std::string &text) override
    {
        if (!initialized)
            return false;

        // TODO: Implement Wayland selection reading
        // This would use wl_data_device_manager and wl_data_source protocols
        return false;
    }

    // bool SetTextRangeCoordinates(uint64_t window, TextSelectionInfo &selectionInfo) override
    // {
    //     if (!initialized || !window)
    //         return false;

    //     // TODO: Implement Wayland-specific coordinate retrieval
    //     // This would be more complex as Wayland doesn't expose global coordinates
    //     return false;
    // }

    // Clipboard operations
    bool WriteClipboard(const std::string &text) override
    {
        if (!initialized)
            return false;

        // TODO: Implement Wayland clipboard writing
        // This would use wl_data_device_manager to set selection
        return false;
    }

    bool ReadClipboard(std::string &text) override
    {
        if (!initialized)
            return false;

        // TODO: Implement Wayland clipboard reading
        // This would use wl_data_device_manager to get selection
        return false;
    }

    // // Key operations
    // void SendCopyKey(CopyKeyType type) override
    // {
    //     if (!initialized)
    //         return;

    //     // TODO: Implement Wayland key sending
    //     // This would use virtual input protocols or other methods
    // }

    // bool ShouldKeyInterruptViaClipboard() override
    // {
    //     if (!initialized)
    //         return false;

    //     // TODO: Implement Wayland-specific key state checking
    //     return false;
    // }

    // Input monitoring implementation
    bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                   void *context) override
    {
        if (!initialized)
            return false;

        mouse_callback = mouseCallback;
        keyboard_callback = keyboardCallback;
        callback_context = context;

        return InitializeInputDevices();
    }

    void CleanupInputMonitoring() override
    {
        CleanupInputDevices();
        mouse_callback = nullptr;
        keyboard_callback = nullptr;
        callback_context = nullptr;
    }

    bool StartInputMonitoring() override
    {
        if (!initialized || input_monitoring_running)
            return false;

        if (input_devices.empty() || epoll_fd < 0)
            return false;

        input_monitoring_running = true;
        input_monitoring_thread = std::thread(&WaylandProtocol::InputMonitoringThreadProc, this);
        return true;
    }

    void StopInputMonitoring() override
    {
        input_monitoring_running = false;
        if (input_monitoring_thread.joinable())
        {
            input_monitoring_thread.join();
        }
    }
};

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

    // Check if device has mouse or keyboard capabilities
    bool is_mouse = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) || libevdev_has_event_code(dev, EV_REL, REL_X) ||
                    libevdev_has_event_code(dev, EV_REL, REL_Y);

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
    }

    // Handle keyboard events
    if (device.is_keyboard && keyboard_callback && ev.type == EV_KEY)
    {
        KeyboardEventContext *keyboardEvent = new KeyboardEventContext();
        keyboardEvent->type = ev.type;
        keyboardEvent->code = ev.code;
        keyboardEvent->value = ev.value;
        keyboardEvent->flags = 0;  // TODO: Add modifier flags

        keyboard_callback(callback_context, keyboardEvent);
    }
}

// Factory function to create WaylandProtocol instance
std::unique_ptr<ProtocolBase> CreateWaylandProtocol()
{
    return std::make_unique<WaylandProtocol>();
}
