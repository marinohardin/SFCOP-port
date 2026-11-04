// The guest test host: the guest host (GuestHost.h) running the lifted test program ehtest.exe
// instead of the game, on the real Threads and Clock and stubs for the rest.
//
//   sfcop_guest_tests DIR DATA [ARGUMENTS]
//
// DIR holds ehtest.exe; DATA is a scratch per-user folder. ARGUMENTS go on the guest's command
// line ("uncaught" for the uncaught-throw run). SFCOP_GUEST_TRACE=1 in the environment traces each
// import call and each exception transfer.
#include "GuestHost.h"
#include "hle/kernel32rt/Runtime.h"

extern "C" {
#include "win32hle.h"
}

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace sfcop::guest {

namespace {

constexpr std::uint32_t kTestModule = 0x7FE00000u;

// sfcoptest.dll!LockDepth, stdcall (): the machine lock's depth on the calling thread.
void lockDepth() {
    RET(static_cast<std::uint32_t>(hle_mach_depth()), 0);
}

const win32hle_shim kTestExports[] = {
    {"sfcoptest.LockDepth", lockDepth},
    {nullptr, nullptr},
};

} // namespace

void InstallGameSpecifics() {
    win32hle_register(kTestExports);
    auto &modules = hle::kernel32rt::Runtime::Instance().BuiltIns();
    modules.Add("sfcoptest.dll", kTestModule);
    modules.AddExport(kTestModule, "LockDepth", hle_resolve(kTestExports[0].name));
}

} // namespace sfcop::guest

// The host loads B9's data image before it enters the program; the test program has none.
extern "C" void sfcop_guest_load_objects(void) {}

int main(int argc, char **argv) {
    using namespace sfcop;
    if (argc < 3) {
        std::fprintf(stderr, "usage: sfcop_guest_tests DIR DATA [ARGUMENTS]\n");
        return 2;
    }
    guest::Options options;
    options.installDir = argv[1];
    options.dataDir = argv[2];
    options.exeName = "ehtest.exe";
    options.arguments = argc > 3 ? argv[3] : "";
    options.headless = true;
    options.trace = std::getenv("SFCOP_GUEST_TRACE") != nullptr;
    options.watchdogSeconds = 60;
    auto window = platform::makeWindowStub();
    auto gpu = platform::makeGpuStub();
    auto input = platform::makeInputStub();
    auto audio = platform::makeAudioStub();
    auto store = platform::makeStoreStub();
    auto transport = platform::makeTransportStub();
    auto clock = platform::makeClock();
    auto threads = platform::makeThreads();
    guest::Components components{*window, *gpu,       *input, *audio,
                                 *store,  *transport, *clock, *threads};
    return guest::Run(options, components);
}
