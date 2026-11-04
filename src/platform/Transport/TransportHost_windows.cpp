// The host backend of Transport on Windows. The Winsock2 version of TransportHost.cpp (which is
// the BSD socket backend) is not written yet: there is no Windows toolchain to build or test it
// until the CI matrix has one (cmake/toolchains/windows-x64.cmake). Until then the Windows build
// gets the stub, so it links, and every socket call fails with kUnsupported.
#include "platform/Transport/Transport.h"

namespace sfcop::platform {

std::unique_ptr<Transport> makeTransport() { return makeTransportStub(); }

} // namespace sfcop::platform
