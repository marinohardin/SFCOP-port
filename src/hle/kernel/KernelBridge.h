/*
 * The kernel module as KernelGuest.c calls it: plain C, because the lift's headers do not compile
 * as C++ (src/hle/README.md). The shim reads the game's arguments, turns guest addresses into
 * host pointers and calls these; they convert the game's strings (ANSI, or UTF-16 where `wide` is
 * 1) and answer in the game's form, including the guest structures, which they write byte by
 * byte. A file or search id is the module's; the shim maps it to the handle the game holds.
 *
 * One kernel serves the process, behind a lock. Until the guest runtime attaches it to the
 * client's folders, the shim attaches it on first use to the game folder and the per-user folder
 * the client would choose without --data.
 */
#ifndef SFCOP_HLE_KERNEL_BRIDGE_H
#define SFCOP_HLE_KERNEL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The install (read-only) and the per-user folder, absolute host paths in UTF-8. The per-user
 * folder is created when missing. Replaces any earlier attachment and closes what it held. */
void sfcop_hle_kernel_attach(const char *installDir, const char *userDir);
void sfcop_hle_kernel_detach(void);
int sfcop_hle_kernel_attached(void);
/* The per-user folder the client uses when no --data is given (src/app/DataDir.h) into `out`;
 * its length, or 0 when the platform's variables do not give one. */
size_t sfcop_hle_default_user_dir(char *out, size_t size);

/* The error the last call set, if it set one (1, *code), else 0. Reading clears it. */
int sfcop_kernel_take_error(uint32_t *code);

/* ---- paths ---- */
/* The GetCurrentDirectory / GetWindowsDirectory / GetSystemDirectory answer: the length without
 * the NUL when it fits in `size` characters, else the size needed with it. */
uint32_t sfcop_kernel_current_directory(void *buffer, uint32_t size, int wide);
uint32_t sfcop_kernel_windows_directory(void *buffer, uint32_t size, int wide, int system);
int sfcop_kernel_set_current_directory(const void *name, int wide);
/* GetModuleFileNameA's answer for the game's own module: truncated to `size` with its NUL. */
uint32_t sfcop_kernel_module_file_name(char *buffer, uint32_t size);
/* GetFullPathNameA: `*filePart` is the offset of the last segment in `buffer`, or UINT32_MAX. */
uint32_t sfcop_kernel_full_path_name(const char *name, char *buffer, uint32_t size,
                                     uint32_t *filePart);
uint32_t sfcop_kernel_short_path_name(const char *name, char *buffer, uint32_t size);
uint32_t sfcop_kernel_drive_type(const void *root, int wide);
int sfcop_kernel_disk_free_space(const void *root, int wide, uint32_t *sectorsPerCluster,
                                 uint32_t *bytesPerSector, uint32_t *freeClusters,
                                 uint32_t *totalClusters);
/* GetVolumeInformation; a name buffer may be NULL; one too small fails with ERROR_BAD_LENGTH. */
int sfcop_kernel_volume_information(const void *root, int wide, void *volumeName,
                                    uint32_t volumeSize, uint32_t *serial,
                                    uint32_t *maxComponentLength, uint32_t *flags,
                                    void *fileSystem, uint32_t fileSystemSize);

/* ---- files ---- */
/* A file id (never 0), or 0 with the error set. */
uint32_t sfcop_kernel_create_file(const void *name, int wide, uint32_t access, uint32_t share,
                                  uint32_t disposition, uint32_t flags);
int sfcop_kernel_is_file(uint32_t file);
int sfcop_kernel_read_file(uint32_t file, void *out, uint32_t bytes, uint32_t *read);
int sfcop_kernel_write_file(uint32_t file, const void *data, uint32_t bytes, uint32_t *written);
uint32_t sfcop_kernel_set_file_pointer(uint32_t file, int32_t distance, int32_t *high,
                                       uint32_t method);
uint32_t sfcop_kernel_get_file_size(uint32_t file, uint32_t *high);
/* FILETIMEs as 64-bit counts; a NULL pointer is a time not asked for or not changed. */
int sfcop_kernel_get_file_time(uint32_t file, uint64_t *creation, uint64_t *access,
                               uint64_t *write);
int sfcop_kernel_set_file_time(uint32_t file, const uint64_t *creation, const uint64_t *access,
                               const uint64_t *write);
int sfcop_kernel_flush_file(uint32_t file);
int sfcop_kernel_set_end_of_file(uint32_t file);
int sfcop_kernel_close_file(uint32_t file);

int sfcop_kernel_delete_file(const void *name, int wide);
int sfcop_kernel_move_file(const char *from, const char *to);
int sfcop_kernel_copy_file(const char *from, const char *to, int failIfExists);
int sfcop_kernel_create_directory(const void *name, int wide);
int sfcop_kernel_remove_directory(const void *name, int wide);
uint32_t sfcop_kernel_get_file_attributes(const void *name, int wide);
int sfcop_kernel_set_file_attributes(const void *name, int wide, uint32_t attributes);

/* ---- directory search ----
 * `data` is the game's WIN32_FIND_DATAA (320 bytes) or WIN32_FIND_DATAW (592 bytes). */
#define SFCOP_FIND_DATA_A_SIZE 320u
#define SFCOP_FIND_DATA_W_SIZE 592u
/* A search id (never 0) with the first entry written to `data`, or 0 with the error set. */
uint32_t sfcop_kernel_find_first(const void *pattern, int wide, uint8_t *data);
int sfcop_kernel_find_next(uint32_t find, uint8_t *data, int wide);
int sfcop_kernel_find_close(uint32_t find);
int sfcop_kernel_is_find(uint32_t find);

/* ---- .ini profiles (ANSI only, as the game imports them) ---- */
uint32_t sfcop_kernel_get_profile_string(const char *section, const char *key,
                                         const char *fallback, char *buffer, uint32_t size,
                                         const char *file);
uint32_t sfcop_kernel_get_profile_int(const char *section, const char *key, int32_t fallback,
                                      const char *file);
int sfcop_kernel_write_profile_string(const char *section, const char *key, const char *value,
                                      const char *file);

#ifdef __cplusplus
}
#endif

#endif /* SFCOP_HLE_KERNEL_BRIDGE_H */
