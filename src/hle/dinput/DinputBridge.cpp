#include "hle/dinput/DinputBridge.h"

#include "hle/dinput/DirectInput.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sfcop::hle::dinput {
namespace {

struct Registry {
    std::mutex mutex;
    Platform platform;
    std::unique_ptr<platform::Input> ownInput;
    std::unique_ptr<platform::Threads> ownThreads;
    std::uint32_t nextHandle = 1;
    std::map<std::uint32_t, DirectInput *> inputs;
    std::map<std::uint32_t, KeyboardDevice *> devices;
};

Registry &registry() {
    static Registry instance;
    return instance;
}

// The components new objects use: the runtime's, or an unfed Input and a Threads stub. The
// caller holds the registry's mutex.
Platform currentPlatformLocked(Registry &r) {
    Platform chosen = r.platform;
    if (chosen.input == nullptr) {
        if (!r.ownInput) {
            r.ownInput = platform::makeInput();
        }
        chosen.input = r.ownInput.get();
    }
    if (chosen.threads == nullptr) {
        if (!r.ownThreads) {
            r.ownThreads = platform::makeThreadsStub();
        }
        chosen.threads = r.ownThreads.get();
    }
    return chosen;
}

std::uint32_t addLocked(Registry &r, DirectInput *object) {
    std::uint32_t handle = r.nextHandle++;
    r.inputs[handle] = object;
    return handle;
}

std::uint32_t addLocked(Registry &r, KeyboardDevice *object) {
    std::uint32_t handle = r.nextHandle++;
    r.devices[handle] = object;
    return handle;
}

// The raw pointers the registry hands out assume Input and Threads outlive every object made on
// them and that the guest is single-threaded: no object is released while another call uses it.
DirectInput *findInput(std::uint32_t handle) {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.inputs.find(handle);
    return it == r.inputs.end() ? nullptr : it->second;
}

KeyboardDevice *findDevice(std::uint32_t handle) {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.devices.find(handle);
    return it == r.devices.end() ? nullptr : it->second;
}

GUID guidFromBytes(const std::uint8_t *bytes) {
    GUID guid{};
    std::memcpy(&guid.Data1, bytes, 4);
    std::memcpy(&guid.Data2, bytes + 4, 2);
    std::memcpy(&guid.Data3, bytes + 6, 2);
    std::memcpy(guid.Data4, bytes + 8, 8);
    return guid;
}

void guidToBytes(const GUID &guid, std::uint8_t *bytes) {
    std::memcpy(bytes, &guid.Data1, 4);
    std::memcpy(bytes + 4, &guid.Data2, 2);
    std::memcpy(bytes + 6, &guid.Data3, 2);
    std::memcpy(bytes + 8, guid.Data4, 8);
}

void copyName(const std::string &from, char (&to)[260]) {
    std::size_t length = (std::min)(from.size(), sizeof to - 1);
    std::memcpy(to, from.data(), length);
    to[length] = '\0';
}

sfcop_dinput_device_instance toC(const DeviceInstance &device) {
    sfcop_dinput_device_instance out{};
    guidToBytes(device.instance, out.instance);
    guidToBytes(device.product, out.product);
    out.dev_type = device.devType;
    copyName(device.instanceName, out.instance_name);
    copyName(device.productName, out.product_name);
    return out;
}

sfcop_dinput_object_instance toC(const ObjectInstance &object) {
    sfcop_dinput_object_instance out{};
    guidToBytes(object.guidType, out.guid_type);
    out.ofs = object.ofs;
    out.type = object.type;
    out.flags = object.flags;
    copyName(object.name, out.name);
    return out;
}

// A 32-bit little-endian word, as the game's structures hold it.
void putWord(std::uint8_t *at, std::uint32_t value) {
    at[0] = static_cast<std::uint8_t>(value);
    at[1] = static_cast<std::uint8_t>(value >> 8);
    at[2] = static_cast<std::uint8_t>(value >> 16);
    at[3] = static_cast<std::uint8_t>(value >> 24);
}

PropertyHeader fromC(const sfcop_dinput_prop_header *header) {
    PropertyHeader out;
    if (header != nullptr) {
        out.size = header->size;
        out.headerSize = header->header_size;
        out.obj = header->obj;
        out.how = header->how;
    }
    return out;
}

} // namespace

void setPlatform(Platform platform) {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.platform = std::move(platform);
}

} // namespace sfcop::hle::dinput

using namespace sfcop::hle::dinput;

