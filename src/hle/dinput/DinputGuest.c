/* The game's side of DirectInput (src/hle/README.md, "Implementing a module"): one stdcall shim
 * per surface row of DINPUT.DLL, IDirectInputA and IDirectInputDeviceA, in win32hle's form. Each
 * reads its arguments from the guest stack, turns guest structures into the bridge's host forms
 * (DinputBridge.h), calls the module and writes the results back into the game's layouts, then
 * purges the stack bytes surface.tsv gives. Built only with the lift.
 *
 * A guest COM object made here is hle_com_new()'s: its vtable, a reference count at +4 (kept
 * equal to the module's), and the module's handle at +8. */
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"

#include "hle/dinput/DinputBridge.h"

#define DI_OK 0x00000000u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_OUTOFMEMORY 0x8007000Eu
#define E_NOINTERFACE 0x80004002u

#define OBJECT_SIZE 12u
#define HANDLE_OF(self) MEM32((self) + 8)
#define MAX_FORMAT_OBJECTS 1024u  /* c_dfDIKeyboard has 256 */
#define DEVICE_INSTANCE_SIZE 580u /* DIDEVICEINSTANCEA, for interface versions 5 to 7 */
#define OBJECT_INSTANCE_SIZE 316u /* DIDEVICEOBJECTINSTANCEA */

