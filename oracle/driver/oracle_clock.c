/*
 * oracle_clock.dll: the fixed-step clock for the original under Wine, the same step the native
 * client takes under --oracle and --record-trace (src/platform/Clock/FixedStepClock.h). Loaded
 * into the game by oracle_launch.exe before its first instruction runs.  C89 so the VC6
 * toolchain builds it.
 *
 * oracle_clock_bind(main thread id), called by the launcher in the suspended game, replaces these
 * entries of the exe's import table (no other module's):
 *   KERNEL32 GetTickCount, QueryPerformanceCounter, QueryPerformanceFrequency, Sleep
 *   WINMM    timeGetTime, timeSetEvent, timeKillEvent
 *   USER32   PeekMessageA
 * and returns how many it replaced.
 *
 * The clock is one count of 100 ns units from 0. GetTickCount and timeGetTime give it in
 * milliseconds (wrapping at 2^32), QueryPerformanceCounter as it is, QueryPerformanceFrequency
 * 10 MHz. It grows only:
 *   - by the step (SFCOP_FIXED_STEP_US microseconds, set by the launcher; 16667 by default) each
 *     time PeekMessageA, on the main thread, finds no message: the game then runs a frame;
 *   - by the milliseconds asked on Sleep from the main thread (which still sleeps as asked);
 *   - by one millisecond when the main thread has read the clock 10,000 times since it last
 *     grew, so that a loop waiting for time to pass ends.
 * Sleep on any other thread sleeps and leaves the count alone.
 *
 * The multimedia timers start no thread: as the native client's (src/hle/kernel32rt/Timers.h)
 * they fall due by this clock and fire on the main thread, in PeekMessageA, right after the
 * frame's step: every timer due then, the earliest due time first and a tie by the lower id. A
 * periodic timer's next due time follows its last, and after a stall of more than a period one
 * catch-up tick fires at once. TIME_CALLBACK_FUNCTION calls the procedure (id, 0, user, 0, 0);
 * TIME_CALLBACK_EVENT_SET and _PULSE set or pulse the event the procedure argument names.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#define UNITS_PER_MS 10000
#define FREQUENCY 10000000
#define SPIN_READS 10000
#define DEFAULT_STEP_US 16667
#define MAX_TIMERS 32
#define CALLBACK_EVENT_SET 0x10
#define CALLBACK_EVENT_PULSE 0x20

typedef BOOL( WINAPI* PeekFn )( LPMSG, HWND, UINT, UINT, UINT );
typedef VOID( WINAPI* SleepFn )( DWORD );

static CRITICAL_SECTION gLock;
static unsigned __int64 gUnits = 0;
static unsigned __int64 gStep = (unsigned __int64)DEFAULT_STEP_US * 10;
static DWORD gMainThread = 0;
static unsigned long gReadsSinceGrowth = 0;
static PeekFn gPeek = NULL;
static SleepFn gSleep = NULL;

typedef void( CALLBACK* TimerFn )( UINT, UINT, DWORD, DWORD, DWORD );
struct Timer
{
	UINT id; /* 0: the slot is free */
	DWORD due, period;
	BOOL periodic;
	UINT flags;
	DWORD procedure, user;
};
static struct Timer gTimers[MAX_TIMERS];
static UINT gNextTimer = 1;
static CRITICAL_SECTION gFiring; /* held while a callback runs; timeKillEvent waits on it */

static void mGrow( unsigned __int64 units )
{
	EnterCriticalSection( &gLock );
	gUnits += units;
	if( GetCurrentThreadId() == gMainThread )
		gReadsSinceGrowth = 0;
	LeaveCriticalSection( &gLock );
}

static unsigned __int64 mRead( void )
{
	unsigned __int64 units;
	EnterCriticalSection( &gLock );
	if( GetCurrentThreadId() == gMainThread && ++gReadsSinceGrowth >= SPIN_READS )
	{
		gReadsSinceGrowth = 0;
		gUnits += UNITS_PER_MS;
	}
	units = gUnits;
	LeaveCriticalSection( &gLock );
	return units;
}

static DWORD WINAPI hGetTickCount( void )
{
	return (DWORD)( mRead() / UNITS_PER_MS );
}

static DWORD WINAPI hTimeGetTime( void )
{
	return (DWORD)( mRead() / UNITS_PER_MS );
}

