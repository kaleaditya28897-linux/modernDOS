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
#define DISK_RW_FILE  u"\\MODERNDOS\\DISKRW"  /* present: disks writable */
#define BOOTWAIT_FILE u"\\MODERNDOS\\BOOTWAIT" /* present: the core's log stays a while */
#define FBPROBE_FILE  u"\\MODERNDOS\\FBPROBE"  /* present: probe the framebuffer (tests) */
#define LOG_FILE      u"\\MODERNDOS\\BOOTLOG.TXT"

extern const uint8_t core_image[], core_image_end[];
void enter_core(uint64_t low, uint64_t bootinfo, uint64_t pool) __attribute__((noreturn));
int hwinfo_write(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL *root);

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;
static uint8_t map_buf[32768];

static EFI_GUID loaded_image_guid = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID simple_fs_guid = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static EFI_GUID uga_guid = { 0x982c298b, 0xf4fa, 0x41cb, { 0xb8, 0x38, 0x77, 0xaa, 0x68, 0x8f, 0xb8, 0x39 } };
static EFI_GUID conctl_guid = { 0xf42f7782, 0x012e, 0x4c12, { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x04, 0xf7, 0x21 } };
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

/* everything printed is also kept for \MODERNDOS\BOOTLOG.TXT: on some
   machines (Apple) the firmware's text console may not be visible */
static char logbuf[6000];
static UINTN loglen;
static EFI_HANDLE img;

static void print(const CHAR16 *s)
{
  const CHAR16 *c;

  for (c = s; *c; c++)
    if (loglen < sizeof(logbuf))
      logbuf[loglen++] = *c < 128 ? (char)*c : '?';
  st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}

static void print_dec(uint64_t v)
{
  CHAR16 buf[24];
  int i = 23;

  buf[i] = 0;
  do
    buf[--i] = (CHAR16)('0' + v % 10);
  while (v /= 10);
  print(buf + i);
}

/* write the log so far to the boot volume (if it is writable) */
static void save_log(void)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root, *f;
  UINTN n = loglen;

  if (bs->HandleProtocol(img, &loaded_image_guid, (void **)&li) ||
      bs->HandleProtocol(li->DeviceHandle, &simple_fs_guid, (void **)&fs) ||
      fs->OpenVolume(fs, &root))
    return;
  if (!root->Open(root, &f, (CHAR16 *)LOG_FILE, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))
    f->Delete(f);
  if (!root->Open(root, &f, (CHAR16 *)LOG_FILE,
                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))
  {
    f->Write(f, &n, logbuf);
    f->Close(f);
  }
  root->Close(root);
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
  save_log();
  return s;
}

/* read the whole file into pages below 4GB, above the V86 address space */
/* does the file exist on the boot volume? */
static int file_exists(EFI_HANDLE image, const CHAR16 *name)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root, *file;

  if (bs->HandleProtocol(image, &loaded_image_guid, (void **)&li) ||
      bs->HandleProtocol(li->DeviceHandle, &simple_fs_guid, (void **)&fs) ||
      fs->OpenVolume(fs, &root))
    return 0;
  if (root->Open(root, &file, (CHAR16 *)name, EFI_FILE_MODE_READ, 0))
  {
    root->Close(root);
    return 0;
  }
  file->Close(file);
  root->Close(root);
  return 1;
}

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

/* ---------------------------------------------------------------- */
/* the framebuffer                                                  */

static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static uint32_t pci_rd(int bus, int dev, int fn, int reg)
{
  outl(0xcf8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (reg & 0xfc));
  return inl(0xcfc);
}

static void pci_wr(int bus, int dev, int fn, int reg, uint32_t v)
{
  outl(0xcf8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (reg & 0xfc));
  outl(0xcfc, v);
}

#define MARK0 0x00a1b2c3u
#define MARK1 0x00123456u
#define MARK2 0x00c3a1b2u
#define MARK3 0x00654321u
#define PROBE_MAX 0x1000000u            /* look at the first 16 MB of a BAR */

/* Where does the screen live in memory? For firmware that can only draw
   (UGA, or GOP without a framebuffer address): four marker pixels are
   drawn at (0,0) (1,0) (2,0) (0,1) and searched for in the memory BARs of
   the display controllers. That gives the address of pixel (0,0) and the
   length of a scan line. 1 if found. */
