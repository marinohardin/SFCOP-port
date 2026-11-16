// The interpreter's run loop, its block cache and its calls out (Interpreter.h).
#include "app/scripts/interp/Interpreter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sfcop::scripts::interp {

namespace {

constexpr std::size_t kLongestBlock = 128;
// Room left at the end of the code for the longest x86 instruction.
constexpr std::uint32_t kLongestInstruction = 15;

} // namespace

Interpreter::Interpreter(const Machine &machine) : machine_(machine) {}

void Interpreter::addCode(std::uint32_t start, std::uint32_t end, std::string script, std::uint32_t linked) {
    ranges_.push_back(CodeRange{start, end, linked != 0 ? linked : start, std::move(script)});
}

void Interpreter::removeCode(std::uint32_t start, std::uint32_t end) {
    ranges_.erase(std::remove_if(ranges_.begin(), ranges_.end(),
                                 [&](const CodeRange &r) { return r.start == start && r.end == end; }),
                  ranges_.end());
    // Every successor link may point into the range, so every link goes.
    for (auto &[address, block] : blocks_) {
        block->fallBlock = nullptr;
        block->takenBlock = nullptr;
        block->takenAddress = 0;
    }
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        if (it->first >= start && it->first < end) {
            retired_.push_back(std::move(it->second));
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
    recent_.fill({0, nullptr});
    if (active_.empty()) {
        retired_.clear();
    }
}

void Interpreter::leaveEntry(std::uint64_t serial) {
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [&](const ActiveEntry &a) { return a.serial == serial; }),
                  active_.end());
    if (active_.empty()) {
        retired_.clear();
    }
}

void Interpreter::resumeEntry(std::thread::id thread, std::uint64_t serial) {
    // The entries this thread made after the resumed one left their host frames without returning.
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [&](const ActiveEntry &a) { return a.thread == thread && a.serial > serial; }),
                  active_.end());
}

void Interpreter::loadState(Cpu &c) const {
    for (unsigned n = 0; n < 8; ++n) {
        c.r[n] = *machine_.regs[n];
    }
    c.kind = *machine_.flagKind;
    c.a = *machine_.flagA;
    c.b = *machine_.flagB;
    c.carry = *machine_.flagCarry;
    c.segment[1] = machine_.fsBase != nullptr ? *machine_.fsBase : 0;
    c.segment[2] = machine_.gsBase != nullptr ? *machine_.gsBase : 0;
}

void Interpreter::storeState(const Cpu &c) const {
    for (unsigned n = 0; n < 8; ++n) {
        *machine_.regs[n] = c.r[n];
    }
    *machine_.flagKind = c.kind;
    *machine_.flagA = c.a;
    *machine_.flagB = c.b;
    *machine_.flagCarry = c.carry;
}

namespace {

// One entry, as its landing pad's body sees it.
struct Entry {
    Interpreter *interpreter;
    Cpu *cpu;
    std::uint32_t address;
    std::thread::id thread; // the guest thread that made it
    std::uint64_t serial;   // its place in Interpreter::active_
};

} // namespace

void Interpreter::runEntry(void *context, std::uint32_t pc) {
    Entry &e = *static_cast<Entry *>(context);
    Interpreter &self = *e.interpreter;
    Cpu &c = *e.cpu;
    if (pc != 0) {
        // Resumed from a nested entry, which left its frames without returning: the registers are
        // the machine's, the flags undefined, as after a lifted landing pad.
        self.resumeEntry(e.thread, e.serial);
        self.loadState(c);
        c.kind = kFlagsNone;
        c.leave = false;
        ++self.stats_.resumes;
        self.run(c, pc);
    } else {
        self.run(c, e.address);
    }
}

