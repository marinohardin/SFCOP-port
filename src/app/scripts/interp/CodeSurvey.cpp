// The code survey (CodeSurvey.h).
#include "app/scripts/interp/CodeSurvey.h"

#include "app/scripts/interp/Extensions.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sfcop::scripts::interp {

namespace {

constexpr std::uint32_t kLongestInstruction = 15;
// The nested calls the return analysis follows, and the instructions all its walks may decode, for
// one script; far past what a compiled script needs.
constexpr std::uint32_t kDeepestCall = 256;
constexpr std::uint64_t kReturnsBudget = 16u * 1024u * 1024u;

// The opcode after the prefixes, with the byte after it (a 0x0F opcode's second byte, or ModRM),
// and whether a 66, F2 or F3 prefix came first.
struct Opcode {
    std::uint8_t op = 0;
    std::uint8_t next = 0;
    bool prefixed = false;
};

Opcode opcodeAt(const std::uint8_t *code) {
    unsigned n = 0;
    bool prefixed = false;
    while (n < kLongestInstruction - 1) {
        switch (code[n]) {
        case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65:
        case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3:
            prefixed = prefixed || code[n] == 0x66 || code[n] == 0xF2 || code[n] == 0xF3;
            ++n;
            continue;
        default:
            break;
        }
        break;
    }
    return {code[n], code[n + 1], prefixed};
}

// Control does not go on to the next instruction: ret, an unconditional or indirect jump, a trap.
bool endsPath(Opcode o) {
    switch (o.op) {
    case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCC: case 0xCF: case 0xE9: case 0xEA: case 0xEB:
    case 0xF4:
        return true;
    case 0xFF: {
        const unsigned digit = (o.next >> 3) & 7u;
        return digit == 4 || digit == 5;
    }
    default:
        return false;
    }
}

bool isRelativeBranch(Opcode o) {
    return (o.op >= 0x70 && o.op <= 0x7F) || (o.op >= 0xE0 && o.op <= 0xE3) || o.op == 0xE8 || o.op == 0xE9 ||
           o.op == 0xEB || (o.op == 0x0F && o.next >= 0x80 && o.next <= 0x8F);
}

bool isIndirectJump(Opcode o) { return o.op == 0xFF && ((o.next >> 3) & 7u) == 4; }
bool isCall(Opcode o) { return o.op == 0xE8 || (o.op == 0xFF && ((o.next >> 3) & 7u) == 2); }
bool isRet(Opcode o) { return o.op == 0xC2 || o.op == 0xC3; }
bool isExtension(Opcode o) { return o.op == 0x0F && extensionOf(o.next, o.prefixed) != Extension::kNone; }

// The imports after whose call no code need follow.
bool neverReturns(std::string_view import) {
    for (const std::string_view name : {"KERNEL32.DLL!ExitProcess", "KERNEL32.DLL!ExitThread",
                                        "KERNEL32.DLL!RaiseException", "KERNEL32.DLL!FatalExit",
                                        "KERNEL32.DLL!FatalAppExitA"}) {
        if (import.size() == name.size() &&
            std::equal(import.begin(), import.end(), name.begin(),
                       [](char a, char b) { return (a & ~0x20) == (b & ~0x20); })) {
            return true;
        }
    }
    return false;
}

class Survey {
public:
    Survey(const Interpreter &interpreter, const ScriptImage &image)
        : interpreter_(interpreter), image_(image), pointers_(image.pointers),
          state_(image.bytes.size(), kNotSeen) {
        std::sort(pointers_.begin(), pointers_.end());
    }

    SurveyResult run() {
        for (const std::uint32_t root : {image_.entry, image_.missionSpecEntry, image_.scriptEntry}) {
            enter(root, true);
        }
        for (const std::uint32_t at : image_.pointers) {
            if (!inExecutableSection(at)) {
                enter(word(at), true);
            }
        }
        // Everything certain first, so code that both kinds of path reach counts as certain.
        while (!result_.unknown && (!certain_.empty() || !uncertain_.empty())) {
            const bool certain = !certain_.empty();
            std::vector<std::uint32_t> &work = certain ? certain_ : uncertain_;
            const std::uint32_t va = work.back();
            work.pop_back();
            follow(va, certain);
        }
        return result_;
    }

private:
    enum : std::uint8_t { kNotSeen, kSeenUncertain, kSeenCertain };

