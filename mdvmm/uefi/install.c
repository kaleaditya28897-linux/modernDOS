/* BOOTX64.EFI - installing on the internal disk, and trial boots
 *
 * Install: started from a stick that has \MDOS\INSTALL, the loader
 * offers to copy modernDOS to a writable FAT volume of the machine (the
 * EFI system partition of its disk): \EFI\BOOT\BOOTX64.EFI and the files
 * of \MDOS. Nothing else on that disk is touched. The firmware's own
 * file system driver does the writing. On Apple firmware the loader then
 * asks the firmware to start that file by default.
 *
 * Trial boots (updating a machine nobody sits at): a new build is put in
 * \MDOS\TRY (BOOTX64.EFI, BOOT.IMG) together with the file
 * \MDOS\TRYONCE. The installed loader removes TRYONCE and starts the
 * loader in TRY, once. That one boots TRY\BOOT.IMG and tells the monitor
 * it is a trial: unless the trial is confirmed (MDSYS KEEP), the monitor
 * restarts the machine after ten minutes, and the installed build is back.
 */
#include "efi.h"
#include "loader.h"

static EFI_GUID loaded_image_guid = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID simple_fs_guid = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID device_path_guid = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID apple_boot_guid = { 0x7c436110, 0xab2a, 0x4bbb, { 0xa8, 0x80, 0xfe, 0x41, 0x99, 0x5c, 0x9f, 0x82 } };

#define MODE_RW  (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE)
#define MODE_NEW (MODE_RW | EFI_FILE_MODE_CREATE)
#define ATTR_DIR 0x10ull

static EFI_BOOT_SERVICES *bs;

static EFI_FILE_PROTOCOL *volume(EFI_HANDLE dev)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root;

  if (bs->HandleProtocol(dev, &simple_fs_guid, (void **)&fs) || fs->OpenVolume(fs, &root))
    return 0;
  return root;
}

static int exists(EFI_FILE_PROTOCOL *root, const CHAR16 *name)
{
  EFI_FILE_PROTOCOL *f;

  if (root->Open(root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ, 0))
    return 0;
  f->Close(f);
  return 1;
}

static void remove(EFI_FILE_PROTOCOL *root, const CHAR16 *name)
{
  EFI_FILE_PROTOCOL *f;

  if (!root->Open(root, &f, (CHAR16 *)name, MODE_RW, 0))
    f->Delete(f);
}

/* a whole file into pages; 0 on success */
static EFI_STATUS read_file(EFI_FILE_PROTOCOL *root, const CHAR16 *name, uint64_t *addr, UINTN *size)
{
  EFI_FILE_PROTOCOL *f;
  EFI_STATUS s;
  uint64_t end;

  if ((s = root->Open(root, &f, (CHAR16 *)name, EFI_FILE_MODE_READ, 0)))
    return s;
  f->SetPosition(f, ~0ull);
  f->GetPosition(f, &end);
  f->SetPosition(f, 0);
  *size = end;
  *addr = 0xffffffffull;
  if (!(s = bs->AllocatePages(AllocateMaxAddress, EfiLoaderData, (end + 4095) / 4096 + 1, addr)))
    s = f->Read(f, size, (void *)*addr);
  f->Close(f);
  return s ? s : *size != end;
}

static EFI_STATUS write_file(EFI_FILE_PROTOCOL *root, const CHAR16 *name, const void *data, UINTN size)
{
  EFI_FILE_PROTOCOL *f;
  EFI_STATUS s;
  UINTN n = size;

  remove(root, name);
  if ((s = root->Open(root, &f, (CHAR16 *)name, MODE_NEW, 0)))
    return s;
  s = n ? f->Write(f, &n, (void *)data) : 0;
  f->Close(f);
  return s ? s : n != size;
}

static void make_dir(EFI_FILE_PROTOCOL *root, const CHAR16 *name)
{
  EFI_FILE_PROTOCOL *f;

  if (!root->Open(root, &f, (CHAR16 *)name, MODE_NEW, ATTR_DIR))
    f->Close(f);
}

/* ---------------------------------------------------------------- */

/* Started by the installed loader as a trial? (its load options say so) */
int is_trial(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  const CHAR16 *o;

  bs = st->BootServices;
  if (bs->HandleProtocol(image, &loaded_image_guid, (void **)&li) || li->LoadOptionsSize < 8)
    return 0;
  o = li->LoadOptions;
  return o && o[0] == 'T' && o[1] == 'R' && o[2] == 'Y' && o[3] == 0;
}

