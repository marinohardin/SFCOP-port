/* The DirectInput module seen from C: the functions the guest shims (DinputGuest.c) call, since
 * the lift's headers are C (src/hle/README.md, "Implementing a module"). Objects are named by
 * handles, small numbers the shims keep in the guest COM objects; 0 is never a handle. Every
 * function returns the HRESULT the game reads, as the C++ method it calls (DirectInput.h,
 * KeyboardDevice.h) documents. Structures here are host forms; the shims convert to and from the
 * game's layouts. A function given a handle that is not live returns DIERR_INVALIDPARAM.
 *
 * Valid C11 and C++17. The C++ part at the end is how the runtime gives the module its
 * components. */
#ifndef SFCOP_HLE_DINPUT_DINPUTBRIDGE_H
#define SFCOP_HLE_DINPUT_DINPUTBRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sfcop_dinput_device_instance {
    uint8_t instance[16]; /* GUIDs in their 16-byte memory form */
    uint8_t product[16];
    uint32_t dev_type;
    char instance_name[260];
    char product_name[260];
} sfcop_dinput_device_instance;

typedef struct sfcop_dinput_object_instance {
    uint8_t guid_type[16];
    uint32_t ofs;
    uint32_t type;
    uint32_t flags;
    char name[260];
} sfcop_dinput_object_instance;

typedef struct sfcop_dinput_caps {
    uint32_t flags, dev_type, axes, buttons, povs;
} sfcop_dinput_caps;

typedef struct sfcop_dinput_prop_header {
    uint32_t size, header_size, obj, how;
} sfcop_dinput_prop_header;

typedef struct sfcop_dinput_object_format {
    int has_guid; /* 0: the game's pguid was null */
    uint8_t guid[16];
    uint32_t ofs, type, flags;
} sfcop_dinput_object_format;

typedef struct sfcop_dinput_format {
    uint32_t size, obj_size, flags, data_size, num_objs;
    const sfcop_dinput_object_format *objects; /* num_objs of them */
} sfcop_dinput_format;

/* What the two Release functions return for a handle that is not live (never made, or already
 * released to zero): no count is ever this large. The shim frees its guest object only when the
 * count returned is 0, and answers 0 without freeing for this. */
#define SFCOP_DINPUT_UNKNOWN_HANDLE 0xFFFFFFFFu

/* An enumeration callback: nonzero to continue. */
typedef int (*sfcop_dinput_device_callback)(const sfcop_dinput_device_instance *device,
                                            void *context);
typedef int (*sfcop_dinput_object_callback)(const sfcop_dinput_object_instance *object,
                                            void *context);

/* IDirectInputA */
uint32_t sfcop_dinput_create(uint32_t version, int has_outer, uint32_t *handle);
uint32_t sfcop_dinput_add_ref(uint32_t handle);
uint32_t sfcop_dinput_release(uint32_t handle);
uint32_t sfcop_dinput_create_device(uint32_t handle, const uint8_t *guid, int has_outer,
                                    uint32_t *device);
uint32_t sfcop_dinput_enum_devices(uint32_t handle, uint32_t dev_type,
                                   sfcop_dinput_device_callback callback, void *context,
                                   uint32_t flags);
uint32_t sfcop_dinput_get_device_status(uint32_t handle, const uint8_t *guid);
uint32_t sfcop_dinput_run_control_panel(uint32_t handle, uint32_t flags);
uint32_t sfcop_dinput_initialize(uint32_t handle, uint32_t version);
/* Every live DirectInput object's DirectInput::Pump(). */
void sfcop_dinput_pump(void);

/* IDirectInputDeviceA */
uint32_t sfcop_dinput_device_add_ref(uint32_t device);
uint32_t sfcop_dinput_device_release(uint32_t device);
uint32_t sfcop_dinput_device_get_capabilities(uint32_t device, uint32_t size,
                                              sfcop_dinput_caps *caps);
uint32_t sfcop_dinput_device_enum_objects(uint32_t device, sfcop_dinput_object_callback callback,
                                          void *context, uint32_t flags);
uint32_t sfcop_dinput_device_get_property(uint32_t device, uint32_t property,
                                          const sfcop_dinput_prop_header *header, uint32_t *value);
uint32_t sfcop_dinput_device_set_property(uint32_t device, uint32_t property,
                                          const sfcop_dinput_prop_header *header, uint32_t value);
uint32_t sfcop_dinput_device_acquire(uint32_t device);
uint32_t sfcop_dinput_device_unacquire(uint32_t device);
uint32_t sfcop_dinput_device_get_state(uint32_t device, uint32_t size, uint8_t *data);
/* `records`, if not null, has room for *in_out records of `element_size` bytes in the game's
 * DIDEVICEOBJECTDATA layout (dwOfs, dwData, dwTimeStamp, dwSequence as little-endian 32-bit
 * words, then zeros); at most as many as are buffered are written. */
uint32_t sfcop_dinput_device_get_data(uint32_t device, uint32_t element_size, void *records,
                                      uint32_t *in_out, uint32_t flags);
uint32_t sfcop_dinput_device_set_data_format(uint32_t device, const sfcop_dinput_format *format);
/* `event` is the game's HANDLE (0: none), resolved to a Threads event through the runtime. */
uint32_t sfcop_dinput_device_set_event_notification(uint32_t device, uint32_t event);
uint32_t sfcop_dinput_device_set_cooperative_level(uint32_t device, int has_window, uint32_t flags);
uint32_t sfcop_dinput_device_get_object_info(uint32_t device, uint32_t size,
                                             sfcop_dinput_object_instance *info, uint32_t obj,
                                             uint32_t how);
uint32_t sfcop_dinput_device_get_device_info(uint32_t device, uint32_t size,
                                             sfcop_dinput_device_instance *info);
uint32_t sfcop_dinput_device_run_control_panel(uint32_t device, uint32_t flags);
uint32_t sfcop_dinput_device_initialize(uint32_t device, uint32_t version, const uint8_t *guid);

#ifdef __cplusplus
} /* extern "C" */

#include "platform/Input/Input.h"
#include "platform/Threads/Threads.h"

#include <functional>
#include <optional>

namespace sfcop::hle::dinput {

// The components the C functions above work on, given by the runtime before the game runs.
struct Platform {
    platform::Input *input = nullptr;     // the window's InputSink
    platform::Threads *threads = nullptr; // for SetEventNotification's event
    // The Threads event behind a game's event HANDLE (the kernel module's handle table), or
    // std::nullopt if it names none. Without it a notification event is accepted and never
    // signalled.
    std::function<std::optional<platform::Waitable>(std::uint32_t)> waitableOfHandle;
};

// Replaces the components for objects created from now on. Until it is called, objects work on
// an Input of their own that no window feeds (every key reads up) and a Threads stub.
void setPlatform(Platform platform);

} // namespace sfcop::hle::dinput
#endif

#endif
