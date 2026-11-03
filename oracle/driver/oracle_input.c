/*
 * oracle_input.exe: replays a fixed menu path into the game so the oracle needs no human.
 * Runs inside the same Wine prefix as the game.  C89 so the VC6 toolchain builds it.
 *
 *   oracle_input.exe <steps-file>
 *
 * Steps, one per line ('#' starts a comment):
 *   window <timeout_s> <title substring>   wait for a visible top-level window, make it current
 *   sleep <ms>
 *   click <x> <y>                          left click at client coordinates of the current window
 *   key <vk>                               press and release a virtual key (decimal or 0x hex)
 *   shot <file.bmp>                        GDI capture of the current window's client area
 *   waitfile <timeout_s> <path>            wait until a file exists
 *   list                                   print every visible top-level window (title, rect)
 *
 * Every step is echoed to stdout with a millisecond timestamp; the exit code is the number of the
 * first failing line, or 0.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HWND gWindow = NULL;
static const char* gWanted = NULL;

static BOOL CALLBACK mFindWindow( HWND hwnd, LPARAM found )
{
	char title[256];
	if( !IsWindowVisible( hwnd ) )
		return TRUE;
	if( GetWindowTextA( hwnd, title, sizeof( title ) ) > 0 && strstr( title, gWanted ) )
	{
		*(HWND*)found = hwnd;
		return FALSE;
	}
	return TRUE;
}

/* Lists hidden windows too (marked '-'): a title-less or not-yet-shown game window still appears. */
static BOOL CALLBACK mPrintWindow( HWND hwnd, LPARAM unused )
{
	char title[256], name[128];
	RECT rect;
	DWORD process = 0;
	title[0] = name[0] = '\0';
	GetWindowTextA( hwnd, title, sizeof( title ) );
	GetClassNameA( hwnd, name, sizeof( name ) );
	GetWindowThreadProcessId( hwnd, &process );
	GetWindowRect( hwnd, &rect );
	printf( "         window %08lX %c pid %lu class \"%s\" \"%s\" %ld,%ld %ldx%ld\n", (unsigned long)hwnd,
			IsWindowVisible( hwnd ) ? '+' : '-', process, name, title,
			rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top );
	return TRUE;
}

static int mListWindows( void )
{
	EnumWindows( mPrintWindow, 0 );
	fflush( stdout );
	return 1;
}

static int mWaitWindow( int timeout, const char* title )
{
	DWORD end = GetTickCount() + timeout * 1000;
	gWanted = title;
	do
	{
		HWND found = NULL;
		EnumWindows( mFindWindow, (LPARAM)&found );
		if( found )
		{
			gWindow = found;
			SetForegroundWindow( gWindow );
			return 1;
		}
		Sleep( 250 );
	} while( GetTickCount() < end );
	return 0;
}

/* mouse_event/keybd_event rather than SendInput: the VC6 headers predate INPUT. */
static void mSendMouse( DWORD flags )
{
	mouse_event( flags, 0, 0, 0, 0 );
}

static int mClick( int x, int y )
{
	POINT point;
	if( gWindow == NULL )
		return 0;
	point.x = x;
	point.y = y;
	ClientToScreen( gWindow, &point );
	SetForegroundWindow( gWindow );
	SetCursorPos( point.x, point.y );
	Sleep( 50 );
	mSendMouse( MOUSEEVENTF_LEFTDOWN );
	Sleep( 80 );
	mSendMouse( MOUSEEVENTF_LEFTUP );
	return 1;
}

static int mKey( WORD vk )
{
	BYTE scan = (BYTE)MapVirtualKey( vk, 0 );
	SetForegroundWindow( gWindow );
	keybd_event( (BYTE)vk, scan, 0, 0 );
	Sleep( 50 );
	keybd_event( (BYTE)vk, scan, KEYEVENTF_KEYUP, 0 );
	return 1;
}

