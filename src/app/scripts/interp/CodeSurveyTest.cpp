// The code survey on images built here by hand: .text at rva 0x1000, .data at rva 0x2000, laid out
// at 0x60000000. 0F 05 (syscall) is an instruction the interpreter does not know.
#include "app/scripts/interp/CodeSurvey.h"

#include <doctest/doctest.h>

#include <cstring>
#include <initializer_list>

using namespace sfcop::scripts::interp;

namespace {

constexpr std::uint32_t kBase = 0x60000000;

struct Built {
    ScriptImage image;
    Machine machine;
    Interpreter interpreter{machine};

    // .text of `text` bytes at rva 0x1000, .data after it (at rva 0x2000 for the default size).
    explicit Built(std::uint32_t text = 0x200) {
        const std::uint32_t data = text <= 0x1000 ? 0x2000 : 0x1000 + text;
        image.base = kBase;
        image.linkedBase = 0x10000000;
        image.size = data + 0x1000 > 0x10000 ? data + 0x1000 : 0x10000;
        image.bytes.assign(data + 0x1000, 0xC3); // ret wherever nothing else is put
        image.sections = {{0x1000, text, true, false}, {data, 0x100, false, true}};
        image.entry = kBase + 0x1000;
        interpreter.addCode(kBase + 0x1000, kBase + 0x1000 + text);
    }
    void code(std::uint32_t rva, std::initializer_list<std::uint8_t> bytes) {
        std::memcpy(image.bytes.data() + rva, bytes.begin(), bytes.size());
    }
    // A relocated pointer at rva: to rva `to` in the image.
    void pointer(std::uint32_t rva, std::uint32_t to) {
        const std::uint32_t va = kBase + to;
        std::memcpy(image.bytes.data() + rva, &va, 4);
        image.pointers.push_back(rva);
    }
    SurveyResult survey() const { return surveyCode(interpreter, image); }
};

constexpr std::initializer_list<std::uint8_t> kUnknown = {0x0F, 0x05};

// call 0x1010; ret; then a switch on eax over three cases through a jump table at 0x1100, its
// displacement relocated, and a byte table read at 0x1180.
void switchOverThree(Built &b) {
    b.code(0x1000, {0xE8, 0x0B, 0x00, 0x00, 0x00, // call 0x1010
                    0xC3});                       // ret
    b.code(0x1006, kUnknown);                     // after the ret: never reached
    b.code(0x1010, {0x0F, 0xB6, 0x05, 0x80, 0x11, 0x00, 0x60, // movzx eax, byte [0x60001180]
                    0x83, 0xF8, 0x02,                         // cmp eax, 2
                    0x77, 0x0C,                               // ja 0x1028
                    0xFF, 0x24, 0x85, 0x00, 0x11, 0x00, 0x60}); // jmp [eax*4 + 0x60001100]
    b.pointer(0x1013, 0x1180);
    b.pointer(0x101F, 0x1100);
    b.code(0x1023, kUnknown); // after the indirect jump: never reached
    b.pointer(0x1100, 0x1030);
    b.pointer(0x1104, 0x1040);
    b.pointer(0x1108, 0x1050);
    b.code(0x1180, kUnknown); // the byte table: data, never decoded
}

} // namespace

TEST_CASE("the survey follows calls, branches and jump tables to an instruction it does not know") {
    Built b;
    switchOverThree(b);
    b.code(0x1050, kUnknown); // the third case
    const SurveyResult result = b.survey();
    REQUIRE(result.unknown.has_value());
    CHECK(*result.unknown == kBase + 0x1050);
}

TEST_CASE("the survey decodes nothing past a ret or a jump, nor a table an instruction reads") {
    Built b;
    switchOverThree(b);
    const SurveyResult result = b.survey();
    CHECK_FALSE(result.unknown.has_value());
    // call, ret; movzx, cmp, ja, jmp; the ja's target and the three cases, each a ret.
    CHECK(result.instructions == 10u);
}

TEST_CASE("the survey enters code through pointers in data and relocated immediates") {
    Built b;
    b.code(0x1000, {0x68, 0x20, 0x10, 0x00, 0x60, // push 0x60001020, relocated
                    0xC3});
    b.pointer(0x1001, 0x1020);
    b.pointer(0x2000, 0x1040); // a function pointer in .data
    SUBCASE("a handler pushed") {
        b.code(0x1020, kUnknown);
        CHECK(b.survey().unknown == kBase + 0x1020);
    }
    SUBCASE("a function in a table") {
        b.code(0x1040, kUnknown);
        CHECK(b.survey().unknown == kBase + 0x1040);
    }
    SUBCASE("neither") {
        b.code(0x1060, kUnknown); // nothing reaches it
        CHECK_FALSE(b.survey().unknown.has_value());
    }
}

TEST_CASE("the survey stops at a relocated word after a call that does not return") {
    Built b;
    b.code(0x1000, {0xE8, 0xFB, 0xEF, 0xFF, 0xEF}); // call 0x50000000, a function outside that throws
    b.pointer(0x1005, 0x050F);                      // a jump table after it: its first word reads 0F 05
    b.pointer(0x1009, 0x1030);
    const SurveyResult result = b.survey();
    CHECK_FALSE(result.unknown.has_value());
    CHECK(result.instructions == 1u);
}

