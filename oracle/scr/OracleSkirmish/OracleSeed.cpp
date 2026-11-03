/********************************************************************************************************\

	File Name:	OracleSeed.cpp
	Descript:	The game has two random number generators that matter for a replay (found by byte search
				in StarFleetOP.exe 2.5.5.2, see oracle/README.md):

				1.	The static MSVC 6 CRT rand(), state per thread (_getptd()->_holdrand).  srand() is
					called from 9 places, seeded from timeGetTime(), time() and the thread ID.  111 call
					sites use rand().  2.5.5.2 addresses: srand 0x007385DE, rand 0x007385EB.

				2.	An inlined LCG (same constants as the CRT) with a global seed at 0x0088412C, returning
					a float in [0,1).  Only it writes the seed; its static initial value is 1.  93 call
					sites.  2.5.5.2 address: 0x0069B0E0.

				The script DLL has its own CRT, so calling srand() here would not reach the game's.  We
				find the game's functions by byte pattern (not fixed address) so a relinked build with the
				same CRT objects is still pinned, and anything not found is left alone.

\********************************************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "OracleSeed.h"

namespace
{
	//	-1 in a pattern is a wildcard byte.
	//
	//	srand:	call _getptd; mov ecx,[esp+4]; mov [eax+14h],ecx; ret
	//	rand:	call _getptd; mov ecx,[eax+14h]; imul ecx,ecx,343FDh; add ecx,269EC3h
	const int kCrtPattern[] =
	{
		0xE8, -1, -1, -1, -1, 0x8B, 0x4C, 0x24, 0x04, 0x89, 0x48, 0x14, 0xC3,
		0xE8, -1, -1, -1, -1, 0x8B, 0x48, 0x14, 0x69, 0xC9, 0xFD, 0x43, 0x03, 0x00,
		0x81, 0xC1, 0xC3, 0x9E, 0x26, 0x00,
	};

	//	mov eax,[seed]; lea ecx,[eax+eax*2]; ... lea eax,[eax+edx*4+269EC3h]; mov [seed],eax
	const int kLcgPattern[] =
	{
		0xA1, -1, -1, -1, -1, 0x8D, 0x0C, 0x40, 0x8D, 0x14, 0x88, 0xC1, 0xE2, 0x04, 0x03, 0xD0,
		0xC1, 0xE2, 0x08, 0x2B, 0xD0, 0x8D, 0x84, 0x90, 0xC3, 0x9E, 0x26, 0x00, 0xA3, -1, -1, -1, -1,
	};

	bool mGetTextSection( const unsigned char** Start, unsigned int* Size )
	{
		const unsigned char* base = reinterpret_cast< const unsigned char* >( GetModuleHandleA( NULL ) );
		const IMAGE_DOS_HEADER* dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( base );
		if( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return false;
		const IMAGE_NT_HEADERS* nt = reinterpret_cast< const IMAGE_NT_HEADERS* >( base + dos->e_lfanew );
		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION( nt );
		for( int i = 0; i < nt->FileHeader.NumberOfSections; i++, section++ )
		{
			if( memcmp( section->Name, ".text", 6 ) == 0 )
			{
				*Start = base + section->VirtualAddress;
				*Size = section->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	//	Returns the single match, or NULL if there are none or more than one.
	const unsigned char* mFindUnique( const int* Pattern, int Length )
	{
		const unsigned char* text;
		unsigned int size;
		if( !mGetTextSection( &text, &size ) )
			return NULL;

		const unsigned char* found = NULL;
		for( unsigned int i = 0; i + Length <= size; i++ )
		{
			int j = 0;
			while( j < Length && ( Pattern[j] < 0 || text[i + j] == Pattern[j] ) )
				j++;
			if( j == Length )
			{
				if( found )
					return NULL;
				found = text + i;
			}
		}
		return found;
	}
}

std::string
nOracleSeed::mPin( unsigned int CrtSeed, int LcgSeed )
{
	char line[128];
	std::string result;

	const unsigned char* crt = mFindUnique( kCrtPattern, sizeof( kCrtPattern ) / sizeof( kCrtPattern[0] ) );
	if( crt )
	{
		//	srand() works on the calling thread's CRT state, which is the game thread calling the script.
		typedef void ( __cdecl *tSrand )( unsigned int );
		reinterpret_cast< tSrand >( const_cast< unsigned char* >( crt ) )( CrtSeed );
		sprintf( line, "crt_srand=0x%08X", reinterpret_cast< unsigned int >( crt ) );
	}
	else
	{
		sprintf( line, "crt_srand=missing" );
	}
	result += line;

	const unsigned char* lcg = mFindUnique( kLcgPattern, sizeof( kLcgPattern ) / sizeof( kLcgPattern[0] ) );
	int* seed = lcg ? *reinterpret_cast< int* const* >( lcg + 1 ) : NULL;
	if( seed && seed == *reinterpret_cast< int* const* >( lcg + 29 ) )
	{
		*seed = LcgSeed;
		sprintf( line, " lcg_seed=0x%08X", reinterpret_cast< unsigned int >( seed ) );
	}
	else
	{
		sprintf( line, " lcg_seed=missing" );
	}
	result += line;

	return result;
}