static int fb_probe(uint32_t width, uint64_t *base, uint32_t *pitch)
{
  int bus, dev, fn, bar;

  for (bus = 0; bus < 256; bus++)
    for (dev = 0; dev < 32; dev++)
      for (fn = 0; fn < 8; fn++)
      {
        uint32_t id = pci_rd(bus, dev, fn, 0), hdr;
        if ((id & 0xffff) == 0xffff)
        {
          if (fn == 0)
            break;
          continue;
        }
        hdr = pci_rd(bus, dev, fn, 0x0c);
        if ((pci_rd(bus, dev, fn, 8) >> 24) == 0x03 && ((hdr >> 16) & 0x7f) == 0)
          for (bar = 0; bar < 6; bar++)
          {
            uint32_t lo = pci_rd(bus, dev, fn, 0x10 + bar * 4), size, off;
            int is64 = (lo & 7) == 4;
            const volatile uint32_t *m;
            if (lo & 1)
              continue;                 /* I/O ports */
            if (is64 && pci_rd(bus, dev, fn, 0x14 + bar * 4))
            {
              bar++;
              continue;                 /* above 4GB: not usable for us */
            }
            if (!(lo & ~0xfu))
            {
              bar += is64;
              continue;
            }
            /* size of the BAR */
            __asm__ volatile("cli");
            pci_wr(bus, dev, fn, 0x10 + bar * 4, 0xffffffffu);
            size = ~(pci_rd(bus, dev, fn, 0x10 + bar * 4) & ~0xfu) + 1;
            pci_wr(bus, dev, fn, 0x10 + bar * 4, lo);
            __asm__ volatile("sti");
            bar += is64;
            if (size < 0x100000 || (size & (size - 1)))
              continue;                 /* registers, not a framebuffer */
            if (size > PROBE_MAX)
              size = PROBE_MAX;
            m = (const volatile uint32_t *)(uintptr_t)(lo & ~0xfu);
            for (off = 0; off + 16 <= size; off += 4)
              if ((m[off / 4] & 0xffffff) == MARK0 && (m[off / 4 + 1] & 0xffffff) == MARK1 &&
                  (m[off / 4 + 2] & 0xffffff) == MARK3)
              {
                uint32_t p;
                for (p = width; p <= width + 1024 && off + p * 4 + 4 <= size; p++)
                  if ((m[off / 4 + p] & 0xffffff) == MARK2)
                  {
                    *base = (uint64_t)(lo & ~0xfu) + off;
                    *pitch = p;
                    return 1;
                  }
              }
          }
        if (fn == 0 && !(hdr & 0x00800000))
          break;
      }
  return 0;
}

static void log_fb(const CHAR16 *how, uint64_t base, uint32_t w, uint32_t h, uint32_t pitch)
{
  print(how);
  print(u": ");
  print_dec(w);
  print(u"x");
  print_dec(h);
  print(u", framebuffer at ");
  print_hex(base);
  print(u", ");
  print_dec(pitch);
  print(u" pixels per line\r\n");
}