void Interpreter::call(std::uint32_t address) {
    Cpu c;
    c.mem = machine_.memory;
    c.machine = &machine_;
    c.interpreter = this;
    loadState(c);
    std::memcpy(c.entryRegs, c.r, sizeof c.entryRegs);
    c.entryTop = machine_.fpTop != nullptr ? *machine_.fpTop : 0;
    c.entryEsp = c.r[kEsp];
    c.function = address;
    std::uint32_t saved = 0;
    if (machine_.currentFunction != nullptr) {
        saved = *machine_.currentFunction;
        *machine_.currentFunction = address;
    }
    ++stats_.entries;
    Entry entry{this, &c, address, std::this_thread::get_id(), nextSerial_++};
    active_.push_back(ActiveEntry{entry.thread, entry.serial});
    // The record goes however the call ends, a C++ exception from a host function included. A
    // resumePad longjmp that passes over this frame may skip it or run it: resumeEntry removes the
    // record in the first case, and removing it here in the second is the same change.
    struct LeaveOnExit {
        Interpreter &self;
        std::uint64_t serial;
        ~LeaveOnExit() { self.leaveEntry(serial); }
    } leaveOnExit{*this, entry.serial};
    if (!interprets(address)) {
        // An entry function kept past FreeLibrary: no script owns the address any more.
        char line[160];
        std::snprintf(line, sizeof line,
                      "[scripts] a call to 0x%08X came after its script was unloaded; the call fails", address);
        say(line);
        giveUp(c);
    } else if (stopped(address)) {
        giveUp(c);
    } else if (machine_.runInPad != nullptr) {
        machine_.runInPad(c.entryEsp, address, &Interpreter::runEntry, &entry);
    } else {
        runEntry(&entry, 0);
    }
    storeState(c);
    if (machine_.currentFunction != nullptr) {
        *machine_.currentFunction = saved;
    }
}

void Interpreter::leaveUpward(Cpu &c) {
    storeState(c);
    std::uint32_t function = 0;
    void *pad = machine_.padOwning != nullptr ? machine_.padOwning(c.r[kEsp], &function) : nullptr;
    if (pad == nullptr || !interprets(function)) {
        stopStray(c, c.next, "control left its frames upward, to a frame no interpreted entry owns, going to", "");
        return;
    }
    machine_.resumePad(pad, c.next);
    std::abort(); // resumePad does not return
}

void Interpreter::stopStray(Cpu &c, std::uint32_t address, const char *what, const char *after) {
    const CodeRange *range = rangeOf(c.function);
    if (range == nullptr) {
        fatal(c, address, what);
    }
    const std::string script = range->script;
    char line[384];
    std::snprintf(line, sizeof line,
                  "[scripts] stopped: %s: %s 0x%08X%s; the call into it fails, as does every later one",
                  script.empty() ? "a script" : script.c_str(), what, address, after);
    stopNamed(c, script, line);
}

void Interpreter::run(Cpu &c, std::uint32_t address) {
    Block *b = block(address);
    for (;;) {
        ++stats_.blocksRun;
        stats_.instructionsRun += b->insns.size();
        c.next = b->fallthrough;
        for (const Insn &insn : b->insns) {
            insn.run(c, insn);
        }
        // A refused access is reported here, once the block has run: the block's other refused
        // accesses read 0 and wrote nothing, so the only effects that could leave the script first
        // are a call out and a jump out, and those check straySize before they go.
        if (c.straySize != 0) {
            strayAccess(c, b->start);
        }
        if (c.leave) {
            c.leave = false;
            return;
        }
        if (c.r[kEsp] > c.entryEsp) {
            leaveUpward(c);
            if (c.leave) {
                c.leave = false;
                return;
            }
        }
        const std::uint32_t next = c.next;
        Block *following;
        if (next == b->fallthrough && b->fallBlock != nullptr) {
            following = b->fallBlock;
        } else if (next == b->takenAddress && b->takenBlock != nullptr) {
            following = b->takenBlock;
        } else {
            if (!interprets(next)) {
                stopStray(c, next, "a return or jump went to", ", outside its code");
                c.leave = false;
                return;
            }
            following = block(next);
            if (next == b->fallthrough) {
                b->fallBlock = following;
            } else {
                b->takenAddress = next;
                b->takenBlock = following;
            }
        }
        b = following;
    }
}

Block *Interpreter::block(std::uint32_t address) {
    auto &slot = recent_[(address ^ (address >> 12)) & (recent_.size() - 1)];
    if (slot.first == address && slot.second != nullptr) {
        return slot.second;
    }
    Block *found;
    const auto it = blocks_.find(address);
    if (it != blocks_.end()) {
        found = it->second.get();
    } else {
        found = decodeBlock(address);
    }
    slot = {address, found};
    return found;
}