static int mShot( const char* path )
{
	RECT rect;
	HDC window, memory;
	HBITMAP bitmap;
	BITMAPINFOHEADER info;
	BITMAPFILEHEADER header;
	int width, height, stride, ok = 0;
	char* pixels;
	FILE* file;

	if( gWindow == NULL || !GetClientRect( gWindow, &rect ) )
		return 0;
	width = rect.right;
	height = rect.bottom;
	stride = ( width * 3 + 3 ) & ~3;

	window = GetDC( gWindow );
	memory = CreateCompatibleDC( window );
	bitmap = CreateCompatibleBitmap( window, width, height );
	SelectObject( memory, bitmap );
	BitBlt( memory, 0, 0, width, height, window, 0, 0, SRCCOPY );

	memset( &info, 0, sizeof( info ) );
	info.biSize = sizeof( info );
	info.biWidth = width;
	info.biHeight = height;
	info.biPlanes = 1;
	info.biBitCount = 24;
	info.biCompression = BI_RGB;
	pixels = (char*)malloc( stride * height );
	if( pixels && GetDIBits( memory, bitmap, 0, height, pixels, (BITMAPINFO*)&info, DIB_RGB_COLORS ) )
	{
		file = fopen( path, "wb" );
		if( file )
		{
			memset( &header, 0, sizeof( header ) );
			header.bfType = 0x4D42;
			header.bfOffBits = sizeof( header ) + sizeof( info );
			header.bfSize = header.bfOffBits + stride * height;
			fwrite( &header, sizeof( header ), 1, file );
			fwrite( &info, sizeof( info ), 1, file );
			fwrite( pixels, stride * height, 1, file );
			fclose( file );
			ok = 1;
		}
	}
	free( pixels );
	DeleteObject( bitmap );
	DeleteDC( memory );
	ReleaseDC( gWindow, window );
	return ok;
}

static int mWaitFile( int timeout, const char* path )
{
	DWORD end = GetTickCount() + timeout * 1000;
	do
	{
		if( GetFileAttributesA( path ) != 0xFFFFFFFF )
			return 1;
		Sleep( 500 );
	} while( GetTickCount() < end );
	return 0;
}

/* Returns the rest of the line after skipping n whitespace-separated words. */
static char* mRest( char* line, int words )
{
	while( words-- > 0 )
	{
		while( *line == ' ' || *line == '\t' )
			line++;
		while( *line && *line != ' ' && *line != '\t' )
			line++;
	}
	while( *line == ' ' || *line == '\t' )
		line++;
	return line;
}

/* Wine can put a console program started before the game's desktop exists on a desktop of its own,
   where EnumWindows never sees the game.  The game's virtual desktop is "Default": join it. */
static void mJoinDesktop( void )
{
	char name[128];
	HDESK desktop = OpenDesktopA( "Default", 0, FALSE, GENERIC_ALL );
	if( desktop == NULL || !SetThreadDesktop( desktop ) )
		printf( "         desktop: cannot join Default (error %lu)\n", GetLastError() );
	name[0] = '\0';
	GetUserObjectInformationA( GetThreadDesktop( GetCurrentThreadId() ), UOI_NAME, name, sizeof( name ), NULL );
	printf( "         desktop \"%s\"\n", name );
	fflush( stdout );
}

int main( int argc, char** argv )
{
	char line[512], verb[32];
	FILE* steps;
	int number = 0;
	DWORD start = GetTickCount();

	if( argc != 2 )
	{
		fprintf( stderr, "usage: oracle_input <steps-file>\n" );
		return 255;
	}
	steps = fopen( argv[1], "r" );
	if( steps == NULL )
	{
		fprintf( stderr, "oracle_input: cannot open %s\n", argv[1] );
		return 255;
	}
	mJoinDesktop();

	while( fgets( line, sizeof( line ), steps ) )
	{
		int ok = 1, a = 0, b = 0;
		char* end = line + strlen( line );
		number++;
		while( end > line && ( end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ) )
			*--end = '\0';
		if( line[0] == '#' || sscanf( line, "%31s", verb ) != 1 )
			continue;

		printf( "%8lu %3d %s\n", GetTickCount() - start, number, line );
		fflush( stdout );

		if( strcmp( verb, "window" ) == 0 && sscanf( line, "%*s %d", &a ) == 1 )
			ok = mWaitWindow( a, mRest( line, 2 ) );
		else if( strcmp( verb, "sleep" ) == 0 && sscanf( line, "%*s %d", &a ) == 1 )
			Sleep( a );
		else if( strcmp( verb, "click" ) == 0 && sscanf( line, "%*s %d %d", &a, &b ) == 2 )
			ok = mClick( a, b );
		else if( strcmp( verb, "key" ) == 0 && sscanf( line, "%*s %i", &a ) == 1 )
			ok = mKey( (WORD)a );
		else if( strcmp( verb, "shot" ) == 0 )
			ok = mShot( mRest( line, 1 ) );
		else if( strcmp( verb, "list" ) == 0 )
			ok = mListWindows();
		else if( strcmp( verb, "waitfile" ) == 0 && sscanf( line, "%*s %d", &a ) == 1 )
			ok = mWaitFile( a, mRest( line, 2 ) );
		else
			ok = 0;

		if( !ok )
		{
			printf( "%8lu %3d FAILED\n", GetTickCount() - start, number );
			fclose( steps );
			return number;
		}
	}
	fclose( steps );
	printf( "%8lu done\n", GetTickCount() - start );
	return 0;
}
