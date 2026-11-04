/* src/fixes/fixes.h: the fix switches (DESIGN §5.14, §6.9; docs/FIXES.md is the list).
 *
 * Every change to the original's behavior is a named switch with a default. There are two kinds,
 * by where the switch is read:
 *
 *   build    read in game/ by the preprocessor, `#if SFCOP_FIX(fix_name)`, with the original code
 *            in the `#else` branch. Its position is fixed when the file is compiled: under VC6 it
 *            is 0 for every switch, because the byte check and the 32-bit build compile the
 *            original; in the native build it is the switch's default, or the value of
 *            -DSFCOP_FIX_BUILD_fix_name=0|1, and 0 for every switch under -DSFCOP_FIXES_OFF.
 *   runtime  read in src/ through sfcop::fixes::isEnabled("fix_name"), set from the defaults,
 *            settings/fixes.gf and the --fix NAME=on|off command line, in that rising precedence.
 *
 * The switches themselves are listed in switches.def, beside this file, which says how to declare
 * one; this header holds only the mechanism, so adding a fix does not change it. The checklist is
 * src/fixes/README.md.
 *
 * SFCOP_FIX(name) of a name that is not a declared build switch does not compile in the native
 * build: `SFCOP_FIX_name()` is then an undefined function-like macro inside `#if`. VC6 only warns
 * (C4067) and compiles the original, as it does for every switch.
 *
 * The part above the C++ section is plain C and VC6 C++, so that game/ and C code can include it.
 */
#ifndef SFCOP_FIXES_FIXES_H
#define SFCOP_FIXES_FIXES_H

/* --- Build switches: the preprocessor side ------------------------------------------------- */

/* VC6 (_MSC_VER 1200) compiles the original: every build switch is off. */
#if defined(_MSC_VER) && _MSC_VER <= 1200 && !defined(SFCOP_FIXES_OFF)
#define SFCOP_FIXES_OFF
#endif

#if defined(SFCOP_FIXES_OFF)
#define SFCOP_FIX_POSITION(on) 0
#else
#define SFCOP_FIX_POSITION(on) (on)
#endif

/* `#if SFCOP_FIX(fix_name)`: 1 where the fix is compiled in, 0 where the original is. */
#define SFCOP_FIX(name) SFCOP_FIX_##name()

/* The build switches' blocks, from the list. Its SFCOP_FIX_SWITCH lines are read by Fixes.cpp. */
#define SFCOP_FIX_SWITCH(name, kind, on, origin, description)
#include "switches.def"
#undef SFCOP_FIX_SWITCH

/* --- C++ side: the registry and the effective set (native build only) ---------------------- */

#if defined(__cplusplus) && !(defined(_MSC_VER) && _MSC_VER <= 1200)

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::fixes {

enum class Origin { Project, Community };

// Where a switch is read: a build switch is fixed at compile time, a runtime one is not.
enum class Kind { Build, Runtime };

struct Switch {
    std::string_view name;
    bool defaultOn;  // for a build switch, the position this build was compiled in
    Origin origin;
    Kind kind;
    std::string_view description;
};

// Every switch in switches.def, in its order.
const std::vector<Switch> &registry();

// One entry that could not be applied, with where it came from.
struct Problem {
    std::string source;   // "--fix" or the settings file's path
    int line;             // 1-based line in the file; for --fix, the 1-based position of the value
    std::string message;  // a sentence naming the switch or the text at fault
};

// Where a switch's effective position came from.
enum class Source { Default, File, CommandLine };

// The effective position of each switch: the command line over settings/fixes.gf over the
// default, whatever the order the two are applied in. A build switch keeps its compiled position:
// asking for the other one is a Problem, asking for the same one is accepted.
class Settings {
public:
    // The switches of registry(), at their defaults.
    Settings();
    // The given switches, at their defaults (tests).
    explicit Settings(std::vector<Switch> switches);

    // settings/fixes.gf, in the original's `.gf` form: one `fix_name = on|off` (or 1|0, either
    // quoted) per line; `//` starts a comment; blank lines are skipped; a `[Section]` line, an
    // unknown name, a bad value or a line without `=` is a Problem and the line is skipped. The
    // last line for a name wins. `source` names the file in each Problem.
    std::vector<Problem> applyFile(std::string_view text, std::string_view source);
    // The same, read from disk. A missing file is no overrides and no Problem; a file that exists
    // but cannot be read is one Problem.
    std::vector<Problem> loadFile(const std::filesystem::path &path);
    // The values of every `--fix`, as the command line parser (C4) collected them, in order:
    // each `fix_name=on|off` (or 1|0). An unknown name or a malformed value is a Problem and is
    // skipped; the last value for a name wins.
    std::vector<Problem> applyCommandLine(const std::vector<std::string> &values);

    // The effective position of `name`; false for a name that is not declared.
    bool isEnabled(std::string_view name) const;
    // Where that position came from; Source::Default for a name that is not declared.
    Source sourceOf(std::string_view name) const;
    bool isDeclared(std::string_view name) const;

    // One line per switch, in registry order: `fix_name on (default)`, `... off (--fix)`,
    // `... on (settings)`, plus ` [build]` for a build switch. Ends with a newline.
    std::string report() const;

private:
    struct Entry {
        Switch decl;
        int file = -1;     // -1 unset, else 0 or 1
        int command = -1;  // -1 unset, else 0 or 1
    };
    Entry *find(std::string_view name);
    const Entry *find(std::string_view name) const;
    // Records `on` for `name` in the given layer; a non-empty string is the Problem's message.
    std::string set(std::string_view name, bool on, Source layer);

    std::vector<Entry> entries_;
};

// The process-wide effective set. Install it once at start-up, before the game's threads start;
// until then it is Settings() at the defaults. isEnabled is safe to call from any thread after.
void install(Settings settings);
const Settings &current();

// For runtime switches read in src/: the position in current(). A name that is not declared is
// a programming error and reads as false; a doctest per switch keeps the names honest.
bool isEnabled(std::string_view name);

}  // namespace sfcop::fixes

#endif /* C++ */

#endif /* SFCOP_FIXES_FIXES_H */
