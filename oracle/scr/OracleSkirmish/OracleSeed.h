/********************************************************************************************************\

	File Name:	OracleSeed.h
	Descript:	Locates and pins the random number generators inside the host executable.

\********************************************************************************************************/

#if !defined( OracleSeed_h )
#define OracleSeed_h

#include <string>

namespace nOracleSeed
{
	//
	//	Searches the host exe's .text for the MSVC 6 static CRT srand()/rand() pair and for the inlined
	//	game LCG, then seeds both.  Returns a one-line description for the trace header, e.g.
	//	"crt_srand=0x007385DE lcg_seed=0x0088412C".  Anything not found is reported as "missing" and
	//	left untouched, so the script stays harmless on other builds.
	//
	std::string mPin( unsigned int CrtSeed, int LcgSeed );
}

#endif
