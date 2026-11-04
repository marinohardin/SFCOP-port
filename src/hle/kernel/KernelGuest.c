/*
 * KERNEL32's file, directory, module-path and .ini rows for the game, in pcrecomp's win32hle shim
 * form (src/hle/README.md): each shim reads its stdcall arguments from the guest stack, calls the
 * kernel module through KernelBridge.h, writes out-parameters back into guest memory, hands the
 * module's error to GetLastError and pops its arguments.
 *
 * The file and search handles the game holds are win32hle handle-table entries of two types of
 * this file's own, each holding the module's id; CloseHandle and FindClose release them. A
 * handle-taking row given a handle the module did not make (a standard handle, a file another
 * module opened) is passed to win32hle's own row, which answers it as before.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hle/kernel/KernelBridge.h"
#include "win32hle.h"

/* Handle types of this file's own in win32hle's table (its own types end at HLE_H_MMIO). */
#define KERNEL_H_FILE 24
#define KERNEL_H_FIND 25

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_INVALID_HANDLE_CODE 6u
#define ERROR_TOO_MANY_OPEN_FILES_CODE 4u
#define FILE_TYPE_DISK 1u
#define PATH_CHARS 1024

/* ---- attachment, errors and the rows this file hands on ---- */

static void file_closer(void *obj) { sfcop_kernel_close_file((uint32_t)(uintptr_t)obj); }
static void find_closer(void *obj) { sfcop_kernel_find_close((uint32_t)(uintptr_t)obj); }

/* Until the guest runtime attaches the module to the client's folders: the game folder (drive
 * C:, as the host maps it) and the per-user folder the client uses without --data. */
static void ensure_attached(void) {
    static int closers_set;
    if (!closers_set) {
        hle_handle_set_closer(KERNEL_H_FILE, file_closer);
        hle_handle_set_closer(KERNEL_H_FIND, find_closer);
        closers_set = 1;
    }
    if (sfcop_hle_kernel_attached()) return;
    char install[PATH_CHARS] = "", user[PATH_CHARS] = "";
    hle_host_path("C:\\", install, sizeof install);
    sfcop_hle_default_user_dir(user, sizeof user);
    sfcop_hle_kernel_attach(install, user);
}

/* The module's last error, if it set one, as GetLastError's next answer. */
static void pass_error(void) {
    uint32_t code = 0;
    if (sfcop_kernel_take_error(&code)) hle_set_last_error(code);
}

/* win32hle's own row of this name, which this table's registration hides. */
static recomp_func_t original(const char *name) {
    const win32hle_shim *tables[] = { win32hle_kernel32_ext, win32hle_kernel32, win32hle_module,
                                      win32hle_kernel32_crt };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (const win32hle_shim *s = tables[t]; s->name; s++)
            if (strcmp(s->name, name) == 0) return s->fn;
    return NULL;
}

/* Runs win32hle's row, which pops the arguments; without one, fails as Windows does for a handle
 * nobody made. */
static void hand_on(const char *name, int argc, uint32_t failure) {
    recomp_func_t fn = original(name);
    if (fn) { fn(); return; }
    hle_set_last_error(ERROR_INVALID_HANDLE_CODE);
    RET(failure, argc);
}

static uint32_t file_of(uint32_t handle) {
    return (uint32_t)(uintptr_t)hle_handle_obj(handle, KERNEL_H_FILE);
}
static uint32_t find_of(uint32_t handle) {
    return (uint32_t)(uintptr_t)hle_handle_obj(handle, KERNEL_H_FIND);
}

/* A guest handle for a module id, or INVALID_HANDLE when the table is full (the id is closed). */
static uint32_t handle_for(int type, uint32_t id) {
    uint32_t handle = hle_handle_alloc(type, (void *)(uintptr_t)id);
    if (handle) return handle;
    if (type == KERNEL_H_FILE) sfcop_kernel_close_file(id);
    else sfcop_kernel_find_close(id);
    hle_set_last_error(ERROR_TOO_MANY_OPEN_FILES_CODE);
    return INVALID_HANDLE;
}

