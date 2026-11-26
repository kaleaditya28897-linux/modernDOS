/* VGA mode 13h, as DOS games use it (tests/compat): 320x200 in 256
   colours, pixels written to A000:0, the palette set through the DAC
   ports and through the BIOS. Built for real mode and for DOS/4GW. The
   upper half of the screen becomes red (colour 1, set through the ports),
   the lower half green (colour 2, set through INT 10h); "VGA: showing" on
   the debug port tells the test to take its picture. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

#ifdef __386__
#define SCREEN ((unsigned char *)0xA0000)
#define INT86 int386
#define RW(r) r.w
#else
#define SCREEN ((unsigned char far *)MK_FP(0xA000, 0))
#define INT86 int86
#define RW(r) r.x
#endif

static void say(const char *s)
{
  for (; *s; s++)
    outp(0xe9, *s);
}

static unsigned long ticks(void)
{
  union REGS r;

  memset(&r, 0, sizeof(r));
  INT86(0x1a, &r, &r);
  return ((unsigned long)RW(r).cx << 16) | RW(r).dx;
}

int main(void)
{
  union REGS r;
  unsigned long t, i;
  int ok = 1, mode, red, green, blue;

  memset(&r, 0, sizeof(r));
  RW(r).ax = 0x0013;
  INT86(0x10, &r, &r);
  memset(&r, 0, sizeof(r));
  r.h.ah = 0x0f;
  INT86(0x10, &r, &r);
  mode = r.h.al;

  outp(0x3c8, 1);                       /* colour 1: red, through the DAC */
  outp(0x3c9, 63);
  outp(0x3c9, 0);
  outp(0x3c9, 0);
  memset(&r, 0, sizeof(r));             /* colour 2: green, through the BIOS */
  RW(r).ax = 0x1010;
  RW(r).bx = 2;
  r.h.ch = 63;
  INT86(0x10, &r, &r);
  for (i = 0; i < 32000u; i++)
    SCREEN[i] = 1;
  for (i = 32000u; i < 64000u; i++)
    SCREEN[i] = 2;

  while (inp(0x3da) & 8)                /* a vertical retrace comes and goes */
    ;
  while (!(inp(0x3da) & 8))
    ;
  outp(0x3c7, 1);                       /* the palette reads back */
  red = inp(0x3c9);
  green = inp(0x3c9);
  blue = inp(0x3c9);
  if (red != 63 || green != 0 || blue != 0)
    ok = 0;
  if (SCREEN[100] != 1 || SCREEN[63999u] != 2)
    ok = 0;

  say(sizeof(int) == 4 ? "VGA: showing 32\r\n" : "VGA: showing 16\r\n");
  for (t = ticks(); ticks() - t < 55;)  /* three seconds */
    ;
  memset(&r, 0, sizeof(r));
  RW(r).ax = 0x0003;
  INT86(0x10, &r, &r);
  printf("vga: %u bit, mode %02Xh, palette %d/%d/%d: %s\n", (unsigned)(sizeof(int) * 8), mode, red, green, blue,
         ok && mode == 0x13 ? "all ok" : "FAILED");
  return !(ok && mode == 0x13);
}
