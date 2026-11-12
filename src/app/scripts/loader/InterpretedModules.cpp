// The provider that runs mission scripts from their .scr files (InterpretedModules.h).
#include "app/scripts/loader/InterpretedModules.h"

#include "app/scripts/interp/CodeSurvey.h"

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

interp::HostFunction clientLookup(std::uint32_t address) {
    for (const LiftedLookup lookup : {g_lookupManual, g_lookupLifted, g_lookupImport}) {
        if (const interp::HostFunction found = lookup(address)) {
            return found;
        }
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
        const std::uint32_t address = runtime_->importAddress(import.name);
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
