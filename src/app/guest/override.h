// The override table: decompiled functions that run in place of their lifted bodies in the
// native build (DESIGN §6.3, §7.2), and the guest runtime that game/port/guest.h declares.
//
// Two builds fill the table:
//
// - The object lift (the native build's route, cmake/lifter/objlift.cmake): each claimed unit is
//   compiled by VC6 as the harness compiles it, and tools/objlift/objlift.py lifts its object
//   into objlift_table.c and objlift_code.c in the build directory. The lifted bodies carry the
//   exe lift's names (sub_XXXXXXXX), the exe lift's own bodies for those addresses are weak, and
//   the table here lists the claimed ones. The data image (sections the exe does not hold, in the
//   reserved guest range) is loaded by sfcop_guest_load_objects(). The build then leaves
//   override.cpp out.
// - The guest-mode fallback (spike/guest-mode/REPORT.md): tools/manifest.py writes override.cpp
//   from the FUNCTION annotations in game/. With SFCOP_GUEST_MODE the build compiles each
//   claimed unit in guest mode and links override.cpp's entry points under the lifted names.
//
// Either way the lift's direct calls, its dispatch table, and so every guest vtable slot holding
// the address, reach the decompiled code. With neither the table is empty.
//
// The C part of this header is the table, readable from C and C++, and the object lift's runtime.
// The C++ part, under the macro SFCOP_GUEST_MODE_SOURCES (which the guest-mode build defines), is
// what override.cpp is built from: the guest calling conventions (an entry point reads ecx and the
// guest stack, calls the decompiled function and pops what the original popped) and the runtime
// game/ calls back into.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sfcop_guest_entry)(void);

typedef struct sfcop_guest_override {
    uint32_t va;              // the original function's address
    sfcop_guest_entry entry;  // its guest-ABI entry point, compiled from the source body
    const char *symbol;       // the source's name for it
    const char *source;       // the unit, from the game/ root
} sfcop_guest_override;

// Sorted by address.
extern const sfcop_guest_override sfcop_guest_overrides[];
extern const unsigned sfcop_guest_override_count;

// The entry point for an original address, or NULL. Fits pcrecomp's manual lookup hook
// (hle_manual_hook), which answers indirect calls only; the build replaces direct calls too.
sfcop_guest_entry sfcop_guest_override_lookup(uint32_t va);

// How often an override has run so far (0 for an address that is not one).
uint32_t sfcop_guest_override_calls(uint32_t va);

// ---- The object lift's runtime (tools/objlift/README.md) ----------------------------------------

// One section of the data image: placed at va in the reserved guest range; bytes is NULL for
// uninitialised data, which the reservation leaves zero.
typedef struct sfcop_guest_object_section {
    uint32_t va;
    uint32_t size;
    const uint8_t *bytes;
} sfcop_guest_object_section;

extern const sfcop_guest_object_section sfcop_guest_object_sections[];
extern const unsigned sfcop_guest_object_section_count;
extern const uint32_t sfcop_guest_object_base;  // the reserved guest range
extern const uint32_t sfcop_guest_object_size;  // what of it the image uses, in 64 KB units
extern int sfcop_guest_objects_loaded;

// Reserve the image's guest range and copy the data image in. A host calls it once, after it has
// mapped the exe and before guest code runs; it is idempotent, and every lifted object body whose
// unit has a data image calls it on entry too (SFCOP_GUEST_OBJECTS_LOAD), so a host that does not
// call it still gets the image before the first use. Fatal if the range is taken.
void sfcop_guest_load_objects(void);
#define SFCOP_GUEST_OBJECTS_LOAD() \
    do { \
        if (!sfcop_guest_objects_loaded) sfcop_guest_load_objects(); \
    } while (0)

// Called by each lifted claimed body before it runs (index into sfcop_guest_overrides): counts,
// and logs the first call with its caller.
void sfcop_guest_entered(unsigned index);

#ifdef __cplusplus
}
#endif

// ---- The object lift's runtime, compiled once: objlift_table.c defines
// SFCOP_GUEST_OBJECTS_IMPLEMENTATION after including recomp_types.h and win32hle.h ----
#if defined(SFCOP_GUEST_OBJECTS_IMPLEMENTATION) && !defined(__cplusplus)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t sfcop_guest_calls[];  // one counter per override, beside the table
int sfcop_guest_objects_loaded;