static void put32(uint32_t va, uint32_t value) { if (va) MEM32(va) = value; }
static void put_filetime(uint32_t va, uint64_t time) {
    if (!va) return;
    MEM32(va) = (uint32_t)time;
    MEM32(va + 4) = (uint32_t)(time >> 32);
}
static uint64_t get_filetime(uint32_t va) {
    return (uint64_t)MEM32(va) | (uint64_t)MEM32(va + 4) << 32;
}

/* With --trace: which game path this module answered, so a trace shows the rows are ours. */
static void trace_path_with(const char *row, const void *name, int wide, uint32_t result,
                            const char *extra) {
    if (!win32hle_trace) return;
    char shown[PATH_CHARS] = "";
    if (name && wide) {
        const uint16_t *w = (const uint16_t *)name;
        size_t i = 0;
        for (; w[i] && i + 1 < sizeof shown; i++) shown[i] = w[i] < 0x80 ? (char)w[i] : '?';
        shown[i] = 0;
    } else if (name) {
        snprintf(shown, sizeof shown, "%s", (const char *)name);
    }
    fprintf(stderr, "[hle-kernel] %s \"%s\"%s -> 0x%x\n", row, shown, extra, result);
}
static void trace_path(const char *row, const void *name, int wide, uint32_t result) {
    trace_path_with(row, name, wide, result, "");
}

/* ---- paths ---- */

static void k_GetCurrentDirectoryA(void) {               /* (size, buffer) */
    ensure_attached();
    uint32_t n = sfcop_kernel_current_directory(APTR(1), A32(0), 0);
    pass_error();
    RET(n, 2);
}
static void k_GetCurrentDirectoryW(void) {
    ensure_attached();
    uint32_t n = sfcop_kernel_current_directory(APTR(1), A32(0), 1);
    pass_error();
    RET(n, 2);
}

/* win32hle keeps a current directory of its own for the rows other modules answer (loading a
 * library, a sound or movie file by a relative name): it follows the module's. */
static void set_current_directory(int wide) {
    ensure_attached();
    int ok = sfcop_kernel_set_current_directory(APTR(0), wide);
    pass_error();
    trace_path(wide ? "SetCurrentDirectoryW" : "SetCurrentDirectoryA", APTR(0), wide, (uint32_t)ok);
    recomp_func_t fn = original("SetCurrentDirectoryA");
    if (!ok || !fn) RET(ok, 1);
    char now[PATH_CHARS];
    if (sfcop_kernel_current_directory(now, sizeof now, 0) >= sizeof now) RET(1, 1);
    uint32_t mark = hle_scratch_mark();
    MEM32(g_esp + 4u) = hle_scratch_copy(now, (uint32_t)strlen(now) + 1);
    uint32_t error = hle_get_last_error();
    fn();                                                 /* pops the argument */
    hle_set_last_error(error);
    hle_scratch_release(mark);
    g_eax = 1;
}
static void k_SetCurrentDirectoryA(void) { set_current_directory(0); }
static void k_SetCurrentDirectoryW(void) { set_current_directory(1); }

static void k_GetModuleFileNameA(void) {                 /* (module, buffer, size) */
    if (A32(0) != 0) { hand_on("GetModuleFileNameA", 3, 0); return; }
    ensure_attached();
    uint32_t n = sfcop_kernel_module_file_name(ASTR(1), A32(2));
    pass_error();
    RET(n, 3);
}

static void k_GetFullPathNameA(void) {                   /* (name, size, buffer, &filePart) */
    ensure_attached();
    uint32_t buffer = A32(2), part = UINT32_MAX;
    uint32_t n = sfcop_kernel_full_path_name(ASTR(0), (char *)hle_ptr(buffer), A32(1), &part);
    pass_error();
    put32(A32(3), part != UINT32_MAX ? buffer + part : 0);
    RET(n, 4);
}

static void k_GetShortPathNameA(void) {                  /* (long, short, size) */
    ensure_attached();
    uint32_t n = sfcop_kernel_short_path_name(ASTR(0), ASTR(1), A32(2));
    pass_error();
    RET(n, 3);
}

