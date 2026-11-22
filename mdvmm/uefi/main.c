/* BOOTX64.EFI - UEFI loader for the modernDOS virtual machine monitor
 *
 * Reads the DOS boot disk image \MODERNDOS\BOOT.IMG from the boot volume,
 * takes the GOP framebuffer and a memory pool below 4GB, copies the MDVMM
 * core (embedded, start.asm) into the pool, exits boot services and starts
 * the core in 32-bit protected mode (enter_core, start.asm). The core then
 * provides a PC BIOS and boots DOS from the disk image in V86 mode.
 *
 * Built position independent (no absolute relocations): the firmware may
 * load the image anywhere.
 */
#include "efi.h"
#include "bootinfo.h"

#define POOL_MB_MAX   32
#define POOL_MB_MIN   8
#define XMS_MB_MAX    64              /* DOS extended memory */
#define XMS_MB_MIN    1
#define BOOT_IMAGE    u"\\MODERNDOS\\BOOT.IMG"

extern const uint8_t core_image[], core_image_end[];
void enter_core(uint64_t low, uint64_t bootinfo, uint64_t pool) __attribute__((noreturn));

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static uint8_t map_buf[32768];

static EFI_GUID loaded_image_guid = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID simple_fs_guid = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static EFI_GUID acpi20_guid = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

/* the compiler may emit calls to these */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n)
{
  uint8_t *dp = d;
  const uint8_t *sp = s;
  while (n--)
    *dp++ = *sp++;
  return d;
}
void *memset(void *d, int c, size_t n)
{
  uint8_t *dp = d;
  while (n--)
    *dp++ = (uint8_t)c;
  return d;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  int i;
  for (i = 0; i < 16; i++)
    if (x[i] != y[i])
      return 0;
  return 1;
}

static void print(const CHAR16 *s)
{
  st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}

static void print_hex(uint64_t v)
{
  CHAR16 buf[19];
  int i;
  buf[0] = '0';
  buf[1] = 'x';
  for (i = 0; i < 16; i++)
    buf[2 + i] = (CHAR16)"0123456789ABCDEF"[(v >> (60 - 4 * i)) & 15];
  buf[18] = 0;
  print(buf);
}

static EFI_STATUS fail(const CHAR16 *what, EFI_STATUS s)
{
  print(u"MDVMM: ");
  print(what);
  print(u" (status ");
  print_hex(s);
  print(u")\r\n");
  return s;
}

/* read the whole file into pages below 4GB, above the V86 address space */
static EFI_STATUS load_file(EFI_HANDLE image, const CHAR16 *name, uint64_t *addr, uint64_t *size)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root, *file;
  EFI_STATUS s;
  UINTN n;

  if ((s = bs->HandleProtocol(image, &loaded_image_guid, (void **)&li)) ||
      (s = bs->HandleProtocol(li->DeviceHandle, &simple_fs_guid, (void **)&fs)) ||
      (s = fs->OpenVolume(fs, &root)))
    return fail(u"cannot open the boot volume", s);
  if ((s = root->Open(root, &file, (CHAR16 *)name, EFI_FILE_MODE_READ, 0)))
    return fail(u"cannot open \\MODERNDOS\\BOOT.IMG", s);
  file->SetPosition(file, ~0ull);
  file->GetPosition(file, size);
  file->SetPosition(file, 0);
  *addr = 0xffffffffull;
  if ((s = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, (*size + 4095) / 4096, addr)))
    return fail(u"no memory for the disk image", s);
  n = *size;
  if ((s = file->Read(file, &n, (void *)*addr)) || n != *size)
    return fail(u"cannot read the disk image", s);
  file->Close(file);
  root->Close(root);
  return EFI_SUCCESS;
}

static void find_framebuffer(struct bootinfo *bi)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

  if (bs->LocateProtocol(&gop_guid, 0, (void **)&gop) || !gop->Mode)
  {
    print(u"MDVMM: no graphics output, the screen stays dark\r\n");
    return;
  }
  info = gop->Mode->Info;
  if (gop->Mode->FrameBufferBase + gop->Mode->FrameBufferSize > 0x100000000ull ||
      (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
       info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor))
  {
    print(u"MDVMM: framebuffer above 4GB or not 32 bpp, not used\r\n");
    return;
  }
  bi->fb_phys = (uint32_t)gop->Mode->FrameBufferBase;
  bi->fb_size = (uint32_t)gop->Mode->FrameBufferSize;
  bi->fb_width = info->HorizontalResolution;
  bi->fb_height = info->VerticalResolution;
  bi->fb_pitch = info->PixelsPerScanLine;
  bi->fb_format = info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ? FB_RGBX : FB_BGRX;
}

