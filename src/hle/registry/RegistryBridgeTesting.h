// For the tests only: attaches the bridge's registry to a Store the test built (a throwing one)
// instead of the host Store on a folder. The bridge owns the Store from here until
// sfcop_hle_registry_detach().
#pragma once

#include "platform/Store/Store.h"

#include <memory>

namespace sfcop::hle::registry::test {

void attachRegistryStore(std::unique_ptr<platform::Store> store);

} // namespace sfcop::hle::registry::test