static const uint8_t kIID_IUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t kIID_IDirectInputA[16] = {0x60, 0x13, 0x52, 0x89, 0x8A, 0xAA, 0xCF, 0x11,
                                               0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t kIID_IDirectInputDeviceA[16] = {
    0x80, 0xE6, 0x44, 0x59, 0x2E, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};

static uint32_t g_vt_input, g_vt_device;

/* A guest GUID pointer as 16 bytes, or NULL. */
static const uint8_t *guid_at(uint32_t va) {
    return va ? (const uint8_t *)hle_ptr(va) : NULL;
}

static int same_iid(uint32_t va, const uint8_t *iid) {
    return va && !memcmp(hle_ptr(va), iid, 16);
}

/* The QueryInterface both interfaces share: this interface or IUnknown only, since the game never
 * asks for another and the objects have no other vtable. */
static void query_interface(const uint8_t *iid, uint32_t (*add_ref)(uint32_t)) {
    uint32_t self = A32(0), riid = A32(1), out = A32(2);
    if (!out)
        RET(DIERR_INVALIDPARAM, 3);
    if (same_iid(riid, kIID_IUnknown) || same_iid(riid, iid)) {
        MEM32(self + 4) = add_ref(HANDLE_OF(self));
        MEM32(out) = self;
        RET(DI_OK, 3);
    }
    MEM32(out) = 0;
    RET(E_NOINTERFACE, 3);
}

/* ---- IDirectInputA ---- */

static void di_QueryInterface(void) {
    query_interface(kIID_IDirectInputA, sfcop_dinput_add_ref);
}
static void di_AddRef(void) {
    uint32_t self = A32(0), n = sfcop_dinput_add_ref(HANDLE_OF(self));
    MEM32(self + 4) = n;
    RET(n, 1);
}
static void di_Release(void) {
    uint32_t self = A32(0), n = sfcop_dinput_release(HANDLE_OF(self));
    if (n == SFCOP_DINPUT_UNKNOWN_HANDLE) /* already released: nothing of ours to free */
        RET(0, 1);
    if (n)
        MEM32(self + 4) = n;
    else
        hle_com_free(self);
    RET(n, 1);
}
static void di_CreateDevice(void) { /* (this, rguid, lplpDevice, pUnkOuter) */
    uint32_t self = A32(0), out = A32(2);
    if (!out)
        RET(DIERR_INVALIDPARAM, 4);
    MEM32(out) = 0;
    uint32_t device = 0;
    uint32_t hr =
        sfcop_dinput_create_device(HANDLE_OF(self), guid_at(A32(1)), A32(3) != 0, &device);
    if (hr == DI_OK) {
        uint32_t object = hle_com_new(g_vt_device, OBJECT_SIZE);
        if (!object) {
            sfcop_dinput_device_release(device);
            RET(DIERR_OUTOFMEMORY, 4);
        }
        HANDLE_OF(object) = device;
        MEM32(out) = object;
    }
    RET(hr, 4);
}

/* A DIDEVICEINSTANCEA (or the DirectX 3 form) at guest va, filled as far as its dwSize goes. */
static void write_device_instance(uint32_t va, uint32_t size,
                                  const sfcop_dinput_device_instance *d) {
    uint8_t full[DEVICE_INSTANCE_SIZE];
    memset(full, 0, sizeof full);
    memcpy(full, &size, 4);
    memcpy(full + 4, d->instance, 16);
    memcpy(full + 20, d->product, 16);
    memcpy(full + 36, &d->dev_type, 4);
    memcpy(full + 40, d->instance_name, 260);
    memcpy(full + 300, d->product_name, 260);
    memcpy(hle_ptr(va), full, size < sizeof full ? size : sizeof full);
}

static int enum_device(const sfcop_dinput_device_instance *device, void *context) {
    const uint32_t *guest = (const uint32_t *)context; /* callback, pvRef */
    uint32_t mark = hle_scratch_mark(), instance = hle_scratch(DEVICE_INSTANCE_SIZE);
    write_device_instance(instance, DEVICE_INSTANCE_SIZE, device);
    uint32_t args[2] = {instance, guest[1]};
    uint32_t more = hle_call_guest(guest[0], 2, args);
    hle_scratch_release(mark);
    return more != 0;
}
static void di_EnumDevices(void) { /* (this, dwDevType, lpCallback, pvRef, dwFlags) */
    uint32_t guest[2] = {A32(2), A32(3)};
    RET(sfcop_dinput_enum_devices(HANDLE_OF(A32(0)), A32(1), guest[0] ? enum_device : NULL, guest,
                                  A32(4)),
        5);
}
static void di_GetDeviceStatus(void) {
    RET(sfcop_dinput_get_device_status(HANDLE_OF(A32(0)), guid_at(A32(1))), 2);
}
static void di_RunControlPanel(void) {
    RET(sfcop_dinput_run_control_panel(HANDLE_OF(A32(0)), A32(2)), 3);
}
static void di_Initialize(void) {
    RET(sfcop_dinput_initialize(HANDLE_OF(A32(0)), A32(2)), 3);
}

/* ---- IDirectInputDeviceA ---- */

static void dv_QueryInterface(void) {
    query_interface(kIID_IDirectInputDeviceA, sfcop_dinput_device_add_ref);
}
static void dv_AddRef(void) {
    uint32_t self = A32(0), n = sfcop_dinput_device_add_ref(HANDLE_OF(self));
    MEM32(self + 4) = n;
    RET(n, 1);
}
static void dv_Release(void) {
    uint32_t self = A32(0), n = sfcop_dinput_device_release(HANDLE_OF(self));
    if (n == SFCOP_DINPUT_UNKNOWN_HANDLE) /* already released: nothing of ours to free */
        RET(0, 1);
    if (n)
        MEM32(self + 4) = n;
    else
        hle_com_free(self);
    RET(n, 1);
}
static void dv_GetCapabilities(void) { /* (this, lpDIDevCaps) */
    uint32_t caps = A32(1);
    if (!caps)
        RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(caps);
    sfcop_dinput_caps c;
    uint32_t hr = sfcop_dinput_device_get_capabilities(HANDLE_OF(A32(0)), size, &c);
    if (hr == DI_OK) {
        memset((uint8_t *)hle_ptr(caps) + 4, 0, size - 4);
        MEM32(caps + 4) = c.flags, MEM32(caps + 8) = c.dev_type, MEM32(caps + 12) = c.axes;
        MEM32(caps + 16) = c.buttons, MEM32(caps + 20) = c.povs;
    }
    RET(hr, 2);
}

/* A DIDEVICEOBJECTINSTANCEA (or the DirectX 3 form) at guest va, as far as its dwSize goes. */
static void write_object_instance(uint32_t va, uint32_t size,
                                  const sfcop_dinput_object_instance *o) {
    uint8_t full[OBJECT_INSTANCE_SIZE];
    memset(full, 0, sizeof full);
    memcpy(full, &size, 4);
    memcpy(full + 4, o->guid_type, 16);
    memcpy(full + 20, &o->ofs, 4);
    memcpy(full + 24, &o->type, 4);
    memcpy(full + 28, &o->flags, 4);
    memcpy(full + 32, o->name, 260);
    memcpy(hle_ptr(va), full, size < sizeof full ? size : sizeof full);
}

static int enum_object(const sfcop_dinput_object_instance *object, void *context) {
    const uint32_t *guest = (const uint32_t *)context; /* callback, pvRef */
    uint32_t mark = hle_scratch_mark(), instance = hle_scratch(OBJECT_INSTANCE_SIZE);
    write_object_instance(instance, OBJECT_INSTANCE_SIZE, object);
    uint32_t args[2] = {instance, guest[1]};
    uint32_t more = hle_call_guest(guest[0], 2, args);
    hle_scratch_release(mark);
    return more != 0;
}
static void dv_EnumObjects(void) { /* (this, lpCallback, pvRef, dwFlags) */
    uint32_t guest[2] = {A32(1), A32(2)};
    RET(sfcop_dinput_device_enum_objects(HANDLE_OF(A32(0)), guest[0] ? enum_object : NULL, guest,
                                         A32(3)),
        4);
}

/* A DIPROPHEADER at guest va; 0 if va is null. */
static int read_header(uint32_t va, sfcop_dinput_prop_header *h) {
    if (!va)
        return 0;
    h->size = MEM32(va), h->header_size = MEM32(va + 4), h->obj = MEM32(va + 8),
    h->how = MEM32(va + 12);
    return 1;
}
static void dv_GetProperty(void) { /* (this, rguidProp, pdiph) */
    uint32_t prop = A32(1), va = A32(2);
    sfcop_dinput_prop_header h;
    if (prop >= 0x10000u)
        RET(DIERR_UNSUPPORTED, 3); /* a real GUID: no keyboard property */
    if (!read_header(va, &h))
        RET(DIERR_INVALIDPARAM, 3);
    uint32_t value = 0;
    uint32_t hr = sfcop_dinput_device_get_property(HANDLE_OF(A32(0)), prop, &h, &value);
    if (hr == DI_OK)
        MEM32(va + 16) = value; /* DIPROPDWORD.dwData; the size was checked */
    RET(hr, 3);
}
static void dv_SetProperty(void) { /* (this, rguidProp, pdiph) */
    uint32_t prop = A32(1), va = A32(2);
    sfcop_dinput_prop_header h;
    if (prop >= 0x10000u)
        RET(DIERR_UNSUPPORTED, 3);
    if (!read_header(va, &h))
        RET(DIERR_INVALIDPARAM, 3);
    uint32_t value = h.size >= 20 ? MEM32(va + 16) : 0;
    RET(sfcop_dinput_device_set_property(HANDLE_OF(A32(0)), prop, &h, value), 3);
}
static void dv_Acquire(void) {
    RET(sfcop_dinput_device_acquire(HANDLE_OF(A32(0))), 1);
}
static void dv_Unacquire(void) {
    RET(sfcop_dinput_device_unacquire(HANDLE_OF(A32(0))), 1);
}
static void dv_GetDeviceState(void) { /* (this, cbData, lpvData): bytes, written in place */
    RET(sfcop_dinput_device_get_state(HANDLE_OF(A32(0)), A32(1), (uint8_t *)hle_ptr(A32(2))), 3);
}
static void dv_GetDeviceData(void) { /* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
    uint32_t io = A32(3);
    uint32_t count = io ? MEM32(io) : 0;
    uint32_t hr = sfcop_dinput_device_get_data(HANDLE_OF(A32(0)), A32(1), hle_ptr(A32(2)),
                                               io ? &count : NULL, A32(4));
    if (io)
        MEM32(io) = count;
    RET(hr, 5);
}
static void dv_SetDataFormat(void) { /* (this, lpdf) */
    uint32_t df = A32(1);
    if (!df)
        RET(sfcop_dinput_device_set_data_format(HANDLE_OF(A32(0)), NULL), 2);
    sfcop_dinput_format f;
    f.size = MEM32(df), f.obj_size = MEM32(df + 4), f.flags = MEM32(df + 8);
    f.data_size = MEM32(df + 12), f.num_objs = MEM32(df + 16);
    uint32_t rgodf = MEM32(df + 20);
    if (f.num_objs > MAX_FORMAT_OBJECTS || (f.num_objs && !rgodf) || f.obj_size != 16)
        RET(DIERR_INVALIDPARAM, 2);
    sfcop_dinput_object_format *objects = f.num_objs ? calloc(f.num_objs, sizeof *objects) : NULL;
    if (f.num_objs && !objects)
        RET(DIERR_OUTOFMEMORY, 2);
    for (uint32_t i = 0; i < f.num_objs; i++) {
        uint32_t o = rgodf + 16 * i, pguid = MEM32(o);
        objects[i].has_guid = pguid != 0;
        if (pguid)
            memcpy(objects[i].guid, hle_ptr(pguid), 16);
        objects[i].ofs = MEM32(o + 4), objects[i].type = MEM32(o + 8),
        objects[i].flags = MEM32(o + 12);
    }
    f.objects = objects;
    uint32_t hr = sfcop_dinput_device_set_data_format(HANDLE_OF(A32(0)), &f);
    free(objects);
    RET(hr, 2);
}
static void dv_SetEventNotification(void) {
    RET(sfcop_dinput_device_set_event_notification(HANDLE_OF(A32(0)), A32(1)), 2);
}
static void dv_SetCooperativeLevel(void) { /* (this, hwnd, dwFlags) */
    RET(sfcop_dinput_device_set_cooperative_level(HANDLE_OF(A32(0)), A32(1) != 0, A32(2)), 3);
}
static void dv_GetObjectInfo(void) { /* (this, pdidoi, dwObj, dwHow) */
    uint32_t info = A32(1);
    if (!info)
        RET(DIERR_INVALIDPARAM, 4);
    uint32_t size = MEM32(info);
    sfcop_dinput_object_instance o;
    uint32_t hr = sfcop_dinput_device_get_object_info(HANDLE_OF(A32(0)), size, &o, A32(2), A32(3));
    if (hr == DI_OK)
        write_object_instance(info, size, &o);
    RET(hr, 4);
}
static void dv_GetDeviceInfo(void) { /* (this, pdidi) */
    uint32_t info = A32(1);
    if (!info)
        RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(info);
    sfcop_dinput_device_instance d;
    uint32_t hr = sfcop_dinput_device_get_device_info(HANDLE_OF(A32(0)), size, &d);
    if (hr == DI_OK)
        write_device_instance(info, size, &d);
    RET(hr, 2);
}
static void dv_RunControlPanel(void) {
    RET(sfcop_dinput_device_run_control_panel(HANDLE_OF(A32(0)), A32(2)), 3);
}
static void dv_Initialize(void) { /* (this, hinst, dwVersion, rguid) */
    RET(sfcop_dinput_device_initialize(HANDLE_OF(A32(0)), A32(2), guid_at(A32(3))), 4);
}

static void build_vtables(void) {
    if (g_vt_input)
        return;
    /* Slot order of src/hle/com/IDirectInputA.h and IDirectInputDeviceA.h. */
    static const win32hle_shim input[] = {{"IDirectInputA::QueryInterface", di_QueryInterface},
                                          {"IDirectInputA::AddRef", di_AddRef},
                                          {"IDirectInputA::Release", di_Release},
                                          {"IDirectInputA::CreateDevice", di_CreateDevice},
                                          {"IDirectInputA::EnumDevices", di_EnumDevices},
                                          {"IDirectInputA::GetDeviceStatus", di_GetDeviceStatus},
                                          {"IDirectInputA::RunControlPanel", di_RunControlPanel},
                                          {"IDirectInputA::Initialize", di_Initialize},
                                          {0, 0}};
    static const win32hle_shim device[] = {
        {"IDirectInputDeviceA::QueryInterface", dv_QueryInterface},
        {"IDirectInputDeviceA::AddRef", dv_AddRef},
        {"IDirectInputDeviceA::Release", dv_Release},
        {"IDirectInputDeviceA::GetCapabilities", dv_GetCapabilities},
        {"IDirectInputDeviceA::EnumObjects", dv_EnumObjects},
        {"IDirectInputDeviceA::GetProperty", dv_GetProperty},
        {"IDirectInputDeviceA::SetProperty", dv_SetProperty},
        {"IDirectInputDeviceA::Acquire", dv_Acquire},
        {"IDirectInputDeviceA::Unacquire", dv_Unacquire},
        {"IDirectInputDeviceA::GetDeviceState", dv_GetDeviceState},
        {"IDirectInputDeviceA::GetDeviceData", dv_GetDeviceData},
        {"IDirectInputDeviceA::SetDataFormat", dv_SetDataFormat},
        {"IDirectInputDeviceA::SetEventNotification", dv_SetEventNotification},
        {"IDirectInputDeviceA::SetCooperativeLevel", dv_SetCooperativeLevel},
        {"IDirectInputDeviceA::GetObjectInfo", dv_GetObjectInfo},
        {"IDirectInputDeviceA::GetDeviceInfo", dv_GetDeviceInfo},
        {"IDirectInputDeviceA::RunControlPanel", dv_RunControlPanel},
        {"IDirectInputDeviceA::Initialize", dv_Initialize},
        {0, 0}};
    g_vt_input = hle_com_vtable(input);
    g_vt_device = hle_com_vtable(device);
}

/* ---- DINPUT.DLL ---- */

static void
s_DirectInputCreateA(void) { /* (hinst, dwVersion, lplpDirectInput, punkOuter); hinst may be 0 */
    uint32_t out = A32(2);
    if (!out)
        RET(DIERR_INVALIDPARAM, 4);
    MEM32(out) = 0;
    build_vtables();
    uint32_t handle = 0;
    uint32_t hr = sfcop_dinput_create(A32(1), A32(3) != 0, &handle);
    if (hr == DI_OK) {
        uint32_t object = hle_com_new(g_vt_input, OBJECT_SIZE);
        if (!object) {
            sfcop_dinput_release(handle);
            RET(DIERR_OUTOFMEMORY, 4);
        }
        HANDLE_OF(object) = handle;
        MEM32(out) = object;
    }
    RET(hr, 4);
}

const win32hle_shim sfcop_hle_dinput[] = {
    {"DirectInputCreateA", s_DirectInputCreateA},
    {0, 0},
};
