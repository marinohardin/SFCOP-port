// Windows: no clone (block cloning on ReFS is not worth the code for one import); the import
// copies.
#include "app/ImportFiles.h"

namespace sfcop::app {

bool cloneHostFile(const std::string &, const std::string &) { return false; }

} // namespace sfcop::app
