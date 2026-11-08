// The POSIX sockets backend on its own, without the rendezvous wrapper that makeTransport() puts
// around it: its GetRendezvous() answers kUnsupported. For the backend's own tests.
#pragma once

#include "platform/Transport/Transport.h"

#include <memory>

namespace sfcop::platform {

#if !defined(_WIN32)
std::unique_ptr<Transport> makeHostTransport();
#endif

} // namespace sfcop::platform