/* A trial waiting? Start it; only returns if there is none (or it failed) */
void trial_boot(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
  static CHAR16 options[] = u"TRY";
  EFI_LOADED_IMAGE_PROTOCOL *li, *li2;
  EFI_FILE_PROTOCOL *root;
  EFI_HANDLE child;
  uint64_t addr;
  UINTN size;
  EFI_STATUS s;

  bs = st->BootServices;
  if (is_trial(image, st) || bs->HandleProtocol(image, &loaded_image_guid, (void **)&li) ||
      !(root = volume(li->DeviceHandle)))
    return;
  if (!exists(root, u"\\MDOS\\TRYONCE"))
  {
    root->Close(root);
    return;
  }
  remove(root, u"\\MDOS\\TRYONCE");        /* once: the next boot is normal again */
  if (exists(root, u"\\MDOS\\TRYONCE"))
  {
    print(u"Trial boot: cannot remove \\MDOS\\TRYONCE (read-only volume): not started\r\n");
    root->Close(root);
    return;
  }
  print(u"Trial boot: starting \\MDOS\\TRY\\BOOTX64.EFI\r\n");
  s = read_file(root, u"\\MDOS\\TRY\\BOOTX64.EFI", &addr, &size);
  root->Close(root);
  if (!s)
    s = bs->LoadImage(0, image, 0, (void *)addr, size, &child);
  if (!s && !(s = bs->HandleProtocol(child, &loaded_image_guid, (void **)&li2)))
  {
    li2->DeviceHandle = li->DeviceHandle;       /* its files are on our volume */
    li2->LoadOptions = options;
    li2->LoadOptionsSize = sizeof(options);
    save_log();
    s = bs->StartImage(child, 0, 0);
  }
  print(u"Trial boot: failed (status ");
  print_hex(s);
  print(u"), starting the installed system\r\n");
}

/* ---------------------------------------------------------------- */

static int ask_yes(EFI_SYSTEM_TABLE *st)
{
  EFI_INPUT_KEY key;
  int i;

  for (i = 0; i < 6000; i++)            /* 60 seconds */
  {
    if (!st->ConIn->ReadKeyStroke(st->ConIn, &key))
      return key.UnicodeChar == 'y' || key.UnicodeChar == 'Y';
    bs->Stall(10000);
  }
  return 0;
}

static EFI_STATUS copy(EFI_FILE_PROTOCOL *from, EFI_FILE_PROTOCOL *to, const CHAR16 *src, const CHAR16 *dst)
{
  uint64_t addr;
  UINTN size;
  EFI_STATUS s = read_file(from, src, &addr, &size);

  if (!s)
  {
    s = write_file(to, dst, (void *)addr, size);
    bs->FreePages(addr, (size + 4095) / 4096 + 1);
  }
  print(s ? u"  FAILED " : u"  copied ");
  print(dst);
  print(u"\r\n");
  return s;
}

/* Apple firmware: start \EFI\BOOT\BOOTX64.EFI of that volume by default
   (what "bless --setBoot" does): the variable holds its device path */
static void apple_default(EFI_SYSTEM_TABLE *st, EFI_HANDLE dev)
{
  static const CHAR16 file[] = u"\\EFI\\BOOT\\BOOTX64.EFI";
  static uint8_t path[1024];
  EFI_DEVICE_PATH *dp, *n;
  UINTN len = 0, flen = sizeof(file);
  EFI_STATUS s;

  if (bs->HandleProtocol(dev, &device_path_guid, (void **)&dp))
    return;
  for (n = dp; n->Type != 0x7f; n = (EFI_DEVICE_PATH *)((uint8_t *)n + (n->Length[0] | n->Length[1] << 8)))
    if (!(n->Length[0] | n->Length[1] << 8))
      return;
  len = (UINTN)((uint8_t *)n - (uint8_t *)dp);
  if (len + 4 + flen + 4 > sizeof(path))
    return;
  memcpy(path, dp, len);
  path[len] = 4;                        /* media device path: file path */
  path[len + 1] = 4;
  path[len + 2] = (uint8_t)(4 + flen);
  path[len + 3] = (uint8_t)((4 + flen) >> 8);
  memcpy(path + len + 4, file, flen);
  len += 4 + flen;
  path[len] = 0x7f;                     /* end of the path */
  path[len + 1] = 0xff;
  path[len + 2] = 4;
  path[len + 3] = 0;
  len += 4;
  s = st->RuntimeServices->SetVariable(u"efi-boot-device-data", &apple_boot_guid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        len, path);
  print(s ? u"The firmware did not take the default start entry (hold Option at power-on, pick EFI Boot).\r\n"
          : u"Set as the firmware's default start entry.\r\n");
}

