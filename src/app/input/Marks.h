// Marks: which of the lift's marks the game has reached, for a steps file's `waitmark`.
//
// The guest host calls NoteMark() once per mark, the first time the game enters a marked function
// (src/app/guest/GuestGame.cpp, sfcop_mark). It knows nothing of steps files: it passes the
// address and the mark's name. Recording is cheap and needs no player: with no script the record
// is simply never read.
//
// Thread-safe: the game reaches marks on any of its threads; the player reads on the main thread.
#pragma once

#include <cstdint>
#include <string_view>

namespace sfcop::app::input {

// Records that the game reached the mark at `address`; `name` may be empty ("Setup", "WinMain").
void NoteMark(std::uint32_t address, std::string_view name);

// Whether a mark was reached, named by its name ("Setup") or by its address in 0x hex
// ("0x0047C155", any case).
bool MarkReached(std::string_view mark);

// Forgets every mark. For tests.
void ForgetMarks();

} // namespace sfcop::app::input