static void windows_directory(int wide, int system) {   /* (buffer, size) */
    ensure_attached();
    uint32_t n = sfcop_kernel_windows_directory(APTR(0), A32(1), wide, system);
    pass_error();
    RET(n, 2);
}
static void k_GetWindowsDirectoryA(void) { windows_directory(0, 0); }
static void k_GetWindowsDirectoryW(void) { windows_directory(1, 0); }
static void k_GetSystemDirectoryA(void) { windows_directory(0, 1); }
static void k_GetSystemDirectoryW(void) { windows_directory(1, 1); }

static void drive_type(int wide) {                       /* (root) */
    ensure_attached();
    uint32_t type = sfcop_kernel_drive_type(APTR(0), wide);
    pass_error();
    RET(type, 1);
}
static void k_GetDriveTypeA(void) { drive_type(0); }
static void k_GetDriveTypeW(void) { drive_type(1); }

static void disk_free_space(int wide) {   /* (root, &sectorsPerCluster, &bytesPerSector, &free, &total) */
    ensure_attached();
    uint32_t spc = 0, bps = 0, freeClusters = 0, total = 0;
    int ok = sfcop_kernel_disk_free_space(APTR(0), wide, &spc, &bps, &freeClusters, &total);
    pass_error();
    if (ok) put32(A32(1), spc), put32(A32(2), bps), put32(A32(3), freeClusters), put32(A32(4), total);
    RET(ok, 5);
}
static void k_GetDiskFreeSpaceA(void) { disk_free_space(0); }
static void k_GetDiskFreeSpaceW(void) { disk_free_space(1); }

/* (root, volumeName, volumeSize, &serial, &maxComponentLength, &flags, fileSystem, fsSize) */
static void volume_information(int wide) {
    ensure_attached();
    uint32_t serial = 0, length = 0, flags = 0;
    int ok = sfcop_kernel_volume_information(APTR(0), wide, APTR(1), A32(2), &serial, &length,
                                             &flags, APTR(6), A32(7));
    pass_error();
    if (ok) put32(A32(3), serial), put32(A32(4), length), put32(A32(5), flags);
    RET(ok, 8);
}
static void k_GetVolumeInformationA(void) { volume_information(0); }
static void k_GetVolumeInformationW(void) { volume_information(1); }

/* ---- files ---- */

/* (name, access, share, security, disposition, flags, template) */
static void create_file(int wide) {
    ensure_attached();
    uint32_t id = sfcop_kernel_create_file(APTR(0), wide, A32(1), A32(2), A32(4), A32(5));
    pass_error();
    uint32_t handle = id ? handle_for(KERNEL_H_FILE, id) : INVALID_HANDLE;
    char how[48];
    snprintf(how, sizeof how, " access 0x%x disposition %u", A32(1), A32(4));
    trace_path_with(wide ? "CreateFileW" : "CreateFileA", APTR(0), wide, handle, how);
    RET(handle, 7);
}
static void k_CreateFileA(void) { create_file(0); }
static void k_CreateFileW(void) { create_file(1); }

/* An OVERLAPPED's offset (at 8 and 12) is where the transfer starts; the call completes at once,
 * so its status (at 0) and count (at 4) are written straight away. */
static int seek_overlapped(uint32_t id, uint32_t overlapped) {
    if (!overlapped) return 1;
    int32_t high = (int32_t)MEM32(overlapped + 12);
    uint32_t low = sfcop_kernel_set_file_pointer(id, (int32_t)MEM32(overlapped + 8), &high, 0);
    uint32_t code = 0;
    if (low == 0xFFFFFFFFu && sfcop_kernel_take_error(&code)) {
        hle_set_last_error(code);
        return 0;
    }
    return 1;
}
static void finish_overlapped(uint32_t overlapped, uint32_t count) {
    if (!overlapped) return;
    MEM32(overlapped) = 0;
    MEM32(overlapped + 4) = count;
}