/* \MDOS\INSTALL on the boot volume: offer to install */
void install_offer(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
  EFI_LOADED_IMAGE_PROTOCOL *li;
  EFI_FILE_PROTOCOL *src, *dst = 0;
  EFI_HANDLE *handles, target = 0;
  UINTN count, i;
  int apple, bad = 0;

  bs = st->BootServices;
  if (bs->HandleProtocol(image, &loaded_image_guid, (void **)&li) || !(src = volume(li->DeviceHandle)))
    return;
  if (!exists(src, u"\\MDOS\\INSTALL") ||
      bs->LocateHandleBuffer(2 /* ByProtocol */, &simple_fs_guid, 0, &count, &handles))
  {
    src->Close(src);
    return;
  }
  /* the first other volume that can be written (FAT; firmware reads
     HFS+ and others only) */
  for (i = 0; i < count && !target; i++)
  {
    EFI_FILE_PROTOCOL *root, *f;
    if (handles[i] == li->DeviceHandle || !(root = volume(handles[i])))
      continue;
    if (!root->Open(root, &f, (CHAR16 *)u"\\MDTEST.TMP", MODE_NEW, 0))
    {
      f->Delete(f);
      target = handles[i];
      dst = root;
    }
    else
      root->Close(root);
  }
  if (!target)
  {
    print(u"Install: no writable FAT volume (EFI system partition) found on this machine.\r\n");
    src->Close(src);
    return;
  }
  print(u"\r\nINSTALL modernDOS on this machine's disk?\r\n"
        u"  It adds \\EFI\\BOOT\\BOOTX64.EFI and the folder \\MDOS to the EFI system\r\n"
        u"  partition");
  print(exists(dst, u"\\MDOS\\BOOT.IMG") ? u" (modernDOS is there already: it is replaced)" : u"");
  print(u". Nothing else on the disk is changed.\r\n"
        u"  Press Y to install, any other key to just start from the stick (60 s): ");
  if (!ask_yes(st))
  {
    print(u"no\r\n");
    goto out;
  }
  print(u"yes\r\n");
  make_dir(dst, u"\\EFI");
  make_dir(dst, u"\\EFI\\BOOT");
  make_dir(dst, u"\\MDOS");
  /* another system's fallback loader: keep it */
  if (exists(dst, u"\\EFI\\BOOT\\BOOTX64.EFI") && !exists(dst, u"\\MDOS\\BOOT.IMG") &&
      !exists(dst, u"\\EFI\\BOOT\\BOOTX64.OLD"))
    copy(dst, dst, u"\\EFI\\BOOT\\BOOTX64.EFI", u"\\EFI\\BOOT\\BOOTX64.OLD");
  bad |= copy(src, dst, u"\\MDOS\\BOOT.IMG", u"\\MDOS\\BOOT.IMG") != 0;
  bad |= copy(src, dst, u"\\EFI\\BOOT\\BOOTX64.EFI", u"\\EFI\\BOOT\\BOOTX64.EFI") != 0;
  if (exists(src, u"\\MDOS\\BOOTWAIT"))
    copy(src, dst, u"\\MDOS\\BOOTWAIT", u"\\MDOS\\BOOTWAIT");
  /* DOS may write to the disk: C: (this partition) keeps the settings */
  bad |= write_file(dst, u"\\MDOS\\DISKRW", "modernDOS may write to the hard disks\r\n", 38) != 0;
  remove(dst, u"\\MDOS\\TRYONCE");
  if (bad)
    print(u"Install: FAILED, the machine's disk may hold a part of modernDOS.\r\n");
  else
  {
    print(u"Installed.\r\n");
    apple = st->FirmwareVendor && st->FirmwareVendor[0] == 'A' && st->FirmwareVendor[1] == 'p';
    if (apple)
      apple_default(st, target);
    print(u"Remove the stick and switch the machine off and on to start from its disk.\r\n"
          u"Starting from the stick in 15 seconds...\r\n");
    save_log();
    bs->Stall(15000000);
  }
out:
  dst->Close(dst);
  src->Close(src);
  save_log();
}