static BOOL WINAPI hQueryPerformanceCounter( LARGE_INTEGER* count )
{
	count->QuadPart = (LONGLONG)mRead();
	return TRUE;
}

static BOOL WINAPI hQueryPerformanceFrequency( LARGE_INTEGER* frequency )
{
	frequency->QuadPart = FREQUENCY;
	return TRUE;
}

static VOID WINAPI hSleep( DWORD milliseconds )
{
	if( milliseconds != 0 && GetCurrentThreadId() == gMainThread )
		mGrow( (unsigned __int64)milliseconds * UNITS_PER_MS );
	gSleep( milliseconds );
}

static MMRESULT WINAPI hTimeSetEvent( UINT delay, UINT resolution, LPTIMECALLBACK procedure, DWORD user, UINT flags )
{
	MMRESULT id = 0;
	DWORD now;
	int i;
	if( procedure == NULL )
		return 0;
	now = (DWORD)( mRead() / UNITS_PER_MS );
	EnterCriticalSection( &gLock );
	for( i = 0; i < MAX_TIMERS; ++i )
	{
		if( gTimers[i].id != 0 )
			continue;
		id = gNextTimer++;
		if( gNextTimer == 0 )
			gNextTimer = 1;
		gTimers[i].id = id;
		gTimers[i].due = now + delay;
		gTimers[i].period = delay == 0 ? 1 : delay;
		gTimers[i].periodic = ( flags & TIME_PERIODIC ) != 0;
		gTimers[i].flags = flags;
		gTimers[i].procedure = (DWORD)procedure;
		gTimers[i].user = user;
		break;
	}
	LeaveCriticalSection( &gLock );
	return id;
}

static MMRESULT WINAPI hTimeKillEvent( UINT id )
{
	int i, found = 0;
	EnterCriticalSection( &gLock );
	for( i = 0; i < MAX_TIMERS; ++i )
		if( id != 0 && gTimers[i].id == id )
		{
			gTimers[i].id = 0;
			found = 1;
		}
	LeaveCriticalSection( &gLock );
	if( !found )
		return MMSYSERR_INVALPARAM;
	/* A callback running now ends first (from that callback itself the section is its own). */
	EnterCriticalSection( &gFiring );
	LeaveCriticalSection( &gFiring );
	return TIMERR_NOERROR;
}

/* Every timer due by the clock, once; one reading for the whole call. On the main thread. */
static void mFireDue( void )
{
	DWORD now = (DWORD)( mRead() / UNITS_PER_MS );
	EnterCriticalSection( &gFiring );
	for( ;; )
	{
		struct Timer fire;
		int i, next = -1;
		EnterCriticalSection( &gLock );
		for( i = 0; i < MAX_TIMERS; ++i )
		{
			if( gTimers[i].id == 0 || (LONG)( now - gTimers[i].due ) < 0 )
				continue;
			if( next < 0 || (LONG)( gTimers[i].due - gTimers[next].due ) < 0 ||
				( gTimers[i].due == gTimers[next].due && gTimers[i].id < gTimers[next].id ) )
				next = i;
		}
		if( next < 0 )
		{
			LeaveCriticalSection( &gLock );
			break;
		}
		fire = gTimers[next];
		if( gTimers[next].periodic )
		{
			gTimers[next].due += gTimers[next].period;
			if( (LONG)( now - gTimers[next].due ) >= (LONG)gTimers[next].period )
				gTimers[next].due = now;
		}
		else
			gTimers[next].id = 0;
		LeaveCriticalSection( &gLock );
		if( fire.flags & ( CALLBACK_EVENT_SET | CALLBACK_EVENT_PULSE ) )
		{
			if( fire.flags & CALLBACK_EVENT_PULSE )
				PulseEvent( (HANDLE)fire.procedure );
			else
				SetEvent( (HANDLE)fire.procedure );
		}
		else
			( (TimerFn)fire.procedure )( fire.id, 0, fire.user, 0, 0 );
	}
	LeaveCriticalSection( &gFiring );
}

static BOOL WINAPI hPeekMessageA( LPMSG msg, HWND hwnd, UINT first, UINT last, UINT remove )
{
	BOOL found = gPeek( msg, hwnd, first, last, remove );
	if( !found && GetCurrentThreadId() == gMainThread )
	{
		mGrow( gStep );
		mFireDue();
	}
	return found;
}