static void find_framebuffer(struct bootinfo *bi, int probe_test)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
  EFI_UGA_DRAW_PROTOCOL *uga;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
  uint32_t mark[4] = { MARK0, MARK1, MARK3, MARK2 }, pitch = 0;
  uint64_t base = 0;

  if (!bs->LocateProtocol(&gop_guid, 0, (void **)&gop) && gop->Mode)
  {
    info = gop->Mode->Info;
    log_fb(u"Graphics (GOP)", gop->Mode->FrameBufferBase, info->HorizontalResolution,
           info->VerticalResolution, info->PixelsPerScanLine);
    if (probe_test)                     /* tests: the probe must agree with GOP */
    {
      gop->Blt(gop, &mark[0], 0, 0, 0, 0, 0, 1, 1, 0);
      gop->Blt(gop, &mark[1], 0, 0, 0, 1, 0, 1, 1, 0);
      gop->Blt(gop, &mark[2], 0, 0, 0, 2, 0, 1, 1, 0);
      gop->Blt(gop, &mark[3], 0, 0, 0, 0, 1, 1, 1, 0);
      if (fb_probe(info->HorizontalResolution, &base, &pitch))
        log_fb(u"Framebuffer probe", base, info->HorizontalResolution, info->VerticalResolution, pitch);
      else
        print(u"Framebuffer probe: not found\r\n");
    }
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
    return;
  }
  /* EFI 1.1 graphics (older Apple firmware): UGA can draw but does not
     say where the framebuffer is */
  if (!bs->LocateProtocol(&uga_guid, 0, (void **)&uga))
  {
    uint32_t w = 0, h = 0, depth = 0, refresh = 0;
    if (uga->GetMode(uga, &w, &h, &depth, &refresh) || !w || !h)
    {
      print(u"MDVMM: UGA graphics without a mode, the screen stays dark\r\n");
      return;
    }
    uga->Blt(uga, &mark[0], 0, 0, 0, 0, 0, 1, 1, 0);
    uga->Blt(uga, &mark[1], 0, 0, 0, 1, 0, 1, 1, 0);
    uga->Blt(uga, &mark[2], 0, 0, 0, 2, 0, 1, 1, 0);
    uga->Blt(uga, &mark[3], 0, 0, 0, 0, 1, 1, 1, 0);
    if (depth != 32 || !fb_probe(w, &base, &pitch))
    {
      print(u"MDVMM: UGA graphics, ");
      print_dec(w);
      print(u"x");
      print_dec(h);
      print(u" depth ");
      print_dec(depth);
      print(u": framebuffer not found, the screen stays dark\r\n");
      return;
    }
    log_fb(u"Graphics (UGA, framebuffer found by probing)", base, w, h, pitch);
    bi->fb_phys = (uint32_t)base;
    bi->fb_size = pitch * 4 * h;
    bi->fb_width = w;
    bi->fb_height = h;
    bi->fb_pitch = pitch;
    bi->fb_format = FB_BGRX;
    return;
  }
  print(u"MDVMM: no graphics output (no GOP, no UGA), the screen stays dark\r\n");
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
  img = image;
  bs->SetWatchdogTimer(0, 0, 0, 0);
  {
    /* Apple firmware: show the text console (it starts in graphics mode) */
    EFI_CONSOLE_CONTROL_PROTOCOL *cc;
    if (!bs->LocateProtocol(&conctl_guid, 0, (void **)&cc))
      cc->SetMode(cc, 0);
  }
  print(u"MDVMM - modernDOS virtual machine monitor (UEFI)\r\n");
  print(u"Firmware: ");
  print(st->FirmwareVendor ? st->FirmwareVendor : u"?");
  print(u", EFI ");
  print_dec(st->Hdr.Revision >> 16);
  print(u".");
  print_dec(st->Hdr.Revision & 0xffff);
  print(u"\r\n");

  /* the hardware report, first: it is most useful when something fails */
  {
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root;
    if (!bs->HandleProtocol(image, &loaded_image_guid, (void **)&li) &&
        !bs->HandleProtocol(li->DeviceHandle, &simple_fs_guid, (void **)&fs) &&
        !fs->OpenVolume(fs, &root))
    {
      if (!hwinfo_write(image, st, root))
        print(u"Hardware report written to \\MODERNDOS\\HWINFO.TXT\r\n");
      else
        print(u"Hardware report not written (read-only boot medium?)\r\n");
      root->Close(root);
    }
    else
      print(u"Hardware report not written: cannot open the boot volume\r\n");
  }
  save_log();

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
  print(u"Memory: disk image at ");
  print_hex(rd_addr);
  print(u", monitor pool ");
  print_dec(pool_mb);
  print(u" MB at ");
  print_hex(pool);
  print(u", DOS extended memory ");
  print_dec(xms_mb);
  print(u" MB at ");
  print_hex(xms);
  print(u"\r\n");

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
  if (file_exists(image, DISK_RW_FILE))
    bi->flags |= BOOTF_DISK_WRITE;
  else
    print(u"Hard disks are read-only (no \\MODERNDOS\\DISKRW).\r\n");
  bi->xms_phys = (uint32_t)xms;
  bi->xms_size = xms_mb << 20;
  if (file_exists(image, BOOTWAIT_FILE))
  {
    bi->flags |= BOOTF_LOG_PAUSE;
    print(u"The monitor's log will stay on the screen for a while (\\MODERNDOS\\BOOTWAIT).\r\n");
  }
  find_framebuffer(bi, file_exists(image, FBPROBE_FILE));
  for (i = 0; i < st->NumberOfTableEntries; i++)
    if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &acpi20_guid))
      bi->rsdp = (uint32_t)(uint64_t)st->ConfigurationTable[i].VendorTable;
  memcpy((void *)pool, core_image, core_size);

  print(u"Starting the monitor (leaving the firmware's boot services)...\r\n");
  save_log();

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
