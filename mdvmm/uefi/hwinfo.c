/* BOOTX64.EFI - hardware report
 *
 * Before the monitor starts, the loader writes \MODERNDOS\HWINFO.TXT to
 * the boot volume (if it is writable): the firmware, the model (SMBIOS),
 * the CPU, graphics (GOP and UGA), every PCI device with its BARs, the
 * EFI configuration tables and the memory map. It is how drivers for a
 * real machine get written without access to it.
 */
#include "efi.h"

#define REPORT_MAX  65536

static char *rep;
static UINTN len;

static void put(const char *s)
{
  while (*s && len < REPORT_MAX - 1)
    rep[len++] = *s++;
}

static void putc_(char c)
{
  if (len < REPORT_MAX - 1)
    rep[len++] = c;
}

static void hex(uint64_t v, int digits)
{
  int i;
  for (i = digits - 1; i >= 0; i--)
    putc_("0123456789abcdef"[(v >> (i * 4)) & 15]);
}

static void dec(uint64_t v)
{
  char b[24];
  int i = 0;
  do
    b[i++] = (char)('0' + v % 10);
  while ((v /= 10) && i < 23);
  while (i)
    putc_(b[--i]);
}

static void put16(const CHAR16 *s)
{
  for (; s && *s; s++)
    putc_(*s < 128 ? (char)*s : '?');
}

static void nl(void)
{
  put("\r\n");
}

static void guid(const EFI_GUID *g)
{
  int i;
  hex(g->d1, 8); putc_('-'); hex(g->d2, 4); putc_('-'); hex(g->d3, 4); putc_('-');
  for (i = 0; i < 8; i++)
  {
    if (i == 2)
      putc_('-');
    hex(g->d4[i], 2);
  }
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

/* ---------------------------------------------------------------- */

static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static uint32_t pci(int bus, int dev, int fn, int reg)
{
  outl(0xcf8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (reg & 0xfc));
  return inl(0xcfc);
}

static void pci_devices(void)
{
  int bus, dev, fn, i;

  put("PCI devices (bus:dev.fn vendor:device class rev subsystem)"); nl();
  for (bus = 0; bus < 256; bus++)
    for (dev = 0; dev < 32; dev++)
      for (fn = 0; fn < 8; fn++)
      {
        uint32_t id = pci(bus, dev, fn, 0), cls, hdr;
        if ((id & 0xffff) == 0xffff)
        {
          if (fn == 0)
            break;
          continue;
        }
        cls = pci(bus, dev, fn, 8);
        hdr = pci(bus, dev, fn, 0x0c);
        put("  "); hex(bus, 2); putc_(':'); hex(dev, 2); putc_('.'); hex(fn, 1);
        put("  "); hex(id & 0xffff, 4); putc_(':'); hex(id >> 16, 4);
        put("  class "); hex(cls >> 8, 6); put(" rev "); hex(cls & 0xff, 2);
        if (((hdr >> 16) & 0x7f) == 0)
        {
          uint32_t sub = pci(bus, dev, fn, 0x2c);
          put("  sub "); hex(sub & 0xffff, 4); putc_(':'); hex(sub >> 16, 4);
          nl();
          put("      BARs");
          for (i = 0; i < 6; i++)
          {
            putc_(' ');
            hex(pci(bus, dev, fn, 0x10 + i * 4), 8);
          }
          put("  irq "); dec(pci(bus, dev, fn, 0x3c) & 0xff);
        }
        nl();
        if (fn == 0 && !(hdr & 0x00800000))
          break;
      }
}

static void cpu(void)
{
  uint32_t r[12], a, b, c, d;
  int i;

  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000));
  put("CPU: ");
  if (a >= 0x80000004)
  {
    for (i = 0; i < 3; i++)
      __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]),
                       "=d"(r[i * 4 + 3]) : "a"(0x80000002 + i));
    for (i = 0; i < 48; i++)
      if (((char *)r)[i])
        putc_(((char *)r)[i]);
  }
  __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
  put("  (cpuid 1: eax "); hex(a, 8); put(" edx "); hex(d, 8); put(" ecx "); hex(c, 8); put(")");
  nl();
}

static UINTN slen(const char *p)
{
  UINTN n = 0;
  while (p[n])
    n++;
  return n;
}

/* SMBIOS 2.x: string n of a structure */
static const char *sm_string(const uint8_t *s, int n)
{
  const char *p = (const char *)s + s[1];
  if (!n)
    return "";
  while (--n && *p)
    p += slen(p) + 1;
  return p;
}

static void smbios(EFI_SYSTEM_TABLE *st)
{
  static EFI_GUID sm_guid = { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
  const uint8_t *ep = 0, *t, *end;
  UINTN i;
  int n;

  for (i = 0; i < st->NumberOfTableEntries; i++)
    if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &sm_guid))
      ep = st->ConfigurationTable[i].VendorTable;
  if (!ep || ep[0] != '_' || ep[1] != 'S' || ep[2] != 'M')
  {
    put("SMBIOS: none"); nl();
    return;
  }
  t = (const uint8_t *)(uintptr_t)*(const uint32_t *)(ep + 0x18);
  end = t + *(const uint16_t *)(ep + 0x16);
  for (n = 0; t + 4 <= end && n < 256; n++)
  {
    const uint8_t *next = t + t[1];
    if (t[0] == 0)
    {
      put("BIOS: "); put(sm_string(t, t[4])); put(", "); put(sm_string(t, t[5]));
      put(", "); put(sm_string(t, t[8])); nl();
    }
    else if (t[0] == 1)
    {
      put("System: "); put(sm_string(t, t[4])); put(" / "); put(sm_string(t, t[5]));
      put(" / "); put(sm_string(t, t[6])); nl();
    }
    else if (t[0] == 2)
    {
      put("Board: "); put(sm_string(t, t[4])); put(" / "); put(sm_string(t, t[5])); nl();
    }
    else if (t[0] == 127)
      break;
    while (next + 1 < end && (next[0] || next[1]))   /* skip the strings */
      next++;
    t = next + 2;
  }
}

