# Warning levels (CONVENTIONS §1, §4.2): src/, server/ and tools/ build at -Wall -Wextra -Werror;
# the decompilation (decomp/op/, compiled by VC6) builds with warnings allowed; generated and third-party code builds with warnings off.
include_guard(GLOBAL)

function(sfcop_strict_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endfunction()
