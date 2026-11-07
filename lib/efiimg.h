/* Builders for UEFI boot media: a FAT16 EFI System Partition image and a GPT partition table.
 * Shared by the host tool (tools/mkefi.c) and the installer (user/bin/pinstall.c); freestanding. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EFI_SECTOR 512
#define GPT_HEAD_SECTORS 34              /* protective MBR + header + 32 sectors of partition entries */
#define GPT_TAIL_SECTORS 33              /* backup entries + backup header */

uint32_t efi_crc32(const uint8_t *p, size_t n);

/* Sectors an ESP needs for these files (never less than 8192, i.e. 4 MiB). */
uint32_t efi_esp_sectors(size_t efi_len, size_t kernel_len);

/* Fills img (total_secs * 512 zeroed bytes) with a FAT16 volume holding /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN.
 * `hidden` is the partition's first LBA on its disk.  Returns 0, or -1 if the files don't fit. */
int efi_build_esp(uint8_t *img, uint32_t total_secs, const uint8_t *efi, size_t efi_len, const uint8_t *kernel,
                  size_t kernel_len, uint32_t hidden);

/* A GPT with the ESP at [esp_first, esp_last] and, if pfs_last, Petrichor's data partition at [pfs_first, pfs_last].
 * head: GPT_HEAD_SECTORS sectors for LBA 0.., tail: GPT_TAIL_SECTORS sectors for LBA total-33.. (both zeroed). */
void efi_build_gpt(uint8_t *head, uint8_t *tail, uint64_t total, uint64_t esp_first, uint64_t esp_last,
                   uint64_t pfs_first, uint64_t pfs_last, const uint8_t disk_guid[16]);