void sfcop_guest_load_objects(void) {
    if (sfcop_guest_objects_loaded) {
        return;
    }
    sfcop_guest_objects_loaded = 1;
    if (sfcop_guest_object_section_count == 0) {
        return;
    }
    if (hle_vm_reserve(sfcop_guest_object_base, sfcop_guest_object_size) != sfcop_guest_object_base) {
        hle_fatal("[objlift] cannot reserve the data image at 0x%08X (0x%X bytes): the range is in use",
                  sfcop_guest_object_base, sfcop_guest_object_size);
    }
    for (unsigned i = 0; i < sfcop_guest_object_section_count; i++) {
        const sfcop_guest_object_section *s = &sfcop_guest_object_sections[i];
        if (s->bytes) {
            memcpy(hle_ptr(s->va), s->bytes, s->size);
        }
    }
    fprintf(stderr, "[objlift] data image: %u sections at 0x%08X\n", sfcop_guest_object_section_count,
            sfcop_guest_object_base);
}

static void (*sfcop_guest_prev_on_exit)(int code);

static void sfcop_guest_report_calls(int code) {
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        fprintf(stderr, "[override] 0x%08X %s: %u calls\n", sfcop_guest_overrides[i].va,
                sfcop_guest_overrides[i].symbol, sfcop_guest_calls[i]);
    }
    if (sfcop_guest_prev_on_exit) {
        sfcop_guest_prev_on_exit(code);
    }
}

// Silent unless SFCOP_GUEST_VERBOSE=1 is set in the environment.
__attribute__((constructor)) static void sfcop_guest_announce(void) {
    const char *verbose = getenv("SFCOP_GUEST_VERBOSE");
    if (!verbose || strcmp(verbose, "1") != 0) {
        return;
    }
    fprintf(stderr,
            "[override] %u functions from game/ (their VC6 objects, lifted) run in place of their "
            "lifted bodies\n",
            sfcop_guest_override_count);
}

// Runs before the body's RECOMP_ENTER, so g_cur_func is still the caller.
void sfcop_guest_entered(unsigned index) {
    const sfcop_guest_override *o = &sfcop_guest_overrides[index];
    if (sfcop_guest_calls[index]++ == 0) {
        const uint32_t last = (g_icall_trace_idx - 1u) & (ICALL_TRACE_SIZE - 1);
        const int indirect = g_icall_trace_idx != 0 && g_icall_trace[last] == o->va &&
                             g_icall_from[last] == g_cur_func;
        fprintf(stderr, "[override] 0x%08X %s (game/%s) first run from sub_%08X, %s\n", o->va, o->symbol,
                o->source, g_cur_func, indirect ? "an indirect call (vtable or pointer)" : "a direct call");
        if (hle_on_exit != sfcop_guest_report_calls) {
            sfcop_guest_prev_on_exit = hle_on_exit;
            hle_on_exit = sfcop_guest_report_calls;
        }
    }
}

sfcop_guest_entry sfcop_guest_override_lookup(uint32_t va) {
    unsigned lo = 0, hi = sfcop_guest_override_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (sfcop_guest_overrides[mid].va < va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < sfcop_guest_override_count && sfcop_guest_overrides[lo].va == va ? sfcop_guest_overrides[lo].entry
                                                                                 : NULL;
}

uint32_t sfcop_guest_override_calls(uint32_t va) {
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        if (sfcop_guest_overrides[i].va == va) {
            return sfcop_guest_calls[i];
        }
    }
    return 0;
}

#endif  // SFCOP_GUEST_OBJECTS_IMPLEMENTATION

#if defined(__cplusplus) && defined(SFCOP_GUEST_MODE_SOURCES)

#include <cstddef>
#include <type_traits>
#include <utility>

// The lifted machine's register file and call trace (pcrecomp's recomp_types.h), and the call into
// guest code (win32hle.h). Declared here, not included: those are C headers for generated code.
extern "C" {
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_cur_func;
extern uint32_t g_icall_trace[128];
extern uint32_t g_icall_from[128];
extern uint32_t g_icall_trace_idx;
uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args);
void mach_enter(void);
void mach_leave(void);
}