    bool inCode(std::uint32_t va) const {
        // Within this image's bytes, with room for the longest instruction; `va` below the base
        // wraps rva to a large value, which the comparison refuses without adding to it.
        const std::uint32_t rva = va - image_.base;
        return interpreter_.interprets(va) && rva < image_.bytes.size() &&
               image_.bytes.size() - rva >= kLongestInstruction;
    }
    bool inExecutableSection(std::uint32_t rva) const {
        for (const ScriptSection &section : image_.sections) {
            if (section.executable && rva - section.rva < section.size) {
                return true;
            }
        }
        return false;
    }
    bool isPointer(std::uint32_t rva) const { return std::binary_search(pointers_.begin(), pointers_.end(), rva); }
    // Inside a relocated word: no instruction starts there, so a path that gets there has run
    // into data (a jump table after a call that does not return).
    bool insidePointer(std::uint32_t rva) const {
        const auto it = std::upper_bound(pointers_.begin(), pointers_.end(), rva);
        return it != pointers_.begin() && rva - *(it - 1) < 4;
    }
    std::uint32_t word(std::uint32_t rva) const {
        std::uint32_t v = 0;
        if (rva + 4 <= image_.bytes.size()) {
            std::memcpy(&v, image_.bytes.data() + rva, 4);
        }
        return v;
    }
    bool unseen(std::uint32_t va, bool certain) const {
        const std::uint8_t s = state_[va - image_.base];
        return certain ? s != kSeenCertain : s == kNotSeen;
    }
    void enter(std::uint32_t va, bool certain) {
        if (va != 0 && inCode(va) && unseen(va, certain)) {
            (certain ? certain_ : uncertain_).push_back(va);
        }
    }

    // Whether control comes back after the call at `va`. `open` is set when the answer "no" rests
    // on a function whose own answer is not settled yet (see `returns`).
    bool callReturns(const Opcode &o, const Insn &insn, bool &open) {
        if (o.op == 0xE8) {
            return !interpreter_.interprets(insn.target) || returns(insn.target, open);
        }
        // call [import slot]
        if (o.op == 0xFF && insn.rm == kMem && insn.base == kNoReg && insn.index == kNoReg) {
            for (const ScriptImport &import : image_.imports) {
                if (insn.disp == image_.base + import.iatRva) {
                    return !neverReturns(import.name);
                }
            }
        }
        return true;
    }

    // Whether a `ret` can be reached from the function at `va`, along its own jumps and past the
    // calls that return. A "yes" is always right. A "no" is the safe answer, since the code after
    // the call is then surveyed as uncertain and left to the run-time stop; it is given:
    // - for a call to a function still being worked out, further up (a recursion cycle). A "no"
    //   that rests on such an answer is not kept, and `open` tells the caller so; it is worked out
    //   again when asked after the cycle's function is settled;
    // - past kDeepestCall nested calls, and once the walks have decoded kReturnsBudget
    //   instructions, so that a long or hostile call chain costs neither the host's stack nor
    //   unbounded time.
    bool returns(std::uint32_t function, bool &open) {
        if (const auto it = returns_.find(function); it != returns_.end()) {
            if (it->second == kWorking) {
                open = true;
                return false;
            }
            return it->second == kReturns;
        }
        if (depth_ >= kDeepestCall || budget_ == 0) {
            open = true;
            return false;
        }
        returns_[function] = kWorking;
        ++depth_;
        // Marks of this walk; a nested walk (a call's) takes the next number.
        const std::uint32_t walk = ++walks_;
        if (seen_.empty()) {
            seen_.assign(image_.bytes.size(), 0);
        }
        std::vector<std::uint32_t> work{function};
        bool found = false;
        bool rests = false; // a "no" here rests on an answer not settled
        while (!work.empty() && !found && budget_ != 0) {
            std::uint32_t va = work.back();
            work.pop_back();
            while (inCode(va) && seen_[va - image_.base] != walk && !insidePointer(va - image_.base)) {
                if (budget_ == 0) {
                    rests = true;
                    break;
                }
                --budget_;
                seen_[va - image_.base] = walk;
                const std::uint8_t *code = image_.bytes.data() + (va - image_.base);
                Insn insn;
                const Opcode o = opcodeAt(code);
                if (!decodeInstruction(interpreter_, code, va, insn) || isExtension(o)) {
                    break;
                }
                if (isRet(o)) {
                    found = true;
                    break;
                }
                if (isRelativeBranch(o) && !isCall(o)) {
                    work.push_back(insn.target);
                }
                if (endsPath(o) || (isCall(o) && !callReturns(o, insn, rests))) {
                    break;
                }
                va = insn.next;
            }
        }
        if (budget_ == 0 && !found) {
            rests = true;
        }
        --depth_;
        if (found) {
            returns_[function] = kReturns;
        } else if (rests && depth_ != 0) {
            // Not kept: worked out again once what it rests on is settled.
            returns_.erase(function);
            open = true;
        } else {
            // At the top every open answer was this function's own (a call back into it), or a
            // limit; "no" is then the answer to keep.
            returns_[function] = kDoesNotReturn;
        }
        return found;
    }