/* Replaces `name` of `dll` in `module`'s import table with `hook`; the replaced entry's old value
 * in *original when it is still empty. Returns how many entries it replaced. */
static int mPatch( HMODULE module, const char* dll, const char* name, void* hook, void** original )
{
	BYTE* base = (BYTE*)module;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)( base + dos->e_lfanew );
	IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	IMAGE_IMPORT_DESCRIPTOR* desc;
	FARPROC real = GetProcAddress( LoadLibraryA( dll ), name );
	int patched = 0;
	if( dir->VirtualAddress == 0 )
		return 0;
	for( desc = (IMAGE_IMPORT_DESCRIPTOR*)( base + dir->VirtualAddress ); desc->Name != 0; ++desc )
	{
		/* Both thunk arrays as plain 32-bit words: VC6's headers type their fields as pointers. */
		DWORD* slot;
		DWORD* byName;
		if( lstrcmpiA( (const char*)( base + desc->Name ), dll ) != 0 )
			continue;
		slot = (DWORD*)( base + desc->FirstThunk );
		byName = desc->OriginalFirstThunk != 0 ? (DWORD*)( base + (DWORD)desc->OriginalFirstThunk ) : NULL;
		for( ; *slot != 0; ++slot, byName = byName ? byName + 1 : NULL )
		{
			BOOL match;
			DWORD protect;
			if( byName != NULL && !( *byName & IMAGE_ORDINAL_FLAG ) )
				match = strcmp( (const char*)( base + *byName + 2 ), name ) == 0; /* past the hint */
			else
				match = real != NULL && (FARPROC)*slot == real;
			if( !match )
				continue;
			if( *original == NULL )
				*original = (void*)*slot;
			VirtualProtect( slot, sizeof( *slot ), PAGE_READWRITE, &protect );
			*slot = (DWORD)hook;
			VirtualProtect( slot, sizeof( *slot ), protect, &protect );
			++patched;
		}
	}
	return patched;
}

/* The launcher's call, as a thread procedure: the parameter is the game's main thread id. */
__declspec( dllexport ) DWORD WINAPI oracle_clock_bind( LPVOID mainThread )
{
	HMODULE exe = GetModuleHandleA( NULL );
	char text[16];
	void* unused = NULL;
	int patched = 0;
	if( GetEnvironmentVariableA( "SFCOP_FIXED_STEP_US", text, sizeof( text ) ) > 0 )
	{
		long micros = atol( text );
		if( micros > 0 && micros <= 1000000 )
			gStep = (unsigned __int64)micros * 10;
	}
	gMainThread = (DWORD)mainThread;
	gSleep = (SleepFn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "Sleep" );
	gPeek = (PeekFn)GetProcAddress( LoadLibraryA( "user32.dll" ), "PeekMessageA" );
	patched += mPatch( exe, "KERNEL32.dll", "GetTickCount", (void*)hGetTickCount, &unused );
	unused = NULL;
	patched += mPatch( exe, "KERNEL32.dll", "QueryPerformanceCounter", (void*)hQueryPerformanceCounter, &unused );
	unused = NULL;
	patched += mPatch( exe, "KERNEL32.dll", "QueryPerformanceFrequency", (void*)hQueryPerformanceFrequency, &unused );
	patched += mPatch( exe, "KERNEL32.dll", "Sleep", (void*)hSleep, (void**)&gSleep );
	unused = NULL;
	patched += mPatch( exe, "WINMM.dll", "timeGetTime", (void*)hTimeGetTime, &unused );
	unused = NULL;
	patched += mPatch( exe, "WINMM.dll", "timeSetEvent", (void*)hTimeSetEvent, &unused );
	unused = NULL;
	patched += mPatch( exe, "WINMM.dll", "timeKillEvent", (void*)hTimeKillEvent, &unused );
	patched += mPatch( exe, "USER32.dll", "PeekMessageA", (void*)hPeekMessageA, (void**)&gPeek );
	return (DWORD)patched;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, LPVOID reserved )
{
	if( reason == DLL_PROCESS_ATTACH )
	{
		InitializeCriticalSection( &gLock );
		InitializeCriticalSection( &gFiring );
		DisableThreadLibraryCalls( instance );
	}
	return TRUE;
}