namespace sfcop::guest {

// 32-bit guest values to and from the C++ types of a decompiled signature.
template <class T, class = void>
struct Value;

template <class T>
struct Value<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> {
    static T in(uint32_t v) {
        if constexpr (std::is_same_v<T, bool>) {
            return (v & 0xFFu) != 0;  // VC6 passes and returns bool in the low byte only
        } else {
            return static_cast<T>(v);
        }
    }
    static uint32_t out(T v) { return static_cast<uint32_t>(v); }
};

template <class T>
struct Value<T, std::enable_if_t<std::is_pointer_v<T>>> {
    static T in(uint32_t v) { return static_cast<T>(host(v)); }
    static uint32_t out(T p) { return addr(p); }
};

template <class T>
struct Value<T, std::enable_if_t<std::is_reference_v<T>>> {
    using Object = std::remove_reference_t<T>;
    static T in(uint32_t v) { return *static_cast<Object *>(host(v)); }
    static uint32_t out(T r) { return addr(&r); }
};

// Argument i of the call being entered: the guest stack holds [return address][arg 0][arg 1]...
inline uint32_t stackArgument(uint32_t esp, std::size_t i) {
    return *static_cast<const uint32_t *>(host(esp + 4u + 4u * static_cast<uint32_t>(i)));
}

template <class T>
constexpr void checkSlot() {
    static_assert(std::is_reference_v<T> || std::is_pointer_v<T> || sizeof(T) <= 4,
                  "a guest argument wider than 32 bits is not handled yet");
}

// Entry points: Entry<F>::run() is what a lifted caller's `call` reaches.
template <auto F, class Signature = decltype(F)>
struct Entry;

// __thiscall: `this` in ecx, the callee pops its arguments.
template <auto F, class C, class R, class... A>
struct MemberEntry {
    template <std::size_t... I>
    static void call(C *self, uint32_t esp, std::index_sequence<I...>) {
        (checkSlot<A>(), ...);
        if constexpr (std::is_void_v<R>) {
            (self->*F)(Value<A>::in(stackArgument(esp, I))...);
        } else {
            g_eax = Value<R>::out((self->*F)(Value<A>::in(stackArgument(esp, I))...));
        }
    }
    static void run() {
        const uint32_t esp = g_esp;
        call(static_cast<C *>(host(g_ecx)), esp, std::index_sequence_for<A...>{});
        g_esp = esp + 4u * (1u + sizeof...(A));
    }
};

template <auto F, class C, class R, class... A>
struct Entry<F, R (C::*)(A...)> : MemberEntry<F, C, R, A...> {};
template <auto F, class C, class R, class... A>
struct Entry<F, R (C::*)(A...) const> : MemberEntry<F, const C, R, A...> {};

// __cdecl (free functions, static members): the caller pops.
template <auto F, class R, class... A>
struct Entry<F, R (*)(A...)> {
    template <std::size_t... I>
    static void call(uint32_t esp, std::index_sequence<I...>) {
        (checkSlot<A>(), ...);
        if constexpr (std::is_void_v<R>) {
            F(Value<A>::in(stackArgument(esp, I))...);
        } else {
            g_eax = Value<R>::out(F(Value<A>::in(stackArgument(esp, I))...));
        }
    }
    static void run() {
        const uint32_t esp = g_esp;
        call(esp, std::index_sequence_for<A...>{});
        g_esp = esp + 4u;
    }
};

// A member function pointer of a given type, for a name that may be overloaded.
template <class Signature, class C>
constexpr Signature C::*member(Signature C::*p) {
    return p;
}

// A call from decompiled code to a virtual member with no guest-mode body: through the guest
// object's vtable, as the original's `call [reg+slot*4]` does.
template <class R, class... A>
R virtualCall(const void *self, uint32_t slot, A... args) {
    (checkSlot<A>(), ...);
    const uint32_t object = addr(self);
    const uint32_t vtable = *static_cast<const uint32_t *>(host(object));
    const uint32_t target = *static_cast<const uint32_t *>(host(vtable + 4u * slot));
    const uint32_t values[sizeof...(A) + 1] = {Value<A>::out(args)..., 0u};
    const uint32_t eax = sfcop_guest_call(target, object, static_cast<int>(sizeof...(A)), values);
    if constexpr (!std::is_void_v<R>) {
        return Value<R>::in(eax);
    }
}

// Called by every generated entry point before it runs: counts, and logs the first call with the
// caller (the lifted function that made it) and how it got here.
void entered(std::size_t index);

}  // namespace sfcop::guest

// ---- The runtime, compiled once: override.cpp defines SFCOP_GUEST_OVERRIDE_IMPLEMENTATION ----
#if defined(SFCOP_GUEST_OVERRIDE_IMPLEMENTATION)

#include <stdio.h>

extern "C" void hle_exit(int code);
extern "C" void (*hle_on_exit)(int code);