Block *Interpreter::decodeBlock(std::uint32_t address) {
    auto made = std::make_unique<Block>();
    Block *b = made.get();
    b->start = address;
    std::uint32_t at = address;
    const CodeRange *range = rangeOf(address);
    const std::uint32_t rangeEnd = range != nullptr ? range->end : 0;
    if (range == nullptr) {
        Cpu c;
        fatal(c, address, "control reached an address outside every script's code");
    }
    for (;;) {
        Insn insn;
        const bool known = decodeInstruction(*this, machine_.memory + at, at, insn);
        ++stats_.instructionsDecoded;
        b->insns.push_back(insn);
        at = insn.next;
        if (!known || insn.endsBlock || b->insns.size() >= kLongestBlock ||
            at + kLongestInstruction > rangeEnd) {
            break;
        }
    }
    b->fallthrough = at;
    ++stats_.blocksDecoded;
    blocks_.emplace(address, std::move(made));
    return b;
}

bool Interpreter::callHost(Cpu &c, std::uint32_t target) {
    const HostFunction fn = machine_.lookup != nullptr ? machine_.lookup(target) : nullptr;
    if (fn == nullptr) {
        // As an indirect call in lifted code that nothing answers: said, and eax is 0. A script
        // that does it in a loop would fill the log, so only the first few are said.
        ++stats_.unresolvedCalls;
        if (stats_.unresolvedCalls <= kUnresolvedCallsSaid) {
            char line[160];
            std::snprintf(line, sizeof line, "[scripts] ICALL: unresolved VA 0x%08X from 0x%08X%s", target,
                          c.function,
                          stats_.unresolvedCalls == kUnresolvedCallsSaid ? "; further ones are not said" : "");
            say(line);
        }
        c.r[kEax] = 0;
        return false;
    }
    ++stats_.callsOut;
    storeState(c);
    std::uint32_t saved = 0;
    if (machine_.currentFunction != nullptr) {
        saved = *machine_.currentFunction;
    }
    fn();
    if (machine_.currentFunction != nullptr) {
        *machine_.currentFunction = saved;
    }
    loadState(c);
    if (stopped(c.function)) {
        giveUp(c); // the script stopped while the call was out: this entry gives up as well
    }
    return true;
}

void Interpreter::callOut(Cpu &c, std::uint32_t target) {
    if (c.straySize != 0) {
        return; // the block's stray access stops the script before anything is called with its values
    }
    if (!callHost(c, target)) {
        c.r[kEsp] += 4; // the return address the call pushed, which no host function popped
    }
}

void Interpreter::jumpOut(Cpu &c, std::uint32_t target) {
    // The host function returns for whoever called the jump's function: its `ret` pops that
    // caller's return address unread, so it is read here first.
    if (c.straySize != 0) {
        return;
    }
    const std::uint32_t esp = c.r[kEsp];
    const std::uint32_t returnTo = c.load32(esp);
    if (!callHost(c, target)) {
        c.r[kEsp] += 4;
    }
    if (esp == c.entryEsp) {
        c.leave = true;
    } else {
        c.next = returnTo;
    }
}

void Interpreter::backEdge(Cpu &c) {
    if (machine_.backEdges == nullptr) {
        return;
    }
    if ((++*machine_.backEdges & (kYieldEvery - 1)) == 0 && machine_.yieldHook != nullptr &&
        *machine_.yieldHook != nullptr) {
        storeState(c);
        (*machine_.yieldHook)();
        loadState(c);
    }
}

void Interpreter::backEdges(Cpu &c, std::uint32_t count) {
    if (machine_.backEdges == nullptr) {
        return;
    }
    const std::uint32_t before = *machine_.backEdges;
    *machine_.backEdges = before + count;
    // count < kYieldEvery: the edges passed a multiple of it when the bits above it changed.
    if (((before ^ (before + count)) & ~(kYieldEvery - 1)) != 0 && machine_.yieldHook != nullptr &&
        *machine_.yieldHook != nullptr) {
        storeState(c);
        (*machine_.yieldHook)();
        loadState(c);
    }
}

void Interpreter::strayAccess(Cpu &c, std::uint32_t block) {
    char reached[160];
    std::snprintf(reached, sizeof reached,
                  "an access of %u bytes at 0x%08X, past the end of guest memory, in the block", c.straySize,
                  c.strayAddress);
    c.straySize = 0;
    stopScript(c, block, reached);
}

void Interpreter::say(const char *line) const {
    if (machine_.report != nullptr) {
        machine_.report(line);
    } else {
        std::fprintf(stderr, "%s\n", line);
    }
}

