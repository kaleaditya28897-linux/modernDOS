# mdgui - modernDOS graphics library (32-bit DPMI programs, DJGPP)
# Builds build/mdgui.o: the library as one relocatable object (link it
# into a program like a .o; DJGPP's ar is broken in the build image).

DJGPP   ?= /opt/djgpp/bin/i586-pc-msdosdjgpp-
CC      = $(DJGPP)gcc
CFLAGS  = -O2 -Wall -Wextra -Werror -Ilib

B       = build
OBJS    = $(B)/gfx.o $(B)/draw.o $(B)/text.o $(B)/input.o $(B)/wm.o $(B)/font.o

all: $(B)/mdgui.o

$(B):
	mkdir -p $(B)

$(B)/%.o: lib/%.c lib/mdgui.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/font.o: ../mdvmm/core/font.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/mdgui.o: $(OBJS)
	$(CC) -r -nostdlib -o $@ $(OBJS)

clean:
	rm -rf $(B)

.PHONY: all clean
