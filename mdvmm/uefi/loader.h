/* BOOTX64.EFI: shared between its source files */
#ifndef MDVMM_LOADER_H
#define MDVMM_LOADER_H
#include "efi.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

/* main.c: console output that is also kept for \MDOS\BOOTLOG.TXT */
void print(const CHAR16 *s);
void print_hex(uint64_t v);
void print_dec(uint64_t v);
void save_log(void);

/* install.c */
int is_trial(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
void trial_boot(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
void install_offer(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

#endif