void Interpreter::divideFault(Cpu &c, std::uint32_t code) {
    if (machine_.divideFaultHook != nullptr && *machine_.divideFaultHook != nullptr) {
        storeState(c);
        (*machine_.divideFaultHook)(code);
        loadState(c);
    }
}

void Interpreter::stopScript(Cpu &c, std::uint32_t address, const char *reached, const char *because) {
    const CodeRange *range = rangeOf(address);
    const std::string script = range != nullptr ? range->script : std::string();
    const std::uint32_t linked = range != nullptr ? range->linked + (address - range->start) : address;
    const std::uint8_t *p = machine_.memory + address;
    char line[384];
    std::snprintf(line, sizeof line,
                  "[scripts] stopped: %s: it reached %s at 0x%08X (bytes %02X %02X %02X %02X)%s; the call "
                  "into it fails, as does every later one",
                  script.empty() ? "a script" : script.c_str(), reached, linked, p[0], p[1], p[2], p[3], because);
    stopNamed(c, script, line);
}

void Interpreter::stopNamed(Cpu &c, const std::string &script, const char *line) {
    say(line);
    for (CodeRange &r : ranges_) {
        if (r.script == script) {
            r.stopped = true;
        }
    }
    ++stats_.stops;
    giveUp(c);
}

void Interpreter::giveUp(Cpu &c) {
    const std::uint32_t pops = argumentBytes(c.function);
    for (const unsigned n : {kEbx, kEbp, kEsi, kEdi}) {
        c.r[n] = c.entryRegs[n];
    }
    c.r[kEax] = 0;
    c.r[kEdx] = 0;
    c.r[kEsp] = c.entryEsp + 4 + pops;
    c.kind = kFlagsNone;
    if (machine_.fpTop != nullptr) {
        *machine_.fpTop = c.entryTop;
    }
    c.leave = true;
}

// The bytes of arguments the function at `address` pops: the operand of the first `ret` its own
// code reaches, following its jumps but not into its calls. 0 when it reaches none.
std::uint32_t Interpreter::argumentBytes(std::uint32_t function) const {
    std::vector<std::uint32_t> work{function};
    std::vector<std::uint32_t> seen;
    while (!work.empty() && seen.size() < 4096) {
        std::uint32_t at = work.back();
        work.pop_back();
        while (interprets(at) && std::find(seen.begin(), seen.end(), at) == seen.end()) {
            seen.push_back(at);
            const std::uint8_t *code = machine_.memory + at;
            Insn insn;
            if (!decodeInstruction(*this, code, at, insn)) {
                break;
            }
            unsigned n = 0;
            while (n + 1 < insn.length && (code[n] == 0x66 || code[n] == 0xF2 || code[n] == 0xF3 ||
                                           code[n] == 0x2E || code[n] == 0x3E || code[n] == 0x64)) {
                ++n;
            }
            const std::uint8_t op = code[n];
            if (op == 0xC3) {
                return 0;
            }
            if (op == 0xC2) {
                return static_cast<std::uint32_t>(code[n + 1] | (code[n + 2] << 8));
            }
            const bool call = op == 0xE8 || (op == 0xFF && ((code[n + 1] >> 3) & 7u) == 2);
            if (!call && insn.target != 0 && interprets(insn.target)) {
                work.push_back(insn.target);
            }
            // Past calls, traps and the instructions that stop a script: what follows is the
            // function's own code. Not past an unconditional jump.
            if (op == 0xE9 || op == 0xEB || (op == 0xFF && ((code[n + 1] >> 3) & 6u) == 4)) {
                break;
            }
            at = insn.next;
        }
    }
    return 0;
}

void Interpreter::fatal(const Cpu &c, std::uint32_t address, const char *what) {
    (void)c;
    char message[256];
    std::uint8_t p[4] = {}; // left 0 for an address in the guest's last three bytes
    if (Cpu::fits(address, sizeof p)) {
        std::memcpy(p, machine_.memory + address, sizeof p);
    }
    std::snprintf(message, sizeof message,
                  "[scripts] interpreter: %s at 0x%08X (bytes %02X %02X %02X %02X)", what, address,
                  p[0], p[1], p[2], p[3]);
    if (machine_.fatal != nullptr) {
        machine_.fatal(message);
    }
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

} // namespace sfcop::scripts::interp