static void k_ReadFile(void) {                  /* (handle, buffer, count, &read, overlapped) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("ReadFile", 5, 0); return; }
    uint32_t read = 0;
    int ok = seek_overlapped(id, A32(4)) && sfcop_kernel_read_file(id, APTR(1), A32(2), &read);
    pass_error();
    put32(A32(3), read);
    finish_overlapped(A32(4), read);
    RET(ok, 5);
}

static void k_WriteFile(void) {                 /* (handle, data, count, &written, overlapped) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("WriteFile", 5, 0); return; }
    uint32_t written = 0;
    int ok = seek_overlapped(id, A32(4)) && sfcop_kernel_write_file(id, APTR(1), A32(2), &written);
    pass_error();
    put32(A32(3), written);
    finish_overlapped(A32(4), written);
    RET(ok, 5);
}

static void k_SetFilePointer(void) {            /* (handle, distance, &distanceHigh, method) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("SetFilePointer", 4, 0xFFFFFFFFu); return; }
    uint32_t highVa = A32(2);
    int32_t high = highVa ? (int32_t)MEM32(highVa) : 0;
    uint32_t low = sfcop_kernel_set_file_pointer(id, (int32_t)A32(1), highVa ? &high : NULL, A32(3));
    pass_error();
    put32(highVa, (uint32_t)high);
    RET(low, 4);
}

static void k_GetFileSize(void) {               /* (handle, &high) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("GetFileSize", 2, 0xFFFFFFFFu); return; }
    uint32_t high = 0;
    uint32_t low = sfcop_kernel_get_file_size(id, &high);
    pass_error();
    put32(A32(1), high);
    RET(low, 2);
}

static void k_GetFileTime(void) {               /* (handle, &creation, &access, &write) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("GetFileTime", 4, 0); return; }
    uint64_t creation = 0, access = 0, write = 0;
    int ok = sfcop_kernel_get_file_time(id, &creation, &access, &write);
    pass_error();
    if (ok) put_filetime(A32(1), creation), put_filetime(A32(2), access), put_filetime(A32(3), write);
    RET(ok, 4);
}

static void k_SetFileTime(void) {               /* (handle, creation, access, write) */
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("SetFileTime", 4, 0); return; }
    uint64_t creation = A32(1) ? get_filetime(A32(1)) : 0;
    uint64_t access = A32(2) ? get_filetime(A32(2)) : 0;
    uint64_t write = A32(3) ? get_filetime(A32(3)) : 0;
    int ok = sfcop_kernel_set_file_time(id, A32(1) ? &creation : NULL, A32(2) ? &access : NULL,
                                        A32(3) ? &write : NULL);
    pass_error();
    RET(ok, 4);
}

static void k_FlushFileBuffers(void) {
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("FlushFileBuffers", 1, 0); return; }
    int ok = sfcop_kernel_flush_file(id);
    pass_error();
    RET(ok, 1);
}

static void k_SetEndOfFile(void) {
    uint32_t id = file_of(A32(0));
    if (!id) { hand_on("SetEndOfFile", 1, 0); return; }
    int ok = sfcop_kernel_set_end_of_file(id);
    pass_error();
    RET(ok, 1);
}

static void k_GetFileType(void) {
    if (!file_of(A32(0))) { hand_on("GetFileType", 1, 0); return; }
    RET(FILE_TYPE_DISK, 1);
}

static void k_CloseHandle(void) {
    uint32_t handle = A32(0);
    if (!file_of(handle)) { hand_on("CloseHandle", 1, 0); return; }
    hle_handle_close(handle);                     /* the closer closes the module's file */
    pass_error();
    RET(1, 1);
}

static void name_row(const char *row, int (*call)(const void *, int), int wide) {   /* (name) */
    ensure_attached();
    int ok = call(APTR(0), wide);
    pass_error();
    trace_path(row, APTR(0), wide, (uint32_t)ok);
    RET(ok, 1);
}
static void k_DeleteFileA(void) { name_row("DeleteFileA", sfcop_kernel_delete_file, 0); }
static void k_DeleteFileW(void) { name_row("DeleteFileW", sfcop_kernel_delete_file, 1); }
static void k_RemoveDirectoryA(void) { name_row("RemoveDirectoryA", sfcop_kernel_remove_directory, 0); }
static void k_RemoveDirectoryW(void) { name_row("RemoveDirectoryW", sfcop_kernel_remove_directory, 1); }