/* usable conventional memory: contiguous usable pages from 0 (after
   ExitBootServices everything but runtime and reserved memory is ours) */
static uint32_t conv_kb(UINTN map_size, UINTN desc_size)
{
  uint64_t addr = 0x1000;               /* page 0 is often left out */
  int found = 1;
  UINTN i;

  while (found && addr < 0xa0000)
  {
    found = 0;
    for (i = 0; i + desc_size <= map_size; i += desc_size)
    {
      EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map_buf + i);
      uint64_t end = d->PhysicalStart + d->NumberOfPages * 4096;
      int usable = d->Type == EfiConventionalMemory || d->Type == EfiLoaderCode ||
                   d->Type == EfiLoaderData || d->Type == EfiBootServicesCode ||
                   d->Type == EfiBootServicesData;
      if (usable && d->PhysicalStart <= addr && end > addr)
      {
        addr = end;
        found = 1;
      }
    }
  }
  if (addr > 0xa0000)
    addr = 0xa0000;
  return (uint32_t)(addr / 1024);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
  struct bootinfo *bi;
  uint64_t rd_addr, rd_size, pool, low, core_size = (uint64_t)(core_image_end - core_image);
  uint32_t pool_mb, xms_mb, conv;
  uint64_t xms = 0;
  UINTN map_size, map_key, desc_size, i;
  uint32_t desc_ver;
  EFI_STATUS s;

  st = systab;
  bs = st->BootServices;
  bs->SetWatchdogTimer(0, 0, 0, 0);
  print(u"MDVMM - modernDOS virtual machine monitor (UEFI)\r\n");

  if (load_file(image, BOOT_IMAGE, &rd_addr, &rd_size))
    return 1;

  /* memory pool for the core: below 4GB */
  for (pool_mb = POOL_MB_MAX; pool_mb >= POOL_MB_MIN; pool_mb /= 2)
  {
    pool = 0xffffffffull;
    if (!bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, pool_mb * 256, &pool))
      break;
  }
  if (pool_mb < POOL_MB_MIN)
    return fail(u"not enough memory below 4GB", 1);
  /* extended memory for DOS (XMS, HMA): contiguous, below 4GB */
  for (xms_mb = XMS_MB_MAX; xms_mb >= XMS_MB_MIN; xms_mb /= 2)
  {
    xms = 0xffffffffull;
    if (!bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, xms_mb * 256, &xms))
      break;
  }
  if (xms_mb < XMS_MB_MIN)
    xms_mb = 0, xms = 0;
  /* one page in conventional memory: hand-over code, GDT, stack, bootinfo */
  low = 0x9ffff;
  if ((s = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, 1, &low)))
    return fail(u"no page below 640K", s);
  if (pool < 0x110000 || rd_addr < 0x110000 || (xms && xms < 0x110000))
    return fail(u"memory allocated in the DOS address space", 1);

  memset((void *)low, 0, 4096);
  bi = (struct bootinfo *)(low + 0x800);
  bi->magic = BOOTINFO_MAGIC;
  bi->version = BOOTINFO_VERSION;
  bi->boot_type = BOOT_UEFI;
  bi->pool_phys = (uint32_t)pool;
  bi->pool_size = pool_mb << 20;
  bi->core_size = (uint32_t)core_size;
  bi->ramdisk_phys = (uint32_t)rd_addr;
  bi->ramdisk_size = (uint32_t)rd_size;
  bi->xms_phys = (uint32_t)xms;
  bi->xms_size = xms_mb << 20;
  find_framebuffer(bi);
  for (i = 0; i < st->NumberOfTableEntries; i++)
    if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &acpi20_guid))
      bi->rsdp = (uint32_t)(uint64_t)st->ConfigurationTable[i].VendorTable;
  memcpy((void *)pool, core_image, core_size);

  print(u"Starting the monitor...\r\n");

  /* exit boot services; retry once if the map changed in between */
  for (i = 0; i < 2; i++)
  {
    map_size = sizeof(map_buf);
    if ((s = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf, &map_key, &desc_size, &desc_ver)))
      return fail(u"cannot get the memory map", s);
    conv = conv_kb(map_size, desc_size);
    if (!(s = bs->ExitBootServices(image, map_key)))
      break;
  }
  if (s)
    return fail(u"ExitBootServices failed", s);

  /* no firmware services from here on */
  bi->conv_kb = conv;
  enter_core(low, (uint64_t)bi, pool);
}
