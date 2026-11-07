/*
 * oracle_launch.exe: starts a program with oracle_clock.dll's fixed-step clock in it, so that the
 * original under Wine reads the same times per frame as the native client under --oracle
 * (README.md, "The fixed step under Wine").  C89 so the VC6 toolchain builds it.
 *
 *   oracle_launch.exe [--fixed-step MS] <exe> [arguments...]
 *
 * MS is milliseconds per frame with at most three decimals, above 0 and up to 1000; 16.667 by
 * default. oracle_clock.dll is taken from the launcher's own folder. The program is created
 * suspended, the DLL loaded into it, oracle_clock_bind called with its main thread, then it runs;
 * the launcher waits for it and exits with its exit code. One line goes to stdout before it runs:
 *   oracle_launch: fixed step 16.667 ms, 8 imports replaced
 * Exit 2 for a bad command line, 3 when the program or the DLL cannot be started or bound.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

/* "16.667" as 16667; 0 when malformed or out of range. */
static long mMicroseconds( const char* text )
{
	long whole = 0, fraction = 0;
	int digits = 0, fractionDigits = 0, point = 0;
	for( ; *text; ++text )
	{
		if( *text == '.' && !point )
		{
			point = 1;
			continue;
		}
		if( *text < '0' || *text > '9' )
			return 0;
		digits = 1;
		if( point )
		{
			if( ++fractionDigits > 3 )
				return 0;
			fraction = fraction * 10 + ( *text - '0' );
		}
		else
		{
			whole = whole * 10 + ( *text - '0' );
			if( whole > 1000 )
				return 0;
		}
	}
	while( fractionDigits++ < 3 )
		fraction *= 10;
	if( !digits || whole * 1000 + fraction > 1000000 )
		return 0;
	return whole * 1000 + fraction;
}

/* Appends `arg` to `line`, quoted when it holds a space. */
static void mAppend( char* line, size_t size, const char* arg )
{
	int quote = strchr( arg, ' ' ) != NULL || *arg == '\0';
	if( line[0] != '\0' )
		strncat( line, " ", size - strlen( line ) - 1 );
	if( quote )
		strncat( line, "\"", size - strlen( line ) - 1 );
	strncat( line, arg, size - strlen( line ) - 1 );
	if( quote )
		strncat( line, "\"", size - strlen( line ) - 1 );
}

static DWORD mRemoteCall( HANDLE process, LPTHREAD_START_ROUTINE start, LPVOID parameter )
{
	DWORD result = 0;
	HANDLE thread = CreateRemoteThread( process, NULL, 0, start, parameter, 0, NULL );
	if( thread == NULL )
		return 0;
	WaitForSingleObject( thread, INFINITE );
	GetExitCodeThread( thread, &result );
	CloseHandle( thread );
	return result;
}

int main( int argc, char** argv )
{
	char dll[MAX_PATH], line[4096], text[16];
	char* slash;
	long micros = 16667;
	int first = 1, i;
	STARTUPINFOA startup;
	PROCESS_INFORMATION info;
	void* remotePath;
	HMODULE local;
	FARPROC bind;
	DWORD remoteBase, patched, code = 0;

	if( argc > 2 && strcmp( argv[1], "--fixed-step" ) == 0 )
	{
		micros = mMicroseconds( argv[2] );
		first = 3;
	}
	if( micros == 0 || first >= argc )
	{
		fprintf( stderr, "usage: oracle_launch.exe [--fixed-step MS] <exe> [arguments...]\n" );
		return 2;
	}

	GetModuleFileNameA( NULL, dll, sizeof( dll ) );
	slash = strrchr( dll, '\\' );
	strcpy( slash != NULL ? slash + 1 : dll, "oracle_clock.dll" );
	sprintf( text, "%ld", micros );
	SetEnvironmentVariableA( "SFCOP_FIXED_STEP_US", text ); /* the program inherits it */

	line[0] = '\0';
	for( i = first; i < argc; ++i )
		mAppend( line, sizeof( line ), argv[i] );
	memset( &startup, 0, sizeof( startup ) );
	startup.cb = sizeof( startup );
	if( !CreateProcessA( NULL, line, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &info ) )
	{
		fprintf( stderr, "oracle_launch: cannot start %s (error %lu)\n", argv[first], GetLastError() );
		return 3;
	}

	/* LoadLibraryA(dll) in the program: kernel32 is at the same address in every process. */
	remotePath = VirtualAllocEx( info.hProcess, NULL, sizeof( dll ), MEM_COMMIT, PAGE_READWRITE );
	local = LoadLibraryExA( dll, NULL, DONT_RESOLVE_DLL_REFERENCES );
	bind = local != NULL ? GetProcAddress( local, "_oracle_clock_bind@4" ) : NULL;
	if( bind == NULL && local != NULL )
		bind = GetProcAddress( local, "oracle_clock_bind" );
	remoteBase = 0;
	if( remotePath != NULL && bind != NULL &&
		WriteProcessMemory( info.hProcess, remotePath, dll, sizeof( dll ), NULL ) )
	{
		remoteBase = mRemoteCall( info.hProcess,
			(LPTHREAD_START_ROUTINE)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "LoadLibraryA" ),
			remotePath );
	}
	if( remoteBase == 0 )
	{
		fprintf( stderr, "oracle_launch: cannot load %s into the program\n", dll );
		TerminateProcess( info.hProcess, 3 );
		return 3;
	}
	/* The same entry in the program's copy of the DLL, by its offset from the DLL's base. */
	patched = mRemoteCall( info.hProcess,
		(LPTHREAD_START_ROUTINE)( remoteBase + ( (DWORD)bind - (DWORD)local ) ),
		(LPVOID)info.dwThreadId );
	printf( "oracle_launch: fixed step %ld.%03ld ms, %lu imports replaced\n", micros / 1000,
		micros % 1000, patched );
	fflush( stdout );
	if( patched == 0 )
	{
		fprintf( stderr, "oracle_launch: the program imports none of the clock's entries\n" );
		TerminateProcess( info.hProcess, 3 );
		return 3;
	}

	ResumeThread( info.hThread );
	WaitForSingleObject( info.hProcess, INFINITE );
	GetExitCodeProcess( info.hProcess, &code );
	return (int)code;
}