extern "C" {

uint32_t sfcop_dinput_create(uint32_t version, int has_outer, uint32_t *handle) {
    if (handle == nullptr) {
        return kDiErrInvalidParam;
    }
    *handle = 0;
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    Platform chosen = currentPlatformLocked(r);
    DirectInput *object = nullptr;
    HResult result =
        DirectInput::Create(version, has_outer != 0, *chosen.input, *chosen.threads, &object);
    if (result == kDiOk) {
        *handle = addLocked(r, object);
    }
    return result;
}

uint32_t sfcop_dinput_add_ref(uint32_t handle) {
    DirectInput *object = findInput(handle);
    return object == nullptr ? 0 : object->AddRef();
}

uint32_t sfcop_dinput_release(uint32_t handle) {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.inputs.find(handle);
    if (it == r.inputs.end()) {
        return SFCOP_DINPUT_UNKNOWN_HANDLE;
    }
    std::uint32_t left = it->second->Release();
    if (left == 0) {
        r.inputs.erase(it);
    }
    return left;
}

uint32_t sfcop_dinput_create_device(uint32_t handle, const uint8_t *guid, int has_outer,
                                    uint32_t *device) {
    DirectInput *object = findInput(handle);
    if (object == nullptr || device == nullptr) {
        return kDiErrInvalidParam;
    }
    *device = 0;
    std::optional<GUID> wanted;
    if (guid != nullptr) {
        wanted = guidFromBytes(guid);
    }
    KeyboardDevice *created = nullptr;
    HResult result = object->CreateDevice(wanted ? &*wanted : nullptr, &created, has_outer != 0);
    if (result == kDiOk) {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        *device = addLocked(r, created);
    }
    return result;
}

uint32_t sfcop_dinput_enum_devices(uint32_t handle, uint32_t dev_type,
                                   sfcop_dinput_device_callback callback, void *context,
                                   uint32_t flags) {
    DirectInput *object = findInput(handle);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    std::function<bool(const DeviceInstance &)> each;
    if (callback != nullptr) {
        each = [callback, context](const DeviceInstance &device) {
            sfcop_dinput_device_instance c = toC(device);
            return callback(&c, context) != 0;
        };
    }
    return object->EnumDevices(dev_type, each, flags);
}

uint32_t sfcop_dinput_get_device_status(uint32_t handle, const uint8_t *guid) {
    DirectInput *object = findInput(handle);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    if (guid == nullptr) {
        return object->GetDeviceStatus(nullptr);
    }
    GUID wanted = guidFromBytes(guid);
    return object->GetDeviceStatus(&wanted);
}

uint32_t sfcop_dinput_run_control_panel(uint32_t handle, uint32_t flags) {
    DirectInput *object = findInput(handle);
    return object == nullptr ? kDiErrInvalidParam : object->RunControlPanel(flags);
}

uint32_t sfcop_dinput_initialize(uint32_t handle, uint32_t version) {
    DirectInput *object = findInput(handle);
    return object == nullptr ? kDiErrInvalidParam : object->Initialize(version);
}

void sfcop_dinput_pump(void) {
    std::vector<DirectInput *> live;
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const auto &entry : r.inputs) {
            live.push_back(entry.second);
        }
    }
    for (DirectInput *object : live) {
        object->Pump();
    }
}

uint32_t sfcop_dinput_device_add_ref(uint32_t device) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? 0 : object->AddRef();
}

uint32_t sfcop_dinput_device_release(uint32_t device) {
    Registry &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.devices.find(device);
    if (it == r.devices.end()) {
        return SFCOP_DINPUT_UNKNOWN_HANDLE;
    }
    std::uint32_t left = it->second->Release();
    if (left == 0) {
        r.devices.erase(it);
    }
    return left;
}

uint32_t sfcop_dinput_device_get_capabilities(uint32_t device, uint32_t size,
                                              sfcop_dinput_caps *caps) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    DeviceCaps host;
    HResult result = object->GetCapabilities(size, caps == nullptr ? nullptr : &host);
    if (result == kDiOk) {
        *caps = sfcop_dinput_caps{host.flags, host.devType, host.axes, host.buttons, host.povs};
    }
    return result;
}

uint32_t sfcop_dinput_device_enum_objects(uint32_t device, sfcop_dinput_object_callback callback,
                                          void *context, uint32_t flags) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    std::function<bool(const ObjectInstance &)> each;
    if (callback != nullptr) {
        each = [callback, context](const ObjectInstance &key) {
            sfcop_dinput_object_instance c = toC(key);
            return callback(&c, context) != 0;
        };
    }
    return object->EnumObjects(each, flags);
}

uint32_t sfcop_dinput_device_get_property(uint32_t device, uint32_t property,
                                          const sfcop_dinput_prop_header *header, uint32_t *value) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr || header == nullptr) {
        return kDiErrInvalidParam;
    }
    return object->GetProperty(property, fromC(header), value);
}

uint32_t sfcop_dinput_device_set_property(uint32_t device, uint32_t property,
                                          const sfcop_dinput_prop_header *header, uint32_t value) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr || header == nullptr) {
        return kDiErrInvalidParam;
    }
    return object->SetProperty(property, fromC(header), value);
}

uint32_t sfcop_dinput_device_acquire(uint32_t device) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? kDiErrInvalidParam : object->Acquire();
}

