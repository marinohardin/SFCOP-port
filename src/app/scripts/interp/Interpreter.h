// The x86-32 interpreter that runs a mission script's code on the guest machine (DESIGN §5.9): the
// lifted game calls into script code through a host function per entry address (EntryThunks.h),
// and the script calls back out to the game, to the Windows layer and to other scripts through the
// same lookup an indirect call in lifted code uses. No machine code is generated: each instruction
// is decoded once into a handler and its operands, in blocks cached by guest address.
//
// Control enters with the caller's return address already on the guest stack, as for any lifted
// call, and leaves when a `ret` pops that slot. When it leaves upward instead, the guest stack
// switched to a frame of an entry further up (a catch's continuation), it goes on in that entry
// (Machine::runInPad). Inside, calls between script functions push their
// real return addresses; a call out pushes the address after the call, which the host function
// pops unread.
//
// Threading: runs with the guest machine held. A call out may hand the machine to another guest
// thread, which may enter the interpreter in turn; each entry keeps its state on the host stack.
#pragma once

#include "app/scripts/interp/Insn.h"
#include "app/scripts/interp/Machine.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sfcop::scripts::interp {

struct Block {
    std::uint32_t start = 0;
    std::uint32_t fallthrough = 0; // where control goes when no instruction says otherwise
    std::vector<Insn> insns;
    // The blocks control went to last, so a loop does not look its successor up again.
    Block *fallBlock = nullptr;
    std::uint32_t takenAddress = 0;
    Block *takenBlock = nullptr;
};

struct InterpreterStats {
    std::uint64_t blocksDecoded = 0;
    std::uint64_t instructionsDecoded = 0;
    std::uint64_t blocksRun = 0;
    std::uint64_t instructionsRun = 0; // the instructions of the blocks run
    std::uint64_t callsOut = 0;
    std::uint64_t entries = 0;
    std::uint64_t resumes = 0; // entries resumed from a nested one (a catch, an __except, a longjmp)
    std::uint64_t stops = 0;   // scripts stopped (stopScript)
    std::uint64_t unresolvedCalls = 0; // calls out to an address nothing answers
};

// The unresolved calls out that are said; the rest are only counted.
inline constexpr std::uint64_t kUnresolvedCallsSaid = 16;

class Interpreter {
public:
    explicit Interpreter(const Machine &machine);
    Interpreter(const Interpreter &) = delete;
    Interpreter &operator=(const Interpreter &) = delete;

    // Script code in [start, end): addresses there are interpreted, every other one is called out.
    // `script` names it in the lines the interpreter writes, at `linked`, the address its file
    // gives `start` (0: start itself), so a mod author finds it in a disassembly of the file.
    void addCode(std::uint32_t start, std::uint32_t end, std::string script = {}, std::uint32_t linked = 0);
    // Forgets the range and every block decoded from it.
    void removeCode(std::uint32_t start, std::uint32_t end);
    bool interprets(std::uint32_t address) const { return rangeOf(address) != nullptr; }
    // Whether the script whose code holds `address` was stopped.
    bool stopped(std::uint32_t address) const {
        const CodeRange *range = rangeOf(address);
        return range != nullptr && range->stopped;
    }

    // Runs the script function at `address` as a call from lifted code: registers and flags from
    // the machine, the return address on its stack; back to the machine when it returns.
    void call(std::uint32_t address);

    const InterpreterStats &stats() const { return stats_; }
    // The entries running now, on every guest thread: those inside script code or waiting in a
    // call out. Blocks removed while any is running are kept until none is.
    std::size_t activeEntries() const { return active_.size(); }
    std::size_t retiredBlocks() const { return retired_.size(); }
    const Machine &machine() const { return machine_; }

