#!/usr/bin/env python3
"""Generate src/hle/surface.tsv: every Windows and DirectX entry point the game calls.

    python3 src/hle/gen_surface.py               rewrite surface.tsv
    python3 src/hle/gen_surface.py --check       exit 1 if surface.tsv is not what it would write
    python3 src/hle/gen_surface.py --libs DIR    also check the signature table against the
                                                 Win32 import libraries in DIR (KERNEL32.LIB ...)

Inputs:

* the import table of the original exe (located as the harness does, hash-checked against
  analysis/results/retail_hashes.sha256), and a byte scan of its code for the instructions that
  reach each import slot, for the evidence column;
* SIGNATURES below: the calling convention and stack purge of every import, taken from the
  decorated names (`_CreateFileA@28`) in the Visual C++ 6 (VC98) Win32 import libraries, and
  for MSS32 from the decorated import names in the exe itself;
* the COM layouts in src/hle/com/*.h, one method per line, which give each interface's slots;
* the current surface.tsv, whose status and notes columns (and the evidence of COM rows) are
  kept: they change only with an entry's implementation (src/hle/README.md).

Standard library only.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SURFACE = os.path.join(HERE, 'surface.tsv')
COM_DIR = os.path.join(HERE, 'com')
COLUMNS = ['module', 'name', 'ordinal_or_slot', 'convention', 'stack_bytes', 'status',
           'evidence', 'notes']
CONVENTIONS = {'stdcall', 'cdecl', 'thiscall', 'varargs'}
STATUSES = {'implemented', 'stub', 'implemented-spike', 'stub-spike', 'unused', 'todo'}

# module, [#ordinal] name, stack bytes purged by the callee (or "varargs": cdecl, caller pops).
# From the Visual C++ 6 (VC98) import libraries; DBGHELP's functions from IMAGEHLP.LIB, which
# exports the same Sym*/StackWalk entry points. MSS32 is not listed: its import names carry the size.
SIGNATURES = """
ADVAPI32.DLL DeregisterEventSource 4
ADVAPI32.DLL RegCloseKey 4
ADVAPI32.DLL RegCreateKeyExA 36
ADVAPI32.DLL RegDeleteKeyA 8
ADVAPI32.DLL RegisterEventSourceA 8
ADVAPI32.DLL RegOpenKeyExA 20
ADVAPI32.DLL RegQueryValueExA 24
ADVAPI32.DLL RegSetValueExA 24
ADVAPI32.DLL ReportEventA 36
COMDLG32.DLL CommDlgExtendedError 0
COMDLG32.DLL GetOpenFileNameA 4
COMDLG32.DLL GetOpenFileNameW 4
COMDLG32.DLL GetSaveFileNameA 4
COMDLG32.DLL GetSaveFileNameW 4
DBGHELP.DLL StackWalk 36
DBGHELP.DLL SymFunctionTableAccess 8
DBGHELP.DLL SymGetLineFromAddr 16
DBGHELP.DLL SymGetModuleInfo 12
DBGHELP.DLL SymGetOptions 0
DBGHELP.DLL SymGetSymFromAddr 16
DBGHELP.DLL SymInitialize 12
DBGHELP.DLL SymLoadModule 24
DBGHELP.DLL SymSetOptions 4
DDRAW.DLL DirectDrawCreate 12
DDRAW.DLL DirectDrawCreateClipper 12
DDRAW.DLL DirectDrawEnumerateA 8
DINPUT.DLL DirectInputCreateA 16
DSOUND.DLL #1 DirectSoundCreate 12
GDI32.DLL BitBlt 36
GDI32.DLL CreateCompatibleDC 4
GDI32.DLL CreateDIBSection 24
GDI32.DLL CreateFontA 56
GDI32.DLL DeleteDC 4
GDI32.DLL DeleteObject 4
GDI32.DLL ExtTextOutA 32
GDI32.DLL GetDeviceCaps 8
GDI32.DLL GetDIBColorTable 16
GDI32.DLL GetStockObject 4
GDI32.DLL GetSystemPaletteEntries 16
GDI32.DLL GetTextExtentPoint32A 16
GDI32.DLL GetTextMetricsA 8
GDI32.DLL PatBlt 24
GDI32.DLL SelectObject 8
GDI32.DLL SetBkColor 8
GDI32.DLL SetBkMode 8
GDI32.DLL SetDIBColorTable 16
GDI32.DLL SetTextAlign 8
GDI32.DLL SetTextColor 8
GDI32.DLL StretchBlt 44
GDI32.DLL TextOutA 20
KERNEL32.DLL CloseHandle 4
KERNEL32.DLL CompareStringA 24
KERNEL32.DLL CompareStringW 24
KERNEL32.DLL CopyFileA 12
KERNEL32.DLL CreateDirectoryA 8
KERNEL32.DLL CreateDirectoryW 8
KERNEL32.DLL CreateEventA 16
KERNEL32.DLL CreateFileA 28
KERNEL32.DLL CreateFileMappingA 24
KERNEL32.DLL CreateFileW 28
KERNEL32.DLL CreateMutexA 12
KERNEL32.DLL CreateProcessA 40
KERNEL32.DLL CreateSemaphoreA 16
KERNEL32.DLL CreateThread 24
KERNEL32.DLL DeleteCriticalSection 4
KERNEL32.DLL DeleteFileA 4
KERNEL32.DLL DeleteFileW 4
KERNEL32.DLL EnterCriticalSection 4
KERNEL32.DLL EnumSystemLocalesA 8
KERNEL32.DLL ExitProcess 4
KERNEL32.DLL ExitThread 4
KERNEL32.DLL FileTimeToLocalFileTime 8
KERNEL32.DLL FileTimeToSystemTime 8
KERNEL32.DLL FindClose 4
KERNEL32.DLL FindFirstFileA 8
KERNEL32.DLL FindFirstFileW 8
KERNEL32.DLL FindNextFileA 8
KERNEL32.DLL FindNextFileW 8
KERNEL32.DLL FlushFileBuffers 4
KERNEL32.DLL FormatMessageA 28
KERNEL32.DLL FreeEnvironmentStringsA 4
KERNEL32.DLL FreeEnvironmentStringsW 4
KERNEL32.DLL FreeLibrary 4
KERNEL32.DLL GetACP 0
KERNEL32.DLL GetCommandLineA 0
KERNEL32.DLL GetCommState 8
KERNEL32.DLL GetCPInfo 8
KERNEL32.DLL GetCurrentDirectoryA 8
KERNEL32.DLL GetCurrentDirectoryW 8
KERNEL32.DLL GetCurrentProcess 0
KERNEL32.DLL GetCurrentThread 0
KERNEL32.DLL GetCurrentThreadId 0
KERNEL32.DLL GetDiskFreeSpaceA 20
KERNEL32.DLL GetDiskFreeSpaceW 20
KERNEL32.DLL GetDriveTypeA 4
KERNEL32.DLL GetDriveTypeW 4
KERNEL32.DLL GetEnvironmentStrings 0
KERNEL32.DLL GetEnvironmentStringsW 0
KERNEL32.DLL GetEnvironmentVariableA 12
KERNEL32.DLL GetFileAttributesA 4
KERNEL32.DLL GetFileAttributesW 4
KERNEL32.DLL GetFileSize 8
KERNEL32.DLL GetFileTime 16
KERNEL32.DLL GetFileType 4
KERNEL32.DLL GetFullPathNameA 16
KERNEL32.DLL GetLastError 0
KERNEL32.DLL GetLocaleInfoA 16
KERNEL32.DLL GetLocaleInfoW 16
KERNEL32.DLL GetLocalTime 4
KERNEL32.DLL GetModuleFileNameA 12
KERNEL32.DLL GetModuleHandleA 4
KERNEL32.DLL GetOEMCP 0
KERNEL32.DLL GetPrivateProfileIntA 16
KERNEL32.DLL GetPrivateProfileStringA 24
KERNEL32.DLL GetProcAddress 8
KERNEL32.DLL GetProcessHeap 0
KERNEL32.DLL GetShortPathNameA 12
KERNEL32.DLL GetStartupInfoA 4
KERNEL32.DLL GetStdHandle 4
KERNEL32.DLL GetStringTypeA 20
KERNEL32.DLL GetStringTypeW 16
KERNEL32.DLL GetSystemDirectoryA 8
KERNEL32.DLL GetSystemDirectoryW 8
KERNEL32.DLL GetSystemInfo 4
KERNEL32.DLL GetSystemTime 4
KERNEL32.DLL GetSystemTimeAsFileTime 4
KERNEL32.DLL GetThreadContext 8
KERNEL32.DLL GetTickCount 0
KERNEL32.DLL GetTimeZoneInformation 4
KERNEL32.DLL GetUserDefaultLCID 0
KERNEL32.DLL GetVersion 0
KERNEL32.DLL GetVersionExA 4
KERNEL32.DLL GetVolumeInformationA 32
KERNEL32.DLL GetVolumeInformationW 32
KERNEL32.DLL GetWindowsDirectoryA 8
KERNEL32.DLL GetWindowsDirectoryW 8
KERNEL32.DLL GlobalAlloc 8
KERNEL32.DLL GlobalFree 4
KERNEL32.DLL GlobalHandle 4
KERNEL32.DLL GlobalLock 4
KERNEL32.DLL GlobalMemoryStatus 4
KERNEL32.DLL GlobalReAlloc 12
KERNEL32.DLL GlobalUnlock 4
KERNEL32.DLL HeapAlloc 12
KERNEL32.DLL HeapCreate 12
KERNEL32.DLL HeapDestroy 4
KERNEL32.DLL HeapFree 12
KERNEL32.DLL InitializeCriticalSection 4
KERNEL32.DLL InterlockedDecrement 4
KERNEL32.DLL InterlockedExchange 8
KERNEL32.DLL InterlockedIncrement 4
KERNEL32.DLL IsBadCodePtr 4
KERNEL32.DLL IsBadReadPtr 8
KERNEL32.DLL IsBadWritePtr 8
KERNEL32.DLL IsValidCodePage 4
KERNEL32.DLL IsValidLocale 8
KERNEL32.DLL LCMapStringA 24
KERNEL32.DLL LCMapStringW 24
KERNEL32.DLL LeaveCriticalSection 4
KERNEL32.DLL LoadLibraryA 4
KERNEL32.DLL LocalFileTimeToFileTime 8
KERNEL32.DLL LocalFree 4
KERNEL32.DLL lstrcatA 8
KERNEL32.DLL lstrlenA 4
KERNEL32.DLL MapViewOfFile 20
KERNEL32.DLL MapViewOfFileEx 24
KERNEL32.DLL MoveFileA 8
KERNEL32.DLL MulDiv 12
KERNEL32.DLL MultiByteToWideChar 24
KERNEL32.DLL OpenFileMappingA 12
KERNEL32.DLL OpenMutexA 12
KERNEL32.DLL OpenProcess 12
KERNEL32.DLL OutputDebugStringA 4
KERNEL32.DLL QueryPerformanceCounter 4
KERNEL32.DLL QueryPerformanceFrequency 4
KERNEL32.DLL RaiseException 16
KERNEL32.DLL ReadFile 20
KERNEL32.DLL ReleaseMutex 4
KERNEL32.DLL ReleaseSemaphore 12
KERNEL32.DLL RemoveDirectoryA 4
KERNEL32.DLL RemoveDirectoryW 4
KERNEL32.DLL ResetEvent 4
KERNEL32.DLL ResumeThread 4
KERNEL32.DLL RtlUnwind 16
KERNEL32.DLL SetCommState 8
KERNEL32.DLL SetCurrentDirectoryA 4
KERNEL32.DLL SetCurrentDirectoryW 4
KERNEL32.DLL SetEndOfFile 4
KERNEL32.DLL SetEnvironmentVariableA 8
KERNEL32.DLL SetErrorMode 4
KERNEL32.DLL SetEvent 4
KERNEL32.DLL SetFileAttributesA 8
KERNEL32.DLL SetFileAttributesW 8
KERNEL32.DLL SetFilePointer 16
KERNEL32.DLL SetFileTime 16
KERNEL32.DLL SetHandleCount 4
KERNEL32.DLL SetLastError 4
KERNEL32.DLL SetStdHandle 8
KERNEL32.DLL SetThreadPriority 8
KERNEL32.DLL SetUnhandledExceptionFilter 4
KERNEL32.DLL Sleep 4
KERNEL32.DLL SystemTimeToFileTime 8
KERNEL32.DLL TerminateProcess 8
KERNEL32.DLL TlsAlloc 0
KERNEL32.DLL TlsGetValue 4
KERNEL32.DLL TlsSetValue 8
KERNEL32.DLL UnhandledExceptionFilter 4
KERNEL32.DLL UnmapViewOfFile 4
KERNEL32.DLL VirtualAlloc 16
KERNEL32.DLL VirtualFree 12
KERNEL32.DLL VirtualLock 8
KERNEL32.DLL VirtualQuery 12
KERNEL32.DLL VirtualQueryEx 16
KERNEL32.DLL WaitForMultipleObjects 16
KERNEL32.DLL WaitForSingleObject 8
KERNEL32.DLL WideCharToMultiByte 32
KERNEL32.DLL WinExec 8
KERNEL32.DLL WriteFile 20
KERNEL32.DLL WritePrivateProfileStringA 16
OLE32.DLL CoCreateInstance 20
OLE32.DLL CoInitialize 4
OLE32.DLL CoUninitialize 0
USER32.DLL AdjustWindowRect 12
USER32.DLL AppendMenuA 16
USER32.DLL BeginPaint 8
USER32.DLL CallNextHookEx 16
USER32.DLL CallWindowProcA 20
USER32.DLL ChangeDisplaySettingsA 8
USER32.DLL CheckMenuItem 12
USER32.DLL ClientToScreen 8
USER32.DLL CloseClipboard 0
USER32.DLL CreateWindowExA 48
USER32.DLL DefWindowProcA 16
USER32.DLL DeleteMenu 12
USER32.DLL DestroyWindow 4
USER32.DLL DialogBoxIndirectParamA 20
USER32.DLL DispatchMessageA 4
USER32.DLL DrawMenuBar 4
USER32.DLL EmptyClipboard 0
USER32.DLL EndDialog 8
USER32.DLL EndPaint 8
USER32.DLL EnumDisplaySettingsA 12
USER32.DLL FillRect 12
USER32.DLL FindWindowA 8
USER32.DLL GetActiveWindow 0
USER32.DLL GetAsyncKeyState 4
USER32.DLL GetClientRect 8
USER32.DLL GetClipboardData 4
USER32.DLL GetCursorPos 4
USER32.DLL GetDC 4
USER32.DLL GetDesktopWindow 0
USER32.DLL GetKeyState 4
USER32.DLL GetMenu 4
USER32.DLL GetMessageA 16
USER32.DLL GetSystemMenu 8
USER32.DLL GetSystemMetrics 4
USER32.DLL GetUpdateRect 12
USER32.DLL GetWindowLongA 8
USER32.DLL GetWindowRect 8
USER32.DLL InvalidateRect 12
USER32.DLL IsIconic 4
USER32.DLL IsWindow 4
USER32.DLL LoadCursorA 8
USER32.DLL LoadMenuA 8
USER32.DLL MapVirtualKeyA 8
USER32.DLL MessageBeep 4
USER32.DLL MessageBoxA 16
USER32.DLL OffsetRect 12
USER32.DLL OpenClipboard 4
USER32.DLL PeekMessageA 20
USER32.DLL PostMessageA 16
USER32.DLL PostQuitMessage 4
USER32.DLL RedrawWindow 16
USER32.DLL RegisterClassA 4
USER32.DLL ReleaseDC 8
USER32.DLL ScreenToClient 8
USER32.DLL SendMessageA 16
USER32.DLL SetActiveWindow 4
USER32.DLL SetClipboardData 8
USER32.DLL SetCursor 4
USER32.DLL SetFocus 4
USER32.DLL SetForegroundWindow 4
USER32.DLL SetMenu 8
USER32.DLL SetRect 20
USER32.DLL SetWindowLongA 12
USER32.DLL SetWindowPos 28
USER32.DLL SetWindowsHookExA 16
USER32.DLL SetWindowTextA 8
USER32.DLL ShowCursor 4
USER32.DLL ShowWindow 8
USER32.DLL SystemParametersInfoA 16
USER32.DLL TranslateMessage 4
USER32.DLL UnhookWindowsHookEx 4
USER32.DLL UpdateWindow 4
USER32.DLL VkKeyScanA 4
USER32.DLL wsprintfA varargs
WINMM.DLL timeGetTime 0
WINMM.DLL timeKillEvent 4
WINMM.DLL timeSetEvent 20
WSOCK32.DLL #151 __WSAFDIsSet 8
WSOCK32.DLL #1 accept 12
WSOCK32.DLL #2 bind 12
WSOCK32.DLL #3 closesocket 4
WSOCK32.DLL #4 connect 12
WSOCK32.DLL #52 gethostbyname 4
WSOCK32.DLL #57 gethostname 8
WSOCK32.DLL #5 getpeername 12
WSOCK32.DLL #6 getsockname 12
WSOCK32.DLL #7 getsockopt 20
WSOCK32.DLL #8 htonl 4
WSOCK32.DLL #9 htons 4
WSOCK32.DLL #10 inet_addr 4
WSOCK32.DLL #11 inet_ntoa 4
WSOCK32.DLL #12 ioctlsocket 12
WSOCK32.DLL #13 listen 8
WSOCK32.DLL #15 ntohs 4
WSOCK32.DLL #16 recv 16
WSOCK32.DLL #17 recvfrom 24
WSOCK32.DLL #18 select 20
WSOCK32.DLL #19 send 16
WSOCK32.DLL #20 sendto 24
WSOCK32.DLL #21 setsockopt 20
WSOCK32.DLL #22 shutdown 8
WSOCK32.DLL #23 socket 12
WSOCK32.DLL #116 WSACleanup 0
WSOCK32.DLL #111 WSAGetLastError 0
WSOCK32.DLL #115 WSAStartup 8
"""


def parse_signatures(text=SIGNATURES):
    """{(MODULE, name): (ordinal or None, convention, stack_bytes)}."""
    table = {}
    for line in text.strip().splitlines():
        parts = line.split()
        module, rest = parts[0], parts[1:]
        ordinal = None
        if rest[0].startswith('#'):
            ordinal = int(rest[0][1:])
            rest = rest[1:]
        name, size = rest
        if size == 'varargs':
            table[(module, name)] = (ordinal, 'varargs', 0)
        else:
            table[(module, name)] = (ordinal, 'stdcall', int(size))
    return table


# ---------------------------------------------------------------- the exe

class Pe:
    """The few parts of a PE32 image this script reads."""

    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE image')
        nsec, = struct.unpack_from('<H', data, pe + 6)
        optsz, = struct.unpack_from('<H', data, pe + 20)
        opt = pe + 24
        if struct.unpack_from('<H', data, opt)[0] != 0x10B:
            raise ValueError('not a PE32 image')
        self.base, = struct.unpack_from('<I', data, opt + 28)
        ndirs, = struct.unpack_from('<I', data, opt + 92)
        self.dirs = [struct.unpack_from('<II', data, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = data[o:o + 8].rstrip(b'\0').decode('ascii')
            vsize, va, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
            self.sections.append((name, va, vsize, raw, rsize))

    def offset(self, rva):
        for _, va, vsize, raw, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return raw + rva - va
        raise ValueError('rva 0x%X outside every section' % rva)

    def cstr(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b'\0', o)].decode('ascii')

    def section(self, name):
        for sname, va, vsize, raw, rsize in self.sections:
            if sname == name:
                return self.base + va, self.data[raw:raw + rsize]
        raise ValueError('no section ' + name)

    def imports(self):
        """[(MODULE, name or None, ordinal or None, iat_va)] in import-table order."""
        out = []
        o = self.offset(self.dirs[1][0])
        while True:
            lookup, _, _, name_rva, iat = struct.unpack_from('<IIIII', self.data, o)
            if not (lookup or name_rva or iat):
                return out
            module = self.cstr(name_rva).upper()
            table = lookup or iat
            i = 0
            while True:
                entry, = struct.unpack_from('<I', self.data, self.offset(table) + 4 * i)
                if not entry:
                    break
                slot = self.base + iat + 4 * i
                if entry & 0x80000000:
                    out.append((module, None, entry & 0xFFFF, slot))
                else:
                    out.append((module, self.cstr(entry + 2), None, slot))
                i += 1
            o += 20


def code_references(pe, slots):
    """{iat_va: [(kind, va)]}: every instruction in .text that names an import slot.

    kind is 'call' (call [slot]), 'jmp' (jmp [slot]: a thunk or a tail call), 'load' (mov reg,
    [slot], then a call through the register) or 'other'. The exe has no relocation table, so
    this is a byte scan; an import slot address is in .rdata and only appears as an operand.
    """
    text_va, text = pe.section('.text')
    refs = {s: [] for s in slots}
    for slot in slots:
        needle = struct.pack('<I', slot)
        p = text.find(needle)
        while p >= 0:
            if p >= 2 and text[p - 2] == 0xFF and text[p - 1] in (0x15, 0x25):
                refs[slot].append(('call' if text[p - 1] == 0x15 else 'jmp', text_va + p - 2))
            elif p >= 2 and text[p - 2] == 0x8B and (text[p - 1] & 0xC7) == 0x05:
                refs[slot].append(('load', text_va + p - 2))
            elif p >= 1 and text[p - 1] == 0xA1:
                refs[slot].append(('load', text_va + p - 1))
            else:
                refs[slot].append(('other', text_va + p))
            p = text.find(needle, p + 1)
    return refs


def thunk_callers(pe, addresses):
    """{address: [va of each `call rel32` to it]} for the given jump-thunk addresses."""
    text_va, text = pe.section('.text')
    wanted = set(addresses)
    out = {a: [] for a in addresses}
    for p in range(len(text) - 4):
        if text[p] == 0xE8:
            target = (text_va + p + 5 + struct.unpack_from('<i', text, p + 1)[0]) & 0xFFFFFFFF
            if target in wanted:
                out[target].append(text_va + p)
    return out


def import_evidence(pe, imports):
    """{iat_va: va of a call site}: the first direct call, else the first call of a jump thunk,
    else the first tail jump, else the first register load. None if nothing names the slot."""
    refs = code_references(pe, [i[3] for i in imports])
    jumps = [va for r in refs.values() for kind, va in r if kind == 'jmp']
    callers = thunk_callers(pe, jumps)
    out = {}
    for slot, r in refs.items():
        calls = sorted(va for kind, va in r if kind == 'call')
        via = sorted(c for kind, va in r if kind == 'jmp' for c in callers[va])
        tails = sorted(va for kind, va in r if kind == 'jmp' and not callers[va])
        loads = sorted(va for kind, va in r if kind == 'load')
        first = calls or via or tails or loads
        out[slot] = first[0] if first else None
    return out


def locate_exe():
    """The exe and its expected hash, found the way the harness finds them."""
    sys.path.insert(0, os.path.join(REPO, 'harness', 'lib'))
    import paths  # noqa: E402  harness/lib/paths.py
    want = None
    with open(paths.HASHES) as f:
        for line in f:
            digest, _, rel = line.strip().partition('  ')
            if rel == paths.EXE_REL:
                want = digest
    return paths.EXE, want


def read_exe(path, want):
    with open(path, 'rb') as f:
        data = f.read()
    got = hashlib.sha256(data).hexdigest()
    if want and got != want:
        raise SystemExit('%s: sha256 %s, expected %s' % (path, got, want))
    return data


# ---------------------------------------------------------------- the COM layouts

METHOD_RE = re.compile(r'^\s*/\*\s*(\d+)\s*\*/\s*(HRESULT|ULONG)\s+\(SFCOP_STDCALL \*(\w+)\)\((.*)\);\s*$')
VTBL_RE = re.compile(r'^typedef struct (\w+)Vtbl \{')
GUID_RE = re.compile(r'static const GUID k(IID|CLSID)_(\w+) = \{(0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), '
                     r'(0x[0-9A-Fa-f]+), \{((?:0x[0-9A-Fa-f]+, ){7}0x[0-9A-Fa-f]+)\}\};')
WIDE_TYPES = re.compile(r'\b(double|int64_t|uint64_t|LONGLONG|ULONGLONG|LARGE_INTEGER)\b')


def split_params(text):
    params, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and depth == 0:
            params.append(cur.strip())
            cur = ''
            continue
        depth += ch == '('
        depth -= ch == ')'
        cur += ch
    if cur.strip():
        params.append(cur.strip())
    return params


def parse_com_header(path):
    """(interface, [(slot, method, stack_bytes)], {name: guid bytes}) from one layout header."""
    iface, methods = None, []
    with open(path) as f:
        text = f.read()
    for n, line in enumerate(text.splitlines(), 1):
        m = VTBL_RE.match(line)
        if m:
            if iface:
                raise ValueError('%s:%d: a second vtable in one file' % (path, n))
            iface = m.group(1)
            continue
        m = METHOD_RE.match(line)
        if not m:
            if iface and '(SFCOP_STDCALL *' in line:
                raise ValueError('%s:%d: method line not in the one-line form' % (path, n))
            continue
        slot, method, params = int(m.group(1)), m.group(3), split_params(m.group(4))
        if slot != len(methods):
            raise ValueError('%s:%d: slot %d out of order' % (path, n, slot))
        if not params or params[0] != '%s *self' % iface:
            raise ValueError('%s:%d: first parameter must be %s *self' % (path, n, iface))
        for p in params:
            if WIDE_TYPES.search(p) or ('*' not in p and p.split()[0] == 'GUID'):
                raise ValueError('%s:%d: 8-byte or by-value structure parameter: %s' % (path, n, p))
        methods.append((slot, method, 4 * len(params)))
    guids = {}
    for kind, name, d1, d2, d3, rest in GUID_RE.findall(text):
        b = struct.pack('<IHH', int(d1, 16), int(d2, 16), int(d3, 16))
        b += bytes(int(x, 16) for x in rest.split(', '))
        guids['%s_%s' % (kind, name)] = b
    return iface, methods, guids


def com_headers(com_dir=COM_DIR):
    """[(interface, methods, guids, file name)] for every interface layout, by file name."""
    out = []
    for fn in sorted(os.listdir(com_dir)):
        if not fn.endswith('.h') or fn == 'ComBase.h' or fn.endswith('Types.h'):
            continue
        iface, methods, guids = parse_com_header(os.path.join(com_dir, fn))
        if iface != fn[:-2]:
            raise ValueError('%s declares %s' % (fn, iface))
        out.append((iface, methods, guids, fn))
    return out


# ---------------------------------------------------------------- the table

def read_surface(path=SURFACE):
    """{(module, name): row dict} from an existing surface.tsv, or {}."""
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        lines = f.read().splitlines()
    if not lines or lines[0].split('\t') != COLUMNS:
        raise ValueError('%s: header is not %s' % (path, COLUMNS))
    rows = {}
    for line in lines[1:]:
        cells = line.split('\t')
        rows[(cells[0], cells[1])] = dict(zip(COLUMNS, cells))
    return rows


def fmt_va(va):
    return '0x%08X' % va


def build_rows(imports, evidence, headers, kept, signatures=None):
    signatures = signatures if signatures is not None else parse_signatures()
    by_ordinal = {(m, o): n for (m, n), (o, _, _) in signatures.items() if o is not None}
    rows = []
    for module, name, ordinal, slot in imports:
        if name is None:
            name = by_ordinal.get((module, ordinal))
            if name is None:
                raise ValueError('%s ordinal %d: no name in SIGNATURES' % (module, ordinal))
        if module == 'MSS32.DLL':
            m = re.match(r'^_\w+@(\d+)$', name)
            if not m:
                raise ValueError('MSS32 import %s is not a decorated stdcall name' % name)
            convention, size = 'stdcall', int(m.group(1))
        else:
            sig = signatures.get((module, name))
            if sig is None:
                raise ValueError('%s!%s: not in SIGNATURES' % (module, name))
            if sig[0] != ordinal:
                raise ValueError('%s!%s: imported by ordinal %s, SIGNATURES says %s'
                                 % (module, name, ordinal, sig[0]))
            convention, size = sig[1], sig[2]
        old = kept.get((module, name), {})
        site = evidence.get(slot)
        rows.append({
            'module': module, 'name': name,
            'ordinal_or_slot': str(ordinal) if ordinal is not None else '-',
            'convention': convention, 'stack_bytes': str(size),
            'status': old.get('status') or ('todo' if site else 'unused'),
            'evidence': fmt_va(site if site else slot),
            'notes': old.get('notes', ''),
        })
    rows.sort(key=lambda r: (r['module'], r['name'].lower()))
    com_rows = []
    for iface, methods, _, _ in headers:
        for slot, method, size in methods:
            old = kept.get((iface, method), {})
            com_rows.append({
                'module': iface, 'name': method, 'ordinal_or_slot': str(slot),
                'convention': 'stdcall', 'stack_bytes': str(size),
                'status': old.get('status') or 'todo',
                'evidence': old.get('evidence') or '-',
                'notes': old.get('notes', ''),
            })
    return rows + com_rows


def render(rows):
    lines = ['\t'.join(COLUMNS)]
    for r in rows:
        if r['status'] not in STATUSES or r['convention'] not in CONVENTIONS:
            raise ValueError('%s %s: status %r, convention %r'
                             % (r['module'], r['name'], r['status'], r['convention']))
        cells = [r[c] for c in COLUMNS]
        for c in cells:
            if '\t' in c or '\n' in c:
                raise ValueError('tab or newline in a cell: %r' % c)
        lines.append('\t'.join(cells))
    return '\n'.join(lines) + '\n'


def generate(exe_path=None, surface=SURFACE):
    path, want = locate_exe()
    if exe_path:
        path = exe_path
    pe = Pe(read_exe(path, want))
    imports = pe.imports()
    return render(build_rows(imports, import_evidence(pe, imports), com_headers(),
                             read_surface(surface)))


# ---------------------------------------------------------------- --libs

def library_signatures(lib_dir, wanted_modules):
    """{(MODULE, name): (ordinal or None, convention, bytes)} from short-format import members."""
    lib_of = {'DBGHELP.DLL': 'IMAGEHLP.LIB'}
    out = {}
    for module in wanted_modules:
        lib = os.path.join(lib_dir, lib_of.get(module, module.replace('.DLL', '.LIB')))
        with open(lib, 'rb') as f:
            d = f.read()
        if d[:8] != b'!<arch>\n':
            raise ValueError(lib + ': not an archive')
        p = 8
        while p + 60 <= len(d):
            size = int(d[p + 48:p + 58].decode('ascii').strip())
            body = d[p + 60:p + 60 + size]
            p += 60 + size + (size & 1)
            if len(body) < 20 or body[:4] != b'\0\0\xff\xff':
                continue
            ordinal, kind = struct.unpack_from('<HH', body, 16)
            sym = body[20:].split(b'\0')[0].decode('ascii')
            m = re.match(r'^_(\w+?)(?:@(\d+))?$', sym)
            if not m:
                continue
            by_ordinal = (kind >> 2) & 7 == 0
            conv = 'stdcall' if m.group(2) is not None else 'varargs'
            out.setdefault((module, m.group(1)),
                           (ordinal if by_ordinal else None, conv, int(m.group(2) or 0)))
    return out


def check_libs(lib_dir):
    table = parse_signatures()
    libs = library_signatures(lib_dir, sorted({m for m, _ in table}))
    bad = 0
    for key, (ordinal, conv, size) in sorted(table.items()):
        got = libs.get(key)
        if got is None or got[1:] != (conv, size) or (ordinal is not None and got[0] != ordinal):
            print('%s!%s: table %s, libraries %s' % (key[0], key[1], (ordinal, conv, size), got))
            bad += 1
    print('%d signatures checked against %s, %d differ' % (len(table), lib_dir, bad))
    return bad == 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true', help='exit 1 if surface.tsv would change')
    ap.add_argument('--exe', help='the exe (default: where the harness finds it)')
    ap.add_argument('--libs', help='a directory of Win32 import libraries to check SIGNATURES')
    args = ap.parse_args(argv)
    if args.libs and not check_libs(args.libs):
        return 1
    text = generate(args.exe)
    current = open(SURFACE).read() if os.path.exists(SURFACE) else ''
    if args.check:
        if text != current:
            print('surface.tsv is out of date: run python3 src/hle/gen_surface.py')
            return 1
        print('surface.tsv is up to date')
        return 0
    if text != current:
        with open(SURFACE, 'w') as f:
            f.write(text)
    print('wrote %s: %d rows' % (os.path.relpath(SURFACE, REPO), text.count('\n') - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
