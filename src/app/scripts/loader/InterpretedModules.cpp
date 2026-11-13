// The provider that runs mission scripts from their .scr files (InterpretedModules.h).
#include "app/scripts/loader/InterpretedModules.h"

#include "app/scripts/interp/CodeSurvey.h"
#include "app/scripts/interp/Cpu.h"

#include "app/scripts/interp/EntryThunks.h"
#include "app/scripts/loader/NativeModuleLoader.h"
#include "hle/kernel/KernelBridge.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace sfcop::scripts {

namespace {

constexpr std::uint32_t kFirstSlot = 0x60000000u;
constexpr std::uint32_t kSlotSize = 0x00100000u;
constexpr std::size_t kSlots = 256;
// A script file is read whole before it is checked; no image may exceed 16 MB, so no file need.
constexpr std::uint32_t kLargestScriptFile = 0x01000000u;
constexpr std::string_view kScriptSuffix = ".scr";

// WIN32_FIND_DATAA: cFileName at 44; CreateFileA's GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING.
constexpr std::size_t kFindFileName = 44;
constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kShareRead = 1;
constexpr std::uint32_t kOpenExisting = 3;
constexpr std::uint32_t kAttributeNormal = 0x80;

std::string_view baseName(std::string_view name) {
    const std::size_t cut = name.find_last_of("\\/");
    return cut == std::string_view::npos ? name : name.substr(cut + 1);
}

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool isScript(std::string_view name) {
    const std::string_view base = baseName(name);
    return base.size() > kScriptSuffix.size() &&
           sameText(base.substr(base.size() - kScriptSuffix.size()), kScriptSuffix);
}

std::string scriptStem(std::string_view name) {
    const std::string_view base = baseName(name);
    return std::string(base.substr(0, base.size() - kScriptSuffix.size()));
}

bool isModuleName(const std::string &name) {
    return !name.empty() && name[0] != '.' && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
    });
}

void say(const char *format, const std::string &a) { std::fprintf(stderr, format, a.c_str()); }

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The provider the dispatch asks for script addresses.
InterpretedModuleProvider *g_active = nullptr;

sfcop_guest_fn activeLookup(std::uint32_t address) {
    return g_active != nullptr ? g_active->Lookup(address) : nullptr;
}

// The lifter's three lookups, in the order an indirect call in lifted code asks them.
using LiftedLookup = interp::HostFunction (*)(std::uint32_t);
LiftedLookup g_lookupManual = nullptr;
LiftedLookup g_lookupLifted = nullptr;
LiftedLookup g_lookupImport = nullptr;
// The name of the client row at a guest address (pcrecomp's hle_name), or null.
using RowName = const char *(*)(std::uint32_t);
RowName g_rowName = nullptr;

// A script's call out: the game's own code (the manual hooks, which also answer the mission
// modules' code, then the lifted dispatch), or a client row the provider allows
// (InterpretedModuleProvider::AllowsRow). Any other row answers nothing: the call is unresolved
// (Interpreter::callOut).
// A script's dynamic-lookup imports (LoadLibraryA, LoadLibraryExA, GetModuleHandleA,
// GetProcAddress) are bound to the provider's shims, at addresses just past the last script slot,
// where nothing is code; each shim passes an allowed lookup on to the client's own row.
constexpr std::uint32_t kShimBase = 0x70000000u;
constexpr const char *kShimImports[] = {"KERNEL32.DLL!LoadLibraryA", "KERNEL32.DLL!LoadLibraryExA",
                                        "KERNEL32.DLL!GetModuleHandleA", "KERNEL32.DLL!GetProcAddress"};
constexpr std::uint32_t kShims = sizeof kShimImports / sizeof kShimImports[0];
std::uint32_t g_clientRows[kShims] = {}; // the client's own row for each, which the shims pass on to

void loadLibraryA() { g_active->LoadForScript(0, 4); }
void loadLibraryExA() { g_active->LoadForScript(1, 12); }
void getModuleHandleA() { g_active->LoadForScript(2, 4); }
void getProcAddress() { g_active->FindForScript(); }
constexpr interp::HostFunction kShimFunctions[kShims] = {loadLibraryA, loadLibraryExA, getModuleHandleA,
                                                         getProcAddress};