static void create_directory(int wide) {        /* (name, security) */
    ensure_attached();
    int ok = sfcop_kernel_create_directory(APTR(0), wide);
    pass_error();
    trace_path(wide ? "CreateDirectoryW" : "CreateDirectoryA", APTR(0), wide, (uint32_t)ok);
    RET(ok, 2);
}
static void k_CreateDirectoryA(void) { create_directory(0); }
static void k_CreateDirectoryW(void) { create_directory(1); }

static void k_MoveFileA(void) {                 /* (from, to) */
    ensure_attached();
    int ok = sfcop_kernel_move_file(ASTR(0), ASTR(1));
    pass_error();
    trace_path("MoveFileA", ASTR(0), 0, (uint32_t)ok);
    RET(ok, 2);
}

static void k_CopyFileA(void) {                 /* (from, to, failIfExists) */
    ensure_attached();
    int ok = sfcop_kernel_copy_file(ASTR(0), ASTR(1), A32(2) != 0);
    pass_error();
    trace_path("CopyFileA", ASTR(0), 0, (uint32_t)ok);
    RET(ok, 3);
}

static void get_file_attributes(int wide) {     /* (name) */
    ensure_attached();
    uint32_t attributes = sfcop_kernel_get_file_attributes(APTR(0), wide);
    pass_error();
    trace_path(wide ? "GetFileAttributesW" : "GetFileAttributesA", APTR(0), wide, attributes);
    RET(attributes, 1);
}
static void k_GetFileAttributesA(void) { get_file_attributes(0); }
static void k_GetFileAttributesW(void) { get_file_attributes(1); }

static void set_file_attributes(int wide) {     /* (name, attributes) */
    ensure_attached();
    int ok = sfcop_kernel_set_file_attributes(APTR(0), wide, A32(1));
    pass_error();
    RET(ok, 2);
}
static void k_SetFileAttributesA(void) { set_file_attributes(0); }
static void k_SetFileAttributesW(void) { set_file_attributes(1); }

/* ---- directory search ---- */

static void find_first(int wide) {              /* (pattern, &data) */
    ensure_attached();
    uint32_t id = sfcop_kernel_find_first(APTR(0), wide, (uint8_t *)APTR(1));
    pass_error();
    uint32_t handle = id ? handle_for(KERNEL_H_FIND, id) : INVALID_HANDLE;
    trace_path(wide ? "FindFirstFileW" : "FindFirstFileA", APTR(0), wide, handle);
    RET(handle, 2);
}
static void k_FindFirstFileA(void) { find_first(0); }
static void k_FindFirstFileW(void) { find_first(1); }

static void find_next(const char *row, int wide) {   /* (handle, &data) */
    uint32_t id = find_of(A32(0));
    if (!id) { hand_on(row, 2, 0); return; }
    int ok = sfcop_kernel_find_next(id, (uint8_t *)APTR(1), wide);
    pass_error();
    RET(ok, 2);
}
static void k_FindNextFileA(void) { find_next("FindNextFileA", 0); }
static void k_FindNextFileW(void) { find_next("FindNextFileW", 1); }

static void k_FindClose(void) {
    uint32_t handle = A32(0);
    if (!find_of(handle)) { hand_on("FindClose", 1, 0); return; }
    hle_handle_close(handle);                     /* the closer ends the module's search */
    pass_error();
    RET(1, 1);
}

/* ---- .ini profiles ---- */

static void k_GetPrivateProfileStringA(void) {  /* (section, key, default, buffer, size, file) */
    ensure_attached();
    uint32_t n = sfcop_kernel_get_profile_string(ASTR(0), ASTR(1), ASTR(2), ASTR(3), A32(4), ASTR(5));
    pass_error();
    trace_path("GetPrivateProfileStringA", ASTR(5), 0, n);
    RET(n, 6);
}