namespace sfcop::guest {

// The original's CRT functions guest mode calls (StarFleetOP.exe 2.5.5.2; the lifted
// tAllocatedClass::operator== calls both).
constexpr uint32_t kRTtypeid = 0x00737A93u;        // ___RTtypeid(object), __cdecl
constexpr uint32_t kTypeInfoEquals = 0x00735CD5u;  // type_info::operator==(rhs), __thiscall
constexpr uint32_t kImageBase = 0x00400000u;

constexpr unsigned kMaxOverrides = 32768;  // the exe has about 27,600 functions
uint32_t g_calls[kMaxOverrides];

void entered(std::size_t index) {
    const sfcop_guest_override &o = sfcop_guest_overrides[index];
    if (g_calls[index]++ == 0) {
        const uint32_t caller = g_cur_func;
        const uint32_t last = (g_icall_trace_idx - 1u) & 127u;
        const bool indirect =
            g_icall_trace_idx != 0 && g_icall_trace[last] == o.va && g_icall_from[last] == caller;
        fprintf(stderr, "[override] 0x%08X %s (game/%s) first run from sub_%08X, %s\n", o.va, o.symbol,
                o.source, caller, indirect ? "an indirect call (vtable or pointer)" : "a direct call");
    }
    g_cur_func = o.va;
}

static void reportCalls(int code) {
    (void)code;
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        fprintf(stderr, "[override] 0x%08X %s: %u calls\n", sfcop_guest_overrides[i].va,
                sfcop_guest_overrides[i].symbol, g_calls[i]);
    }
}

static int announce() {
    if (sfcop_guest_override_count > kMaxOverrides) {
        fprintf(stderr, "[override] fatal: %u overrides, room for %u\n", sfcop_guest_override_count,
                kMaxOverrides);
        hle_exit(1);
    }
    fprintf(stderr, "[override] %u functions compiled from game/ run in place of their lifted bodies\n",
            sfcop_guest_override_count);
    hle_on_exit = reportCalls;
    return 0;
}
static const int g_announced = announce();

}  // namespace sfcop::guest

extern "C" {

sfcop_guest_entry sfcop_guest_override_lookup(uint32_t va) {
    unsigned lo = 0, hi = sfcop_guest_override_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (sfcop_guest_overrides[mid].va < va) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < sfcop_guest_override_count && sfcop_guest_overrides[lo].va == va
               ? sfcop_guest_overrides[lo].entry
               : nullptr;
}

uint32_t sfcop_guest_override_calls(uint32_t va) {
    for (unsigned i = 0; i < sfcop_guest_override_count; i++) {
        if (sfcop_guest_overrides[i].va == va) {
            return sfcop::guest::g_calls[i];
        }
    }
    return 0;
}

// Under the machine first: entered from the host (depth 0), taking the machine loads this thread's
// registers, which would overwrite ecx; from guest code it only nests.
uint32_t sfcop_guest_call(uint32_t va, uint32_t thisVa, int nargs, const uint32_t *args) {
    mach_enter();
    const uint32_t ecx = g_ecx;
    g_ecx = thisVa;
    const uint32_t eax = hle_call_guest(va, nargs, args);
    g_ecx = ecx;
    mach_leave();
    return eax;
}

uint32_t sfcop_guest_typeid(uint32_t objectVa) {
    return sfcop_guest_call(sfcop::guest::kRTtypeid, 0, 1, &objectVa);
}

int32_t sfcop_guest_type_info_eq(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(sfcop_guest_call(sfcop::guest::kTypeInfoEquals, a, 1, &b));
}

// A TypeDescriptor is {vftable, spare, decorated name}: find the name in the mapped image, once.
uint32_t sfcop_guest_type_descriptor(const char *decorated) {
    using sfcop::guest::host;
    const uint32_t base = sfcop::guest::kImageBase;
    const uint32_t pe = base + *static_cast<const uint32_t *>(host(base + 0x3Cu));
    const uint32_t end = base + *static_cast<const uint32_t *>(host(pe + 0x50u));  // SizeOfImage
    const size_t length = strlen(decorated) + 1;
    const char *image = static_cast<const char *>(host(base));
    for (uint32_t at = base + 0x1000u; at + length <= end; at += 4u) {
        if (memcmp(image + (at - base), decorated, length) == 0) {
            const uint32_t descriptor = at - 8u;
            if (*static_cast<const uint32_t *>(host(descriptor + 4u)) == 0u) {
                return descriptor;
            }
        }
    }
    return 0;
}

void sfcop_guest_fatal(const char *what) {
    fprintf(stderr, "[override] fatal: %s (in sub_%08X)\n", what, g_cur_func);
    hle_exit(1);
}

}  // extern "C"

#endif  // SFCOP_GUEST_OVERRIDE_IMPLEMENTATION

#endif