interp::HostFunction clientLookup(std::uint32_t address) {
    if (const interp::HostFunction shim = InterpretedModuleProvider::ShimAt(address)) {
        return shim;
    }
    for (const LiftedLookup lookup : {g_lookupManual, g_lookupLifted}) {
        if (const interp::HostFunction found = lookup(address)) {
            return found;
        }
    }
    const interp::HostFunction row = g_lookupImport(address);
    if (row != nullptr && g_active != nullptr && g_active->AllowsRow(address)) {
        return row;
    }
    return nullptr;
}

// The lift's landing pad, laid out as its runtime declares it (recomp_ehframe_t): the client's
// exception transfers find a pad by entry_esp and longjmp to jb with resume set.
struct LiftedPad {
    LiftedPad *prev;
    std::jmp_buf jb;
    std::uint32_t entryEsp;
    std::uint32_t resume;
    std::uint32_t function;
};
void (*g_padPush)(LiftedPad *) = nullptr;
void (*g_padPop)(LiftedPad *) = nullptr;
LiftedPad *(*g_padOwner)(std::uint32_t esp) = nullptr;
void (*g_padResume)(LiftedPad *, std::uint32_t pc) = nullptr;

void runInPad(std::uint32_t entryEsp, std::uint32_t function, void (*body)(void *, std::uint32_t), void *context) {
    LiftedPad pad{};
    pad.entryEsp = entryEsp;
    pad.function = function;
    g_padPush(&pad);
    // Each resume comes back here with the registers set; the body goes on at pad.resume.
    const bool resumed = setjmp(pad.jb) != 0;
    body(context, resumed ? pad.resume : 0);
    g_padPop(&pad);
}

void *padOwning(std::uint32_t esp, std::uint32_t *function) {
    LiftedPad *pad = g_padOwner(esp);
    *function = pad != nullptr ? pad->function : 0;
    return pad;
}

void resumePad(void *pad, std::uint32_t pc) { g_padResume(static_cast<LiftedPad *>(pad), pc); }

} // namespace

std::uint32_t scriptSlotBase(std::size_t index) {
    return index < kSlots ? kFirstSlot + kSlotSize * static_cast<std::uint32_t>(index) : 0;
}

