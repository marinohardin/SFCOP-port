#include "app/FileMenu.h"

namespace sfcop::app {

// No menu bar item and no file manager call here yet: the File menu is macOS's.
bool revealInFileManager(const std::string &) { return false; }

void installFileMenu(const std::vector<FileMenuEntry> &) {}

void pumpFileMenu() {}

} // namespace sfcop::app
