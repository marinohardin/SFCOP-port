// Acceptance (DESIGN §5.7): a key script in the oracle driver's steps form (oracle/driver/,
// `key <vk>` presses and releases a key 50 ms apart, `sleep <ms>` waits) replayed into the SDL3
// Input backend, read by the game's way of reading the keyboard (a buffered GetDeviceData once a
// frame), gives the same command sequence the spike's DirectInput gives for the same script.
//
// The driver cannot run against the native build yet (it is a Win32 program that types into the
// game's window inside Wine), so the comparison is at the level of DIK_ records. The spike's
// behaviour is modelled from its header comment: at each read it compares the keyboard state
// with the state it last reported and queues a record per changed key, in DIK_ order; so a press
// and release between two reads are both lost. src/hle/dinput/README.md records both sequences.
#include "hle/dinput/DinputTestUtil.h"
#include "hle/dinput/DirectInput.h"
#include "hle/dinput/KeyTable.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

#include <array>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace sfcop::hle::dinput {
namespace {

// The script, in the driver's form. The window step finds the game's window at once here.
constexpr const char *kScript = R"(# B3 acceptance: keys the game reads through DirectInput
window 120 Starfleet Command
sleep 1000
key 0x1B
sleep 300
key 0x41
sleep 300
key 0x20
sleep 300
key 0x70
sleep 300
key 0x26
sleep 300
key 0x0D
sleep 500
)";

// The Windows virtual-key codes the script uses, as USB usage IDs (written out by hand).
const std::map<unsigned, platform::Scancode> kUsageOfVk = {
    {0x1B, 0x29}, // VK_ESCAPE
    {0x41, 0x04}, // 'A'
    {0x20, 0x2C}, // VK_SPACE
    {0x70, 0x3A}, // VK_F1
    {0x26, 0x52}, // VK_UP
    {0x0D, 0x28}, // VK_RETURN
};

constexpr std::uint32_t kHoldMs = 50;  // the driver's Sleep(50) between down and up
constexpr std::uint32_t kFrameMs = 16; // the game reads its keyboard once a frame

// The script as timed key events.
std::vector<platform::InputEvent> eventsOf(const std::string &script) {
    std::vector<platform::InputEvent> events;
    std::istringstream lines(script);
    std::string line;
    std::uint32_t now = 0;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string step;
        words >> step;
        if (step == "sleep") {
            std::uint32_t ms = 0;
            words >> ms;
            now += ms;
        } else if (step == "key") {
            std::string code;
            words >> code;
            platform::Scancode usage = kUsageOfVk.at(std::stoul(code, nullptr, 0));
            events.push_back(test::keyEvent(true, usage, now));
            now += kHoldMs;
            events.push_back(test::keyEvent(false, usage, now));
        }
    }
    return events;
}

struct Command {
    std::uint32_t dik;
    bool down;
    bool operator==(const Command &other) const { return dik == other.dik && down == other.down; }
};

std::string describe(const std::vector<Command> &commands) {
    std::ostringstream out;
    for (const Command &command : commands) {
        out << std::hex << command.dik << (command.down ? "+ " : "- ");
    }
    return out.str();
}

// Replays `events` into an Input that one keyboard reads, frame by frame, `frameMs` apart.
std::vector<Command> throughModule(const std::vector<platform::InputEvent> &events,
                                   std::uint32_t frameMs) {
    std::unique_ptr<platform::Input> input = platform::makeInput();
    platform::ThreadsMock threads;
    DirectInput *directInput = nullptr;
    KeyboardDevice *keyboard = nullptr;
    REQUIRE(DirectInput::Create(0x0700, false, *input, threads, &directInput) == kDiOk);
    REQUIRE(directInput->CreateDevice(&kGuidSysKeyboard, &keyboard, false) == kDiOk);
    DataFormat format = test::keyboardFormat();
    REQUIRE(keyboard->SetDataFormat(&format) == kDiOk);
    REQUIRE(keyboard->SetCooperativeLevel(true, 0x16) == kDiOk);
    REQUIRE(keyboard->SetProperty(kPropBufferSize, test::deviceHeader(), 64) == kDiOk);
    REQUIRE(keyboard->Acquire() == kDiOk);

    std::vector<Command> commands;
    std::size_t next = 0;
    std::uint32_t end = events.back().timeMs + 2 * frameMs;
    for (std::uint32_t frame = 0; frame <= end; frame += frameMs) {
        while (next < events.size() && events[next].timeMs < frame) {
            input->OnInputEvent(events[next++]); // the window's pump
        }
        DeviceObjectData records[64];
        std::uint32_t count = 64;
        REQUIRE(keyboard->GetDeviceData(20, records, &count, 0) == kDiOk);
        for (std::uint32_t i = 0; i < count; ++i) {
            commands.push_back(Command{records[i].ofs, records[i].data == 0x80});
        }
    }
    keyboard->Release();
    directInput->Release();
    return commands;
}

// The spike's buffered read: compare the state with the last reported one at each read.
std::vector<Command> throughSpikeModel(const std::vector<platform::InputEvent> &events,
                                       std::uint32_t frameMs) {
    std::vector<Command> commands;
    std::array<bool, 256> now{};
    std::array<bool, 256> reported{};
    std::size_t next = 0;
    std::uint32_t end = events.back().timeMs + 2 * frameMs;
    for (std::uint32_t frame = 0; frame <= end; frame += frameMs) {
        while (next < events.size() && events[next].timeMs < frame) {
            const platform::InputEvent &event = events[next++];
            now[dikFromUsage(event.key)] = event.type == platform::InputEventType::kKeyDown;
        }
        for (std::uint32_t dik = 0; dik < 256; ++dik) {
            if (now[dik] != reported[dik]) {
                reported[dik] = now[dik];
                commands.push_back(Command{dik, now[dik]});
            }
        }
    }
    return commands;
}

} // namespace

TEST_CASE("the oracle driver's key script gives the spike's command sequence through Input and "
          "the DirectInput module") {
    std::vector<platform::InputEvent> events = eventsOf(kScript);
    REQUIRE(events.size() == 12);

    const std::vector<Command> expected = {
        {0x01, true}, {0x01, false}, // Esc
        {0x1E, true}, {0x1E, false}, // A
        {0x39, true}, {0x39, false}, // Space
        {0x3B, true}, {0x3B, false}, // F1
        {0xC8, true}, {0xC8, false}, // Up
        {0x1C, true}, {0x1C, false}, // Enter
    };
    std::vector<Command> module = throughModule(events, kFrameMs);
    std::vector<Command> spike = throughSpikeModel(events, kFrameMs);
    CHECK_MESSAGE(module == expected, describe(module));
    CHECK_MESSAGE(spike == expected, describe(spike));
}

TEST_CASE("a tap shorter than a frame is seen through the module and lost by the spike") {
    // Two keys tapped within one 100 ms frame: Esc down 10 up 20, A down 30 up 40.
    std::vector<platform::InputEvent> events = {
        test::keyEvent(true, 0x29, 10),
        test::keyEvent(false, 0x29, 20),
        test::keyEvent(true, 0x04, 30),
        test::keyEvent(false, 0x04, 40),
    };
    const std::vector<Command> module = {{0x01, true}, {0x01, false}, {0x1E, true}, {0x1E, false}};
    CHECK(throughModule(events, 100) == module);
    CHECK(throughSpikeModel(events, 100).empty());
}

} // namespace sfcop::hle::dinput
