/*
 * oracle_clock_check.exe: reads the clock the way the game does, for oracle_clock.dll's test
 * (test_clock.sh). Run through oracle_launch.exe; each line is a reading the test compares with
 * values written by hand. Without the launcher the readings are the wall clock's and the test
 * fails.  C89 so the VC6 toolchain builds it.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

static DWORD WINAPI mOther( LPVOID unused )
{
	Sleep( 50 ); /* another thread: the count does not move */
	return 0;
}

static void CALLBACK mTimer( UINT id, UINT unused, DWORD user, DWORD unused1, DWORD unused2 )
{
	printf( "timer id=%u user=%lu tick=%lu\n", id, user, GetTickCount() );
}

static void mShow( const char* what )
{
	LARGE_INTEGER count;
	QueryPerformanceCounter( &count );
	printf( "%s tick=%lu time=%lu counter=%lu\n", what, GetTickCount(), timeGetTime(),
		(unsigned long)count.QuadPart );
}

int main( void )
{
	LARGE_INTEGER frequency;
	MSG msg;
	HANDLE other;
	DWORD start;
	unsigned long spins = 0;
	MMRESULT timer, killed;
	int frame;

	QueryPerformanceFrequency( &frequency );
	printf( "frequency=%lu\n", (unsigned long)frequency.QuadPart );
	mShow( "start" );
	Sleep( 5 );
	mShow( "sleep5" );
	PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ); /* nothing queued: a frame */
	mShow( "frame1" );
	PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE );
	mShow( "frame2" );
	PostThreadMessageA( GetCurrentThreadId(), WM_USER, 0, 0 );
	PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE ); /* a message: not a frame */
	mShow( "message" );
	other = CreateThread( NULL, 0, mOther, NULL, 0, NULL );
	WaitForSingleObject( other, INFINITE );
	mShow( "other" );
	start = GetTickCount();
	while( GetTickCount() == start ) /* waits for time to pass with neither frame nor sleep */
		++spins;
	printf( "spin reads=%lu\n", spins + 1 );
	mShow( "spun" );
	/* A periodic 25 ms timer: it fires in the frames' PeekMessageA once the clock passes it. */
	timer = timeSetEvent( 25, 0, mTimer, 7, TIME_PERIODIC );
	for( frame = 0; frame < 4; ++frame )
		PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE );
	killed = timeKillEvent( timer );
	printf( "kill=%u again=%u\n", killed, timeKillEvent( timer ) );
	PeekMessageA( &msg, NULL, 0, 0, PM_REMOVE );
	mShow( "after" );
	return 0;
}
