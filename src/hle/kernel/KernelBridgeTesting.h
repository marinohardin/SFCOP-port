// For the tests only: attaches the bridge's kernel to a Store the test built (a recording or a
// throwing one) instead of the host Store on two folders. The bridge owns the Store from here
// until sfcop_hle_kernel_detach().
#pragma once

#include "platform/Store/Store.h"

#include <memory>

namespace sfcop::hle::kernel::test {

void attachKernelStore(std::unique_ptr<platform::Store> store);

} // namespace sfcop::hle::kernel::test