static void graphics(EFI_BOOT_SERVICES *bs)
{
  static EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
  static EFI_GUID uga_guid = { 0x982c298b, 0xf4fa, 0x41cb, { 0xb8, 0x38, 0x77, 0xaa, 0x68, 0x8f, 0xb8, 0x39 } };
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
  EFI_UGA_DRAW_PROTOCOL *uga;
  uint32_t m;

  if (!bs->LocateProtocol(&gop_guid, 0, (void **)&gop) && gop->Mode)
  {
    put("GOP: mode "); dec(gop->Mode->Mode); put(" of "); dec(gop->Mode->MaxMode);
    put(", framebuffer "); hex(gop->Mode->FrameBufferBase, 16); put(" size ");
    hex(gop->Mode->FrameBufferSize, 8); nl();
    for (m = 0; m < gop->Mode->MaxMode && m < 32; m++)
    {
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
      UINTN size;
      if (gop->QueryMode(gop, m, &size, &info))
        continue;
      put("  mode "); dec(m); put(": "); dec(info->HorizontalResolution); putc_('x');
      dec(info->VerticalResolution); put(" format "); dec(info->PixelFormat);
      put(" pitch "); dec(info->PixelsPerScanLine); nl();
    }
  }
  else
  {
    put("GOP: none"); nl();
  }
  if (!bs->LocateProtocol(&uga_guid, 0, (void **)&uga))
  {
    uint32_t w = 0, h = 0, d = 0, r = 0;
    put("UGA: present");
    if (!uga->GetMode(uga, &w, &h, &d, &r))
    {
      put(", "); dec(w); putc_('x'); dec(h); put(" depth "); dec(d); put(" refresh "); dec(r);
    }
    nl();
  }
  else
  {
    put("UGA: none"); nl();
  }
}

static void memory(EFI_BOOT_SERVICES *bs)
{
  static uint8_t map[32768];
  UINTN size = sizeof(map), key, dsize, i;
  uint32_t ver;
  uint64_t total = 0;

  if (bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)map, &key, &dsize, &ver))
  {
    put("Memory map: unavailable"); nl();
    return;
  }
  put("Memory map (type start pages attributes)"); nl();
  for (i = 0; i + dsize <= size; i += dsize)
  {
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + i);
    put("  "); dec(d->Type); put(" "); hex(d->PhysicalStart, 12); put(" "); dec(d->NumberOfPages);
    put(" "); hex(d->Attribute, 16); nl();
    if (d->Type == EfiConventionalMemory || d->Type == EfiBootServicesCode ||
        d->Type == EfiBootServicesData || d->Type == EfiLoaderCode || d->Type == EfiLoaderData)
      total += d->NumberOfPages * 4096;
  }
  put("Usable memory: "); dec(total >> 20); put(" MB"); nl();
}

/* (re)write the report so far; 0 on success */
static int save(EFI_FILE_PROTOCOL *root)
{
  EFI_FILE_PROTOCOL *f;
  UINTN n = len;
  EFI_STATUS s;

  if (!root->Open(root, &f, (CHAR16 *)u"\\MODERNDOS\\HWINFO.TXT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))
    f->Delete(f);
  if (root->Open(root, &f, (CHAR16 *)u"\\MODERNDOS\\HWINFO.TXT",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))
    return -1;
  s = f->Write(f, &n, rep);
  f->Close(f);
  return s || n != len ? -1 : 0;
}

/* write the report to \MODERNDOS\HWINFO.TXT; 0 on success. It is saved
   after every section, the most useful ones first: if a section hangs
   on some firmware, the rest is on the disk already. */
int hwinfo_write(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL *root)
{
  EFI_BOOT_SERVICES *bs = st->BootServices;
  uint64_t buf = 0xffffffffull;
  UINTN i;

  (void)image;
  if (bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, REPORT_MAX / 4096, &buf))
    return -1;
  rep = (char *)(uintptr_t)buf;
  len = 0;
  put("modernDOS hardware report (BOOTX64.EFI)"); nl();
  put("Firmware: "); put16(st->FirmwareVendor); put(" revision "); hex(st->FirmwareRevision, 8);
  put(", EFI "); dec(st->Hdr.Revision >> 16); putc_('.'); dec((st->Hdr.Revision & 0xffff) / 10);
  nl();
  if (save(root))
    return -1;                          /* read-only medium */
  pci_devices();
  save(root);
  graphics(bs);
  save(root);
  cpu();
  smbios(st);
  save(root);
  put("EFI configuration tables"); nl();
  for (i = 0; i < st->NumberOfTableEntries; i++)
  {
    put("  "); guid(&st->ConfigurationTable[i].VendorGuid); nl();
  }
  memory(bs);
  put("End of report"); nl();
  return save(root);
}
