# modernDOS package manager: PKG.EXE (DJGPP + Watt-32); for the build host:
# pkg-host (the same code, tests) and pkgsign (repository keys, mkrepo.sh)

DJ     = /opt/djgpp/bin/i586-pc-msdosdjgpp-
WATT   = /opt/watt32
B      = build
SRC    = pkg.c http.c ../ssh/sha256.c ../ssh/crypto/monocypher.c ../ssh/crypto/monocypher-ed25519.c
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

all: $(B)/pkg.exe

$(B)/pkg.exe: $(SRC) http.h | $(B)
	$(DJ)gcc $(CFLAGS) -s -I$(WATT)/inc -o $@ $(SRC) -L$(WATT)/lib -lwatt

host: $(B)/pkg-host $(B)/pkgsign

$(B)/pkg-host: $(SRC) http.h | $(B)
	gcc $(CFLAGS) -g -o $@ $(SRC)

$(B)/pkgsign: pkgsign.c | $(B)
	gcc $(CFLAGS) -o $@ pkgsign.c ../ssh/crypto/monocypher.c ../ssh/crypto/monocypher-ed25519.c

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

.PHONY: all host clean