std::optional<std::size_t> scriptSlotIndex(std::string_view fileName, std::vector<std::string> folder) {
    folder.erase(std::remove_if(folder.begin(), folder.end(), [](const std::string &n) { return !isScript(n); }),
                 folder.end());
    std::sort(folder.begin(), folder.end()); // std::string compares as unsigned bytes
    folder.erase(std::unique(folder.begin(), folder.end()), folder.end());
    for (std::size_t i = 0; i < folder.size(); ++i) {
        if (folder[i] == fileName) {
            return i;
        }
    }
    for (std::size_t i = 0; i < folder.size(); ++i) {
        if (sameText(folder[i], fileName)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<ScriptFile> readGuestScript(std::string_view name, std::string &why) {
    const std::string path(name);
    const std::string_view file = baseName(name);
    const std::string pattern = path.substr(0, path.size() - file.size()) + "*" + std::string(kScriptSuffix);
    std::vector<std::string> folder;
    std::uint8_t data[SFCOP_FIND_DATA_A_SIZE] = {};
    if (const std::uint32_t find = sfcop_kernel_find_first(pattern.c_str(), 0, data); find != 0) {
        do {
            folder.emplace_back(reinterpret_cast<const char *>(data + kFindFileName),
                                strnlen(reinterpret_cast<const char *>(data + kFindFileName), 260));
        } while (sfcop_kernel_find_next(find, data, 0) != 0);
        sfcop_kernel_find_close(find);
    }
    const std::optional<std::size_t> index = scriptSlotIndex(file, folder);
    if (!index) {
        why = "it is not among the .scr files of its folder";
        return std::nullopt;
    }
    if (*index >= kSlots) {
        why = "it is .scr number " + std::to_string(*index + 1) + " of its folder, past the 256 slots";
        return std::nullopt;
    }
    const std::uint32_t handle =
        sfcop_kernel_create_file(path.c_str(), 0, kGenericRead, kShareRead, kOpenExisting, kAttributeNormal);
    if (handle == 0) {
        why = "the file cannot be opened";
        return std::nullopt;
    }
    std::uint32_t high = 0;
    const std::uint32_t size = sfcop_kernel_get_file_size(handle, &high);
    ScriptFile script;
    script.base = scriptSlotBase(*index);
    std::uint32_t read = 0;
    if (high != 0 || size > kLargestScriptFile) {
        why = "the file is too large for a script";
    } else {
        script.bytes.resize(size);
        if (size != 0 && (sfcop_kernel_read_file(handle, script.bytes.data(), size, &read) == 0 || read != size)) {
            why = "the file cannot be read";
        }
    }
    sfcop_kernel_close_file(handle);
    if (!why.empty()) {
        return std::nullopt;
    }
    return script;
}

std::optional<interp::Machine> clientMachine() {
    const auto symbol = [](const char *name) { return native::programSymbol(name); };
    interp::Machine machine;
    const char *const registers[8] = {"g_eax", "g_ecx", "g_edx", "g_ebx", "g_esp", "g_ebp", "g_esi", "g_edi"};
    for (unsigned n = 0; n < 8; ++n) {
        machine.regs[n] = static_cast<std::uint32_t *>(symbol(registers[n]));
        if (machine.regs[n] == nullptr) {
            return std::nullopt;
        }
    }
    machine.flagKind = static_cast<std::uint32_t *>(symbol("g_flag_k"));
    machine.flagA = static_cast<std::uint32_t *>(symbol("g_flag_a"));
    machine.flagB = static_cast<std::uint32_t *>(symbol("g_flag_b"));
    machine.flagCarry = static_cast<std::uint32_t *>(symbol("g_flag_cf"));
    machine.st = static_cast<double *>(symbol("g_st"));
    machine.stExact = static_cast<std::int64_t *>(symbol("g_st_i64"));
    machine.fpTop = static_cast<int *>(symbol("g_fp_top"));
    machine.fpuControl = static_cast<std::uint16_t *>(symbol("g_fpu_cw"));
    machine.fsBase = static_cast<std::uint32_t *>(symbol("g_fs_base"));
    machine.gsBase = static_cast<std::uint32_t *>(symbol("g_gs_base"));
    machine.currentFunction = static_cast<std::uint32_t *>(symbol("g_cur_func"));
    machine.yieldHook = static_cast<void (**)(void)>(symbol("recomp_yield_hook"));
    machine.backEdges = static_cast<std::uint32_t *>(symbol("g_backedges"));
    machine.divideFaultHook = static_cast<void (**)(std::uint32_t)>(symbol("recomp_div_fault_hook"));
    g_lookupManual = reinterpret_cast<LiftedLookup>(symbol("recomp_lookup_manual"));
    g_lookupLifted = reinterpret_cast<LiftedLookup>(symbol("recomp_lookup"));
    g_lookupImport = reinterpret_cast<LiftedLookup>(symbol("recomp_lookup_import"));
    g_rowName = reinterpret_cast<RowName>(symbol("hle_name"));
    const auto *memoryBase = static_cast<const std::ptrdiff_t *>(symbol("g_mem_base"));
    if (machine.flagKind == nullptr || machine.flagA == nullptr || machine.flagB == nullptr ||
        machine.flagCarry == nullptr || machine.st == nullptr || machine.stExact == nullptr ||
        machine.fpTop == nullptr || machine.fpuControl == nullptr ||
        machine.fsBase == nullptr || machine.currentFunction == nullptr || memoryBase == nullptr ||
        g_lookupManual == nullptr || g_lookupLifted == nullptr || g_lookupImport == nullptr) {
        return std::nullopt;
    }
    machine.memory = reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(*memoryBase));
    machine.lookup = clientLookup;
    g_padPush = reinterpret_cast<void (*)(LiftedPad *)>(symbol("recomp_eh_push"));
    g_padPop = reinterpret_cast<void (*)(LiftedPad *)>(symbol("recomp_eh_pop"));
    g_padOwner = reinterpret_cast<LiftedPad *(*)(std::uint32_t)>(symbol("recomp_eh_owner"));
    g_padResume = reinterpret_cast<void (*)(LiftedPad *, std::uint32_t)>(symbol("recomp_eh_resume"));
    if (g_padPush != nullptr && g_padPop != nullptr && g_padOwner != nullptr && g_padResume != nullptr) {
        machine.runInPad = runInPad;
        machine.padOwning = padOwning;
        machine.resumePad = resumePad;
    }
    return machine;
}

InterpretedModuleProvider::InterpretedModuleProvider(std::optional<GuestRuntime> runtime,
                                                     std::optional<interp::Machine> machine, ScriptReader reader,
                                                     const AllowList &allowList)
    : runtime_(std::move(runtime)), machine_(std::move(machine)), reader_(std::move(reader)),
      allowList_(allowList) {
    if (machine_) {
        interpreter_ = std::make_unique<interp::Interpreter>(*machine_);
    }
}

InterpretedModuleProvider::~InterpretedModuleProvider() {
    if (g_active == this) {
        g_active = nullptr;
    }
    if (interpreter_) {
        releaseEntryThunks(*interpreter_, 0, 0xFFFFFFFFu);
    }
}

bool InterpretedModuleProvider::Handles(std::string_view name) const { return isScript(name); }

std::uint32_t InterpretedModuleProvider::Load(std::string_view scriptName) {
    const std::string name = scriptStem(scriptName);
    for (auto &loaded : byBase_) {
        if (sameText(loaded.second.name, name)) {
            ++loaded.second.references;
            return loaded.first;
        }
    }
    if (!isModuleName(name)) {
        say("[scripts] refused: %s: not a module name\n", name);
        return 0;
    }
    if (!runtime_ || !interpreter_) {
        say("[scripts] %s: not loaded: this program has no guest machine\n", name);
        return 0;
    }
    std::string why;
    std::optional<ScriptFile> file = reader_(scriptName, why);
    if (!file) {
        say("[scripts] refused: %s\n", name + ": " + why);
        return 0;
    }
    interp::ScriptImageResult laid = interp::readScriptImage(file->bytes, file->base);
    if (!laid.ok) {
        say("[scripts] refused: %s\n", name + ": " + laid.error);
        return 0;
    }
    if (laid.image.size > kSlotSize) {
        say("[scripts] refused: %s: its image is larger than a slot\n", name);
        return 0;
    }
    std::vector<std::string> imports;
    for (const interp::ScriptImport &import : laid.image.imports) {
        imports.push_back(import.name);
    }
    if (const std::optional<LoadError> foreign = checkImports(name, imports, allowList_)) {
        say("[scripts] %s\n", describe(*foreign));
        return 0;
    }
    for (const auto &loaded : byBase_) {
        const interp::ScriptImage &other = loaded.second.image;
        if (laid.image.base < other.base + other.size && other.base < laid.image.base + laid.image.size) {
            say("[scripts] refused: %s\n", name + ": its slot overlaps " + loaded.second.name + "'s");
            return 0;
        }
    }
    // Its code, surveyed before any of it is written or run: a script with an instruction the
    // interpreter does not know, where control certainly goes, is refused here, by name, not
    // stopped in the middle of a mission. One where the survey is not sure it is in code loads, said
    // in a line; if it ever runs one, the script stops then (Interpreter::stopScript).
    const auto removeCode = [&](const interp::ScriptImage &image) {
        for (const interp::ScriptSection &section : image.sections) {
            if (section.executable) {
                interpreter_->removeCode(image.base + section.rva, image.base + section.rva + section.size);
            }
        }
    };
    for (const interp::ScriptSection &section : laid.image.sections) {
        if (section.executable) {
            interpreter_->addCode(laid.image.base + section.rva, laid.image.base + section.rva + section.size,
                                  name, laid.image.linkedBase + section.rva);
        }
    }
    const auto surveyStart = std::chrono::steady_clock::now();
    const interp::SurveyResult survey = interp::surveyCode(*interpreter_, laid.image);
    const double surveyTime = millisecondsSince(surveyStart);
    if (survey.unknown) {
        removeCode(laid.image);
        const std::uint32_t rva = *survey.unknown - laid.image.base;
        std::string bytes;
        for (unsigned n = 0; n < 8 && rva + n < laid.image.bytes.size(); ++n) {
            char hex[4];
            std::snprintf(hex, sizeof hex, n == 0 ? "%02X" : " %02X", laid.image.bytes[rva + n]);
            bytes += hex;
        }
        char where[64];
        std::snprintf(where, sizeof where, " at 0x%08X (", laid.image.linkedBase + rva);
        const std::string reason = ": it has an instruction the interpreter does not know" + std::string(where) + bytes + ")";
        say("[scripts] refused: %s\n", name + reason);
        return 0;
    }
    if (!runtime_->reserve(laid.image.base, laid.image.size)) {
        removeCode(laid.image);
        say("[scripts] refused: %s: its guest range is already in use\n", name);
        return 0;
    }
    Module module{name, std::move(laid.image), 1};
    interp::ScriptImage &image = module.image;
    std::uint8_t *memory = runtime_->memory(image.base);
    std::memcpy(memory, image.bytes.data(), image.bytes.size());
    for (const interp::ScriptImport &import : image.imports) {
        std::uint32_t address = runtime_->importAddress(import.name);
        const auto shim = std::find(std::begin(kShimImports), std::end(kShimImports), import.name);
        if (shim != std::end(kShimImports)) {
            const auto index = static_cast<std::uint32_t>(shim - std::begin(kShimImports));
            g_clientRows[index] = address;
            address = kShimBase + index;
        } else {
            boundRows_.insert(address);
        }
        std::memcpy(memory + import.iatRva, &address, sizeof address);
    }
    const std::uint32_t base = image.base;
    byBase_[base] = std::move(module);
    g_active = this;
    if (runtime_->lookupHook != nullptr) {
        answerInDispatch(activeLookup, runtime_->lookupHook);
    }
    const Module &loaded = byBase_[base];
    const auto start = std::chrono::steady_clock::now();
    if (loaded.image.entry != 0) {
        const std::uint32_t attach[3] = {base, 1u /* DLL_PROCESS_ATTACH */, 0u};
        if (Call(loaded.image.entry, 3, attach) == 0) {
            say("[scripts] %s: its DLL entry point refused to attach\n", name);
            const Module gone = std::move(byBase_[base]);
            byBase_.erase(base);
            Unmap(gone);
            return 0;
        }
    }
    const double entryTime = millisecondsSince(start);
    std::fprintf(stderr,
                 "[scripts] loaded %s at 0x%08X, interpreted (%zu imports, %llu instructions surveyed in %.3f ms, "
                 "%u instructions of extensions the processor does not report, entry point %.3f ms)\n",
                 name.c_str(), base, loaded.image.imports.size(), static_cast<unsigned long long>(survey.instructions),
                 surveyTime, survey.extensions, entryTime);
    if (survey.firstUncertain) {
        std::fprintf(stderr,
                     "[scripts] %s: %u instructions the interpreter does not know follow calls that may not "
                     "return, the first at 0x%08X; if one runs, the script stops there\n",
                     name.c_str(), survey.uncertain, loaded.image.linkedBase + (*survey.firstUncertain - base));
    }
    return base;
}

bool InterpretedModuleProvider::Owns(std::uint32_t handle) const { return byBase_.count(handle) != 0; }

std::uint32_t InterpretedModuleProvider::Export(std::uint32_t handle, std::string_view function) {
    const auto found = byBase_.find(handle);
    if (found == byBase_.end()) {
        return 0;
    }
    const interp::ScriptImage &image = found->second.image;
    const std::uint32_t address = function == kMissionSpecEntryName ? image.missionSpecEntry
                                  : function == kScriptEntryName    ? image.scriptEntry
                                                                    : 0;
    std::fprintf(stderr, "[scripts] %s!%.*s: 0x%08X\n", found->second.name.c_str(), static_cast<int>(function.size()),
                 function.data(), address);
    return address;
}

void InterpretedModuleProvider::Free(std::uint32_t handle) {
    const auto found = byBase_.find(handle);
    if (found == byBase_.end() || --found->second.references != 0) {
        return;
    }
    if (found->second.image.entry != 0) {
        const std::uint32_t detach[3] = {handle, 0u /* DLL_PROCESS_DETACH */, 0u};
        Call(found->second.image.entry, 3, detach);
    }
    const Module module = std::move(found->second);
    byBase_.erase(found);
    Unmap(module);
    say("[scripts] unloaded %s\n", module.name);
}

interp::HostFunction InterpretedModuleProvider::ShimAt(std::uint32_t address) {
    if (g_active == nullptr || address < kShimBase || address - kShimBase >= kShims) {
        return nullptr;
    }
    return kShimFunctions[address - kShimBase];
}

bool InterpretedModuleProvider::ScriptMayLoad(std::string_view module) const {
    return !checkDynamicLookup("", module, "", allowList_).has_value();
}

bool InterpretedModuleProvider::ScriptMayFind(std::uint32_t handle, std::string_view name) const {
    if (Owns(handle)) {
        return true; // a script's own exports
    }
    const auto names = handleNames_.find(handle);
    if (names == handleNames_.end() || name.empty() || name.front() == '#') {
        return false;
    }
    for (const std::string &module : names->second) {
        if (!checkDynamicLookup("", module, name, allowList_)) {
            return true;
        }
    }
    return false;
}

namespace {
// A script's text as a log line may show it: anything but printable ASCII is '?'.
std::string Printable(std::string text) {
    for (char &c : text) {
        if (c < 0x20 || c > 0x7E) {
            c = '?';
        }
    }
    return text;
}
} // namespace

std::optional<std::string> InterpretedModuleProvider::Text(std::uint32_t address) const {
    const interp::Machine &m = *machine_;
    std::string text;
    for (std::uint32_t at = address;; ++at) {
        if (!interp::Cpu::fits(at, 1)) {
            return std::nullopt;
        }
        const char c = static_cast<char>(m.memory[at]);
        if (c == 0) {
            return text;
        }
        if (text.size() == kLookupNameLimit) {
            return std::nullopt; // longer than any name the shims take: refused whole, never cut
        }
        text += c;
    }
}

std::string InterpretedModuleProvider::Caller() const {
    const interp::Machine &m = *machine_;
    const std::uint32_t esp = *m.regs[interp::kEsp];
    std::uint32_t back = 0;
    if (interp::Cpu::fits(esp, 4)) {
        std::memcpy(&back, m.memory + esp, 4);
    }
    for (const auto &[base, module] : byBase_) {
        if (back >= base && back - base < module.image.size) {
            return module.name;
        }
    }
    return "a script";
}

bool InterpretedModuleProvider::PassOn(std::uint32_t shim, std::uint32_t argumentBytes, std::uint32_t argument,
                                       const std::string *name) {
    const std::uint32_t address = g_clientRows[shim];
    if (address == 0) {
        return false;
    }
    // The client's import lookup; a machine with none (a test's) answers rows itself.
    const interp::HostFunction row = g_lookupImport != nullptr ? g_lookupImport(address) : machine_->lookup(address);
    if (row == nullptr) {
        return false;
    }
    if (name == nullptr) {
        row(); // pops its own arguments
        return true;
    }
    // The client reads the name that was checked, not the script's string, which it could change
    // or which could say more than was checked: a copy of the name and of the shim's frame, below
    // the frame on the guest stack, where the call would put its own.
    const interp::Machine &m = *machine_;
    const std::uint32_t esp = *m.regs[interp::kEsp];
    const std::uint32_t frameBytes = 4 + argumentBytes;
    const std::uint32_t text = (esp - static_cast<std::uint32_t>(name->size() + 1)) & ~3u;
    const std::uint32_t frame = text - frameBytes;
    if (frame >= esp || !interp::Cpu::fits(frame, esp - frame) || !interp::Cpu::fits(esp, frameBytes)) {
        return false;
    }
    std::memcpy(m.memory + frame, m.memory + esp, frameBytes); // ends at `text`, below esp
    std::memcpy(m.memory + text, name->c_str(), name->size() + 1);
    std::memcpy(m.memory + frame + 4 + 4 * argument, &text, 4);
    *m.regs[interp::kEsp] = frame;
    row(); // pops the copy's arguments
    *m.regs[interp::kEsp] = esp + frameBytes;
    return true;
}

void InterpretedModuleProvider::Refuse(const std::string &what, std::uint32_t argumentBytes) {
    const std::string line = describe(LoadError{Refusal::kForeignImport, Caller(), what});
    if (refusedLookups_.insert(line).second) {
        say("[scripts] a lookup answers 0 (%s)\n", line);
    }
    const interp::Machine &m = *machine_;
    *m.regs[interp::kEax] = 0;
    *m.regs[interp::kEsp] += 4 + argumentBytes;
}

void InterpretedModuleProvider::LoadForScript(std::uint32_t shim, std::uint32_t argumentBytes) {
    // LoadLibraryA(name), LoadLibraryExA(name, file, flags), GetModuleHandleA(name): stdcall, the
    // return address on top. A null name is the calling program's own module, as Windows answers.
    const interp::Machine &m = *machine_;
    const std::uint32_t esp = *m.regs[interp::kEsp];
    if (!interp::Cpu::fits(esp, 4 + argumentBytes)) {
        Refuse(kShimImports[shim], argumentBytes); // its arguments are not in the guest
        return;
    }
    std::uint32_t name = 0;
    std::memcpy(&name, m.memory + esp + 4, 4);
    if (name == 0) {
        if (!PassOn(shim)) {
            Refuse(kShimImports[shim], argumentBytes);
        }
        return;
    }
    // The module is named bare: a path, even one that ends in a name on the list, could reach
    // another file, and a name too long to read is refused whole.
    const std::optional<std::string> text = Text(name);
    if (!text) {
        Refuse("a module name longer than " + std::to_string(kLookupNameLimit) + " bytes", argumentBytes);
        return;
    }
    if (text->empty() || text->find_first_of("\\/:") != std::string::npos) {
        Refuse(Printable(*text), argumentBytes);
        return;
    }
    const std::string module = normalizeModuleName(*text);
    if (!ScriptMayLoad(module) || !PassOn(shim, argumentBytes, 0, &module)) {
        Refuse(Printable(module), argumentBytes);
        return;
    }
    if (const std::uint32_t handle = *m.regs[interp::kEax]; handle != 0) {
        handleNames_[handle].insert(module);
    }
}

void InterpretedModuleProvider::FindForScript() {
    // GetProcAddress(module, name), stdcall, as lifted code calls it: the return address on top.
    const interp::Machine &m = *machine_;
    const std::uint32_t esp = *m.regs[interp::kEsp];
    if (!interp::Cpu::fits(esp, 12)) {
        Refuse(kShimImports[3], 8); // its arguments are not in the guest
        return;
    }
    std::uint32_t module = 0, name = 0;
    std::memcpy(&module, m.memory + esp + 4, 4);
    std::memcpy(&name, m.memory + esp + 8, 4);
    const auto names = handleNames_.find(module);
    const std::string dll = names != handleNames_.end() ? *names->second.begin() : "a module it did not load";
    // An ordinal is on no list ("DLL!#n"); a name is checked and handed to the client as read.
    const bool ordinal = name <= 0xFFFFu;
    const std::optional<std::string> text = ordinal ? std::optional<std::string>("#" + std::to_string(name)) : Text(name);
    if (!text) {
        Refuse(dll + "!a name longer than " + std::to_string(kLookupNameLimit) + " bytes", 8);
        return;
    }
    if (!ScriptMayFind(module, *text) || !(ordinal ? PassOn(3) : PassOn(3, 8, 1, &*text))) {
        Refuse(dll + "!" + Printable(*text), 8);
        return;
    }
    if (const std::uint32_t row = *m.regs[interp::kEax]; row != 0) {
        foundRows_.insert(row);
    }
}

bool InterpretedModuleProvider::AllowsRow(std::uint32_t address) {
    if (BindsRow(address) || foundRows_.count(address) != 0) {
        return true;
    }
    if (unresolvedRows_.insert(address).second) {
        const char *name = g_rowName != nullptr ? g_rowName(address) : nullptr;
        if (name != nullptr) {
            say("[scripts] a script called %s, which its imports and lookups did not give it; nothing is called\n", name);
        }
    }
    return false;
}

sfcop_guest_fn InterpretedModuleProvider::Lookup(std::uint32_t address) {
    if (!interpreter_ || !interpreter_->interprets(address)) {
        return nullptr;
    }
    const sfcop_guest_fn found = interp::entryThunk(*interpreter_, address);
    if (found == nullptr) {
        std::fprintf(stderr, "[scripts] the game holds %zu script functions, all there are; 0x%08X has none\n",
                     interp::kEntryThunks, address);
    }
    return found;
}

std::uint32_t InterpretedModuleProvider::Call(std::uint32_t address, std::uint32_t argc, const std::uint32_t *args) {
    const interp::Machine &m = *machine_;
    std::uint32_t saved[8];
    for (unsigned n = 0; n < 8; ++n) {
        saved[n] = *m.regs[n];
    }
    std::uint32_t &esp = *m.regs[interp::kEsp];
    for (std::uint32_t i = argc; i > 0; --i) {
        esp -= 4;
        std::memcpy(m.memory + esp, &args[i - 1], 4);
    }
    esp -= 4;
    std::memcpy(m.memory + esp, &interp::kLiftedReturnAddress, 4);
    interpreter_->call(address);
    const std::uint32_t result = *m.regs[interp::kEax];
    for (unsigned n = 0; n < 8; ++n) {
        *m.regs[n] = saved[n];
    }
    return result;
}

void InterpretedModuleProvider::Unmap(const Module &module) {
    const interp::ScriptImage &image = module.image;
    for (const interp::ScriptSection &section : image.sections) {
        if (section.executable) {
            interpreter_->removeCode(image.base + section.rva, image.base + section.rva + section.size);
        }
    }
    interp::releaseEntryThunks(*interpreter_, image.base, image.base + image.size);
    runtime_->release(image.base, image.size);
}

MissionModuleProvider::MissionModuleProvider(GuestModuleProvider &native, InterpretedModuleProvider &interpreted)
    : native_(native), interpreted_(interpreted) {}

bool MissionModuleProvider::Handles(std::string_view name) const { return isScript(name); }

std::uint32_t MissionModuleProvider::Load(std::string_view name) {
    std::error_code error;
    if (std::filesystem::is_regular_file(native_.LibraryFor(name), error)) {
        if (const std::uint32_t handle = native_.Load(name)) {
            return handle;
        }
        say("[scripts] %s: its native module was refused; the script is interpreted\n", scriptStem(name));
    }
    return interpreted_.Load(name);
}

bool MissionModuleProvider::Owns(std::uint32_t handle) const {
    return native_.Owns(handle) || interpreted_.Owns(handle);
}

hle::kernel32rt::ModuleProvider &MissionModuleProvider::Owner(std::uint32_t handle) {
    if (native_.Owns(handle)) {
        return native_;
    }
    return interpreted_;
}

std::uint32_t MissionModuleProvider::Export(std::uint32_t handle, std::string_view function) {
    return Owner(handle).Export(handle, function);
}

void MissionModuleProvider::Free(std::uint32_t handle) { Owner(handle).Free(handle); }

} // namespace sfcop::scripts