    // For the handlers. A target nothing answers is said (the first kUnresolvedCallsSaid of
    // them) and gives eax 0, as an unresolved indirect call in lifted code does.
    void callOut(Cpu &c, std::uint32_t target);  // a call: the return address is pushed
    void jumpOut(Cpu &c, std::uint32_t target);  // a tail jump: the caller's is on the stack
    void backEdge(Cpu &c);
    // `count` back edges at once (fewer than kYieldEvery), as a long rep counts its iterations.
    void backEdges(Cpu &c, std::uint32_t count);
    void divideFault(Cpu &c, std::uint32_t code);
    // One line to the client's report, or to stderr when it has none.
    void say(const char *line) const;
    [[noreturn]] void fatal(const Cpu &c, std::uint32_t address, const char *what);
    // Script code that cannot go on (an instruction of an extension the processor does not report,
    // one the interpreter does not know, a trap) at `address`: one line names the script, what it
    // reached (`reached`, then `because`) and where; every range of that script is stopped. The
    // entry running gives up as a failed call: eax and edx 0, the caller's callee-saved registers,
    // x87 depth and stack as they were, its arguments popped as the function's `ret` pops them
    // (when a search from its start reaches that `ret`; past an instruction it does not know it
    // cannot, and pops only the return address).
    // Every entry below it, waiting in a call out, gives up the same way when that call returns,
    // and any later call into the script gives up at once. Neither the game nor the client stops.
    void stopScript(Cpu &c, std::uint32_t address, const char *reached, const char *because = "");

    void loadState(Cpu &c) const;
    void storeState(const Cpu &c) const;

private:
    struct CodeRange {
        std::uint32_t start, end, linked;
        std::string script;
        bool stopped = false;
    };
    const CodeRange *rangeOf(std::uint32_t address) const {
        for (const CodeRange &range : ranges_) {
            if (address - range.start < range.end - range.start) {
                return &range;
            }
        }
        return nullptr;
    }
    void giveUp(Cpu &c);
    std::uint32_t argumentBytes(std::uint32_t function) const;
    bool callHost(Cpu &c, std::uint32_t target);
    static void runEntry(void *context, std::uint32_t pc);
    // Control left the entry's frames upward: resumed in the interpreted entry whose landing pad
    // owns that frame, or else the running script is stopped (stopStray).
    void leaveUpward(Cpu &c);
    // Control went where no script's code is (a `ret` or computed jump through a damaged value,
    // or upward past every interpreted entry): the script whose entry is running is stopped as
    // stopScript stops it, with `what`, `address` and `after` in its line. Only when no script owns the
    // entry does the client end (fatal).
    void stopStray(Cpu &c, std::uint32_t address, const char *what, const char *after);
    // The stopping half of stopScript: `line` said, every range of `script` stopped, the entry gives up.
    void stopNamed(Cpu &c, const std::string &script, const char *line);
    // An access in the block at `block` ran past the guest's last byte (Cpu::strayed): stopScript.
    void strayAccess(Cpu &c, std::uint32_t block);
    Block *block(std::uint32_t address);
    Block *decodeBlock(std::uint32_t address);
    void run(Cpu &c, std::uint32_t address);

    Machine machine_;
    std::vector<CodeRange> ranges_;
    std::unordered_map<std::uint32_t, std::unique_ptr<Block>> blocks_;
    std::array<std::pair<std::uint32_t, Block *>, 4096> recent_{};
    std::vector<std::unique_ptr<Block>> retired_; // removed while code may still be running them
    // One per running entry, in the order they were entered. Entries on one guest thread nest;
    // those of different threads interleave as the machine is handed between them. A resume
    // abandons the entries its own thread entered after the one resumed, and only those.
    struct ActiveEntry {
        std::thread::id thread;
        std::uint64_t serial;
    };
    std::vector<ActiveEntry> active_;
    std::uint64_t nextSerial_ = 0;
    void leaveEntry(std::uint64_t serial);
    void resumeEntry(std::thread::id thread, std::uint64_t serial);
    InterpreterStats stats_;
};

// Decodes the instruction at `address` (its bytes at `code`) into `out`. Returns false, with
// out.run a handler that reports it, when the interpreter does not know the instruction; the
// block ends there. Decode.cpp.
bool decodeInstruction(const Interpreter &interpreter, const std::uint8_t *code, std::uint32_t address,
                       Insn &out);

} // namespace sfcop::scripts::interp