static void k_GetPrivateProfileIntA(void) {     /* (section, key, default, file) */
    ensure_attached();
    uint32_t value = sfcop_kernel_get_profile_int(ASTR(0), ASTR(1), (int32_t)A32(2), ASTR(3));
    pass_error();
    trace_path("GetPrivateProfileIntA", ASTR(3), 0, value);
    RET(value, 4);
}

static void k_WritePrivateProfileStringA(void) {   /* (section, key, value, file) */
    ensure_attached();
    int ok = sfcop_kernel_write_profile_string(ASTR(0), ASTR(1), ASTR(2), ASTR(3));
    pass_error();
    trace_path("WritePrivateProfileStringA", ASTR(3), 0, (uint32_t)ok);
    RET(ok, 4);
}

const win32hle_shim sfcop_hle_kernel[] = {
    { "CloseHandle", k_CloseHandle },
    { "CopyFileA", k_CopyFileA },
    { "CreateDirectoryA", k_CreateDirectoryA }, { "CreateDirectoryW", k_CreateDirectoryW },
    { "CreateFileA", k_CreateFileA }, { "CreateFileW", k_CreateFileW },
    { "DeleteFileA", k_DeleteFileA }, { "DeleteFileW", k_DeleteFileW },
    { "FindClose", k_FindClose },
    { "FindFirstFileA", k_FindFirstFileA }, { "FindFirstFileW", k_FindFirstFileW },
    { "FindNextFileA", k_FindNextFileA }, { "FindNextFileW", k_FindNextFileW },
    { "FlushFileBuffers", k_FlushFileBuffers },
    { "GetCurrentDirectoryA", k_GetCurrentDirectoryA }, { "GetCurrentDirectoryW", k_GetCurrentDirectoryW },
    { "GetDiskFreeSpaceA", k_GetDiskFreeSpaceA }, { "GetDiskFreeSpaceW", k_GetDiskFreeSpaceW },
    { "GetDriveTypeA", k_GetDriveTypeA }, { "GetDriveTypeW", k_GetDriveTypeW },
    { "GetFileAttributesA", k_GetFileAttributesA }, { "GetFileAttributesW", k_GetFileAttributesW },
    { "GetFileSize", k_GetFileSize },
    { "GetFileTime", k_GetFileTime },
    { "GetFileType", k_GetFileType },
    { "GetFullPathNameA", k_GetFullPathNameA },
    { "GetModuleFileNameA", k_GetModuleFileNameA },
    { "GetPrivateProfileIntA", k_GetPrivateProfileIntA },
    { "GetPrivateProfileStringA", k_GetPrivateProfileStringA },
    { "GetShortPathNameA", k_GetShortPathNameA },
    { "GetSystemDirectoryA", k_GetSystemDirectoryA }, { "GetSystemDirectoryW", k_GetSystemDirectoryW },
    { "GetVolumeInformationA", k_GetVolumeInformationA }, { "GetVolumeInformationW", k_GetVolumeInformationW },
    { "GetWindowsDirectoryA", k_GetWindowsDirectoryA }, { "GetWindowsDirectoryW", k_GetWindowsDirectoryW },
    { "MoveFileA", k_MoveFileA },
    { "ReadFile", k_ReadFile },
    { "RemoveDirectoryA", k_RemoveDirectoryA }, { "RemoveDirectoryW", k_RemoveDirectoryW },
    { "SetCurrentDirectoryA", k_SetCurrentDirectoryA }, { "SetCurrentDirectoryW", k_SetCurrentDirectoryW },
    { "SetEndOfFile", k_SetEndOfFile },
    { "SetFileAttributesA", k_SetFileAttributesA }, { "SetFileAttributesW", k_SetFileAttributesW },
    { "SetFilePointer", k_SetFilePointer },
    { "SetFileTime", k_SetFileTime },
    { "WriteFile", k_WriteFile },
    { "WritePrivateProfileStringA", k_WritePrivateProfileStringA },
    { 0, 0 }
};