    void follow(std::uint32_t va, bool certain) {
        while (inCode(va) && unseen(va, certain) && !insidePointer(va - image_.base)) {
            const std::uint32_t rva = va - image_.base;
            if (state_[rva] == kNotSeen) {
                ++result_.instructions;
            }
            state_[rva] = certain ? kSeenCertain : kSeenUncertain;
            const std::uint8_t *code = image_.bytes.data() + rva;
            Insn insn;
            const bool known = decodeInstruction(interpreter_, code, va, insn);
            if (!known) {
                if (certain) {
                    result_.unknown = va;
                } else {
                    ++result_.uncertain;
                    if (!result_.firstUncertain || va < *result_.firstUncertain) {
                        result_.firstUncertain = va;
                    }
                }
                return;
            }
            const Opcode o = opcodeAt(code);
            if (isExtension(o)) {
                ++result_.extensions;
                return;
            }
            if (isRelativeBranch(o)) {
                enter(insn.target, certain);
            }
            // A relocated immediate into code: the last four bytes are the immediate it carries.
            const std::uint32_t last = insn.next - 4 - image_.base;
            if (insn.next - va >= 5 && isPointer(last) && word(last) == insn.imm) {
                enter(insn.imm, certain);
            }
            if (isIndirectJump(o) && insn.rm == kMem && insn.index != kNoReg && insn.scale == 2) {
                // A jump table: its relocated entries, one after another, for as long as they are code.
                for (std::uint32_t slot = insn.disp - image_.base; isPointer(slot) && inCode(word(slot)); slot += 4) {
                    enter(word(slot), certain);
                }
            }
            if (endsPath(o)) {
                return;
            }
            bool open = false;
            if (isCall(o) && certain && !callReturns(o, insn, open)) {
                enter(insn.next, false);
                return;
            }
            va = insn.next;
        }
    }

    const Interpreter &interpreter_;
    const ScriptImage &image_;
    std::vector<std::uint32_t> pointers_;
    std::vector<std::uint8_t> state_;
    std::vector<std::uint32_t> certain_;
    std::vector<std::uint32_t> uncertain_;
    enum Answer : std::uint8_t { kWorking, kReturns, kDoesNotReturn };
    std::unordered_map<std::uint32_t, Answer> returns_;
    std::uint32_t depth_ = 0;
    std::uint64_t budget_ = kReturnsBudget;
    std::vector<std::uint32_t> seen_;
    std::uint32_t walks_ = 0;
    SurveyResult result_;
};

} // namespace

SurveyResult surveyCode(const Interpreter &interpreter, const ScriptImage &image) {
    return Survey(interpreter, image).run();
}

} // namespace sfcop::scripts::interp
