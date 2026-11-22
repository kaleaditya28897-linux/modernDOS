/* MDVMM core - USB keyboards (HID boot protocol) as a PC keyboard
 *
 * An 8-byte boot report holds the modifier keys (bit map) and up to six
 * pressed keys (HID usage IDs). Comparing a report with the previous one
 * gives key presses and releases, which become scancode set 1 make and
 * break codes for vm_key(): DOS sees the keystrokes as from the PS/2
 * controller (virtual IRQ1, scancode at port 60h). USB keyboards do not
 * repeat keys themselves, so the last key pressed repeats here, on timer
 * ticks (about 500 ms delay, then 9 per second).
 */
#include "core.h"

#define REPEAT_DELAY  9                 /* ticks (18.2 Hz) */
#define REPEAT_RATE   2

/* HID usage 00h-64h -> scancode set 1 (E0xxh: extended key, 0: none) */
static const uint16_t usage_sc[0x65] = {
  /* 00 */ 0, 0, 0, 0,
  /* 04 a-z */ 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
               0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f,
               0x11, 0x2d, 0x15, 0x2c,
  /* 1e 1-0 */ 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
  /* 28 */ 0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c, 0x0d, 0x1a, 0x1b, 0x2b,
  /* 32 */ 0x2b, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3a,
  /* 3a F1-F12 */ 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
  /* 46 PrtSc ScrLk Pause Ins Home PgUp Del End PgDn Right Left Down Up */
           0xe037, 0x46, 0, 0xe052, 0xe047, 0xe049, 0xe053, 0xe04f, 0xe051,
           0xe04d, 0xe04b, 0xe050, 0xe048,
  /* 53 NumLk KP/ KP* KP- KP+ KPEnter */ 0x45, 0xe035, 0x37, 0x4a, 0x4e, 0xe01c,
  /* 59 KP1-9 */ 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49,
  /* 62 KP0 KP. non-US \ */ 0x52, 0x53, 0x56,
};

/* modifier bits 0-7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint16_t mod_sc[8] = { 0x1d, 0x2a, 0x38, 0xe05b, 0xe01d, 0x36, 0xe038, 0xe05c };

static uint8_t last[8];
static uint16_t repeat_sc;
static uint32_t repeat_ticks;
static int logged;

static void send(uint16_t sc, int release)
{
  if (!sc)
    return;
  if (sc >> 8)
    vm_key(0xe0);
  vm_key((uint8_t)(sc | (release ? 0x80 : 0)));
}

static int has(const uint8_t *r, uint8_t key)
{
  int i;
  for (i = 2; i < 8; i++)
    if (r[i] == key)
      return 1;
  return 0;
}

static uint16_t key_sc(uint8_t usage)
{
  return usage < sizeof(usage_sc) / sizeof(usage_sc[0]) ? usage_sc[usage] : 0;
}

void usbkbd_report(const uint8_t *r)
{
  uint8_t changed = r[0] ^ last[0];
  int i;

  if (r[2] == 0x01)
    return;                             /* phantom state (rollover) */
  if (!logged)
  {
    kprintf("usbkbd: first key report\n");
    logged = 1;
  }
  for (i = 0; i < 8; i++)
    if (changed & (1 << i))
      send(mod_sc[i], !(r[0] & (1 << i)));
  for (i = 2; i < 8; i++)               /* releases */
    if (last[i] > 3 && !has(r, last[i]))
    {
      send(key_sc(last[i]), 1);
      if (key_sc(last[i]) == repeat_sc)
        repeat_sc = 0;
    }
  for (i = 2; i < 8; i++)               /* presses */
    if (r[i] > 3 && !has(last, r[i]))
    {
      uint16_t sc = key_sc(r[i]);
      send(sc, 0);
      if (sc != 0x3a && sc != 0x45 && sc != 0x46)   /* locks do not repeat */
      {
        repeat_sc = sc;
        repeat_ticks = 0;
      }
    }
  memcpy(last, r, 8);
}

/* timer tick: typematic repeat of the last key held down */
void usbkbd_tick(void)
{
  if (!repeat_sc)
    return;
  if (++repeat_ticks >= REPEAT_DELAY && (repeat_ticks - REPEAT_DELAY) % REPEAT_RATE == 0)
    send(repeat_sc, 0);
}
