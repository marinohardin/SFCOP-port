#!/bin/bash
# Compiles one function from the SDK source, to be linked in place of its delinked copy:
# tOSVersion::IsWindows98 (0x6f55a0, 28 bytes plus padding, byte-exact SDK library match, ten
# callers, no calls of its own).
#
#   spike/delink/replace/build.sh
#
# The SDK's Scripts/ and STL/ trees are staged under work/delink/replace/sdk (the original is
# never touched). The translation unit is generated there from the staged OSVersion.cpp: its
# include block plus the one function, so the object defines nothing else that the delinked
# object also defines. USE_PCH (the SDK's own switch for builds with a precompiled header), with
# windows.h and a declaration of std::auto_ptr in front, keeps OSVersion.h from including the
# STL headers, whose static initializers would add start-up code and symbols the exe already
# has. Flags are the Release "ADD CPP" line of the SDK's _Shared.dsp, minus the browse-info and
# precompiled-header switches (/FR /YX /FD), with the SDK's STL headers first (the SDK readme
# says to copy them over the compiler's), as toolchain/calibration does.
# Output: work/delink/replace/IsWindows98.obj (the whole translation unit) and
# IsWindows98_only.obj (the function's own section, tools/extract_function.py), which is linked.
set -euo pipefail
source "$(dirname "$0")/../env.sh"
export SFCOP_ROOT

API="$GAME/OP API - R2.1"
R="$WORK/replace"
mkdir -p "$R/sdk"
rsync -a "$API/Scripts" "$API/STL" "$R/sdk/"
chmod -R u+w "$R/sdk"
COMMON="$R/sdk/Scripts/shared/Source/Taldren/Common"
SRC="$COMMON/_IsWindows98.cpp"
{
	printf '#define USE_PCH\n#include <windows.h>\nnamespace std { template<class _Ty> class auto_ptr; }\n'
	sed -n '1,/^#include <OSVersion.h>/p' "$COMMON/OSVersion.cpp"
	sed -n '/^bool tOSVersion::IsWindows98()/,/^}/p' "$COMMON/OSVersion.cpp"
} > "$SRC"
grep -q "IsWindows98" "$SRC"

dsp="$R/sdk/Scripts/shared/_Shared.dsp"
line=$(grep -m1 '^# ADD CPP /nologo /MT' "$dsp" | sed 's/^# ADD CPP //; s/\r$//')
eval "flags=($line)"
keep=()
for f in "${flags[@]}"; do
	case "$f" in /FR|/YX|/FD) ;; *) keep+=("$f") ;; esac
done
cd "$R/sdk/Scripts/shared"
"$TOOLCHAIN/bin/cl" /I "Z:$R/sdk/STL/Include" "${keep[@]}" "/FoZ:$R/IsWindows98.obj" \
	"Z:$SRC" > "$R/IsWindows98.cl.log" 2>&1 || { cat "$R/IsWindows98.cl.log"; exit 1; }
python3 "$SPIKE/tools/coffsyms.py" "$R/IsWindows98.obj" | tee "$R/IsWindows98.symbols.txt"
python3 "$SPIKE/tools/extract_function.py" "$R/IsWindows98.obj" '?IsWindows98@tOSVersion@@QAE_NXZ' "$R/IsWindows98_only.obj"
python3 "$SPIKE/tools/coffsyms.py" "$R/IsWindows98_only.obj"
