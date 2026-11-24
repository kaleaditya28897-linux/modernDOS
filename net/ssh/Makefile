# modernDOS SSH: SSHD.EXE (DJGPP + Watt-32), and a Linux build of the same
# protocol code for tests against OpenSSH (make host)

DJ     = /opt/djgpp/bin/i586-pc-msdosdjgpp-
WATT   = /opt/watt32
B      = build
COMMON = ssh.c buf.c sha256.c rng.c console.c sftp.c \
         crypto/monocypher.c crypto/monocypher-ed25519.c
HDRS   = $(wildcard *.h crypto/*.h)
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter

all: $(B)/sshd.exe $(B)/ssh.exe

$(B)/sshd.exe: sshd.c plat_dos.c $(COMMON) $(HDRS) | $(B)
	$(DJ)gcc $(CFLAGS) -s -I$(WATT)/inc -o $@ sshd.c plat_dos.c $(COMMON) -L$(WATT)/lib -lwatt

$(B)/ssh.exe: sshc.c plat_dos.c term.c keyfile.c $(COMMON) $(HDRS) | $(B)
	$(DJ)gcc $(CFLAGS) -s -I$(WATT)/inc -o $@ sshc.c plat_dos.c term.c keyfile.c $(COMMON) -L$(WATT)/lib -lwatt

host: $(B)/sshd-host $(B)/ssh-host

$(B)/ssh-host: sshc.c plat_host.c term.c keyfile.c $(COMMON) $(HDRS) | $(B)
	gcc $(CFLAGS) -g -o $@ sshc.c plat_host.c term.c keyfile.c $(COMMON)

$(B)/sshd-host: sshd.c plat_host.c $(COMMON) $(HDRS) | $(B)
	gcc $(CFLAGS) -g -o $@ sshd.c plat_host.c $(COMMON)

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

.PHONY: all host clean