TEST_CASE("after a call that may not return the survey is not sure it is in code, and does not refuse") {
    Built b;
    b.image.imports = {{0x2010, "KERNEL32.DLL!ExitProcess"}, {0x2014, "KERNEL32.DLL!GetTickCount"}};
    SUBCASE("an import that never returns") {
        b.code(0x1000, {0xFF, 0x15, 0x10, 0x20, 0x00, 0x60}); // call [ExitProcess]
        b.code(0x1006, kUnknown);                             // data, as far as anyone knows
        const SurveyResult result = b.survey();
        CHECK_FALSE(result.unknown.has_value());
        CHECK(result.uncertain == 1u);
        CHECK(result.firstUncertain == kBase + 0x1006);
    }
    SUBCASE("an import that returns") {
        b.code(0x1000, {0xFF, 0x15, 0x14, 0x20, 0x00, 0x60}); // call [GetTickCount]
        b.code(0x1006, kUnknown);
        CHECK(b.survey().unknown == kBase + 0x1006);
    }
    SUBCASE("a function of the script that reaches no ret") {
        b.code(0x1000, {0xE8, 0x1B, 0x00, 0x00, 0x00});       // call 0x1020
        b.code(0x1005, kUnknown);
        b.code(0x1020, {0xFF, 0x15, 0x10, 0x20, 0x00, 0x60}); // call [ExitProcess]
        b.code(0x1026, {0xCC});                               // int3
        const SurveyResult result = b.survey();
        CHECK_FALSE(result.unknown.has_value());
        CHECK(result.uncertain == 1u);
    }
    SUBCASE("a function of the script that returns, past its own calls") {
        b.code(0x1000, {0xE8, 0x1B, 0x00, 0x00, 0x00});       // call 0x1020
        b.code(0x1005, kUnknown);
        b.code(0x1020, {0x74, 0x0E,                           // je 0x1030
                        0xE8, 0x19, 0x00, 0x00, 0x00,         // call 0x1040
                        0xCC});                               // int3
        b.code(0x1030, {0xE8, 0x0B, 0x00, 0x00, 0x00,         // call 0x1040
                        0xC3});                               // ret
        b.code(0x1040, {0xC2, 0x04, 0x00});                   // ret 4
        CHECK(b.survey().unknown == kBase + 0x1005);
    }
}

TEST_CASE("an instruction of an extension the processor does not report ends a path without refusing") {
    Built b;
    b.code(0x1000, {0x74, 0x03,       // je 0x1005
                    0x0F, 0xFD, 0xC1, // paddw mm0, mm1: the script stops here if it runs it
                    0x66, 0x0F, 0xEF, 0xC0, // pxor xmm0, xmm0
                    0xC3});
    b.code(0x1009, kUnknown); // after the SSE instruction: never runs
    const SurveyResult result = b.survey();
    CHECK_FALSE(result.unknown.has_value());
    CHECK(result.extensions == 2u);
    CHECK(result.instructions == 3u); // je, paddw, pxor
}

TEST_CASE("a branch just below the image, into another script's code, is not surveyed as this image's") {
    Built b;
    b.interpreter.addCode(kBase - 0x1000, kBase); // a script in the slot below
    b.code(0x1000, {0xE9, 0xF7, 0xEF, 0xFF, 0xFF}); // jmp kBase - 4
    const SurveyResult result = b.survey();
    CHECK_FALSE(result.unknown.has_value());
    CHECK(result.instructions == 1u);
}

TEST_CASE("a call chain deeper than the survey follows is not sure to return, and does not refuse") {
    // call 0x1010; then an instruction it does not know. At 0x1010, 100,000 calls each to the next
    // instruction, then a ret: the chain returns, but the survey stops following it long before.
    constexpr std::uint32_t kLinks = 100000;
    Built b(0x10 + kLinks * 5 + 0x10);
    b.code(0x1000, {0xE8, 0x0B, 0x00, 0x00, 0x00}); // call 0x1010
    b.code(0x1005, kUnknown);
    for (std::uint32_t n = 0; n < kLinks; ++n) {
        b.code(0x1010 + n * 5, {0xE8, 0x00, 0x00, 0x00, 0x00}); // call the next instruction
    }
    const SurveyResult result = b.survey();
    CHECK_FALSE(result.unknown.has_value());
    CHECK(result.uncertain == 1u);
    REQUIRE(result.firstUncertain.has_value());
    CHECK(*result.firstUncertain == kBase + 0x1005);
}

TEST_CASE("in a recursion cycle a function that returns is found to return") {
    // A (0x1100): test eax, eax; jz to its ret; call B; ret. B (0x1120): call A; ret. B returns
    // because A does; the entry calls A, then B, then reaches an instruction it does not know.
    Built b;
    b.code(0x1000, {0xE8, 0xFB, 0x00, 0x00, 0x00,  // call 0x1100 (A)
                    0xE8, 0x16, 0x01, 0x00, 0x00}); // call 0x1120 (B)
    b.code(0x100A, kUnknown);
    b.code(0x1100, {0x85, 0xC0,                    // test eax, eax
                    0x74, 0x05,                    // jz 0x1109
                    0xE8, 0x17, 0x00, 0x00, 0x00,  // call 0x1120 (B)
                    0xC3});                        // ret
    b.code(0x1120, {0xE8, 0xDB, 0xFF, 0xFF, 0xFF,  // call 0x1100 (A)
                    0xC3});                        // ret
    const SurveyResult result = b.survey();
    REQUIRE(result.unknown.has_value());
    CHECK(*result.unknown == kBase + 0x100A);
}