uint32_t sfcop_dinput_device_unacquire(uint32_t device) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? kDiErrInvalidParam : object->Unacquire();
}

uint32_t sfcop_dinput_device_get_state(uint32_t device, uint32_t size, uint8_t *data) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? kDiErrInvalidParam : object->GetDeviceState(size, data);
}

uint32_t sfcop_dinput_device_get_data(uint32_t device, uint32_t element_size, void *records,
                                      uint32_t *in_out, uint32_t flags) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    if (records == nullptr || in_out == nullptr) {
        return object->GetDeviceData(element_size, nullptr, in_out, flags);
    }
    // Size the host copy by what is buffered, not by the caller's count (which may be INFINITE).
    std::uint32_t available = kInfinite;
    HResult result = object->GetDeviceData(element_size, nullptr, &available, kGetDataPeek);
    if (result != kDiOk && result != kDiBufferOverflow) {
        return result;
    }
    std::vector<DeviceObjectData> host((std::min)(available, *in_out));
    *in_out = static_cast<std::uint32_t>(host.size());
    result = object->GetDeviceData(element_size, host.data(), in_out, flags);
    if (result == kDiOk || result == kDiBufferOverflow) {
        auto *out = static_cast<std::uint8_t *>(records);
        for (std::uint32_t i = 0; i < *in_out; ++i) {
            std::uint8_t *at = out + static_cast<std::size_t>(i) * element_size;
            std::memset(at, 0, element_size);
            putWord(at, host[i].ofs);
            putWord(at + 4, host[i].data);
            putWord(at + 8, host[i].timeStamp);
            putWord(at + 12, host[i].sequence);
        }
    }
    return result;
}

uint32_t sfcop_dinput_device_set_data_format(uint32_t device, const sfcop_dinput_format *format) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    if (format == nullptr) {
        return object->SetDataFormat(nullptr);
    }
    DataFormat host;
    host.size = format->size;
    host.objSize = format->obj_size;
    host.flags = format->flags;
    host.dataSize = format->data_size;
    if (format->num_objs != 0 && format->objects == nullptr) {
        return kDiErrInvalidParam;
    }
    for (std::uint32_t i = 0; i < format->num_objs; ++i) {
        const sfcop_dinput_object_format &c = format->objects[i];
        ObjectFormat entry;
        if (c.has_guid) {
            entry.guid = guidFromBytes(c.guid);
        }
        entry.ofs = c.ofs;
        entry.type = c.type;
        entry.flags = c.flags;
        host.objects.push_back(entry);
    }
    return object->SetDataFormat(&host);
}

uint32_t sfcop_dinput_device_set_event_notification(uint32_t device, uint32_t event) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    std::optional<sfcop::platform::Waitable> waitable;
    if (event != 0) {
        Registry &r = registry();
        std::function<std::optional<sfcop::platform::Waitable>(std::uint32_t)> resolve;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            resolve = r.platform.waitableOfHandle;
        }
        if (resolve) {
            waitable = resolve(event);
            if (!waitable) {
                return kDiErrInvalidParam; // not an event handle
            }
        }
    }
    return object->SetEventNotification(waitable);
}

uint32_t sfcop_dinput_device_set_cooperative_level(uint32_t device, int has_window,
                                                   uint32_t flags) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? kDiErrInvalidParam
                             : object->SetCooperativeLevel(has_window != 0, flags);
}

uint32_t sfcop_dinput_device_get_object_info(uint32_t device, uint32_t size,
                                             sfcop_dinput_object_instance *info, uint32_t obj,
                                             uint32_t how) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    ObjectInstance host;
    HResult result = object->GetObjectInfo(size, info == nullptr ? nullptr : &host, obj, how);
    if (result == kDiOk) {
        *info = toC(host);
    }
    return result;
}

uint32_t sfcop_dinput_device_get_device_info(uint32_t device, uint32_t size,
                                             sfcop_dinput_device_instance *info) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    DeviceInstance host;
    HResult result = object->GetDeviceInfo(size, info == nullptr ? nullptr : &host);
    if (result == kDiOk) {
        *info = toC(host);
    }
    return result;
}

uint32_t sfcop_dinput_device_run_control_panel(uint32_t device, uint32_t flags) {
    KeyboardDevice *object = findDevice(device);
    return object == nullptr ? kDiErrInvalidParam : object->RunControlPanel(flags);
}

uint32_t sfcop_dinput_device_initialize(uint32_t device, uint32_t version, const uint8_t *guid) {
    KeyboardDevice *object = findDevice(device);
    if (object == nullptr) {
        return kDiErrInvalidParam;
    }
    if (guid == nullptr) {
        return object->Initialize(version, nullptr);
    }
    GUID wanted = guidFromBytes(guid);
    return object->Initialize(version, &wanted);
}

} // extern "C"
