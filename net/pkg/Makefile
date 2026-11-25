# modernDOS package manager: PKG.EXE (DJGPP + Watt-32); for the build host:
# pkg-host (the same code, tests) and pkgsign (repository keys, mkrepo.sh)

DJ     = /opt/djgpp/bin/i586-pc-msdosdjgpp-
WATT   = /opt/watt32
BEAR  ?= $(firstword $(wildcard /opt/bearssl ../../_output/ext/bearssl))
B      = build
SRC    = pkg.c http.c tls.c ../ssh/sha256.c ../ssh/crypto/monocypher.c ../ssh/crypto/monocypher-ed25519.c
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

all: $(B)/pkg.exe $(B)/httpget.exe

# HTTPGET: a test of the HTTP client
$(B)/httpget.exe: httpget.c http.c tls.c http.h tls.h | $(B)
	$(DJ)gcc $(CFLAGS) -s -I$(WATT)/inc -I$(BEAR)/inc -I$(BEAR) -o $@ httpget.c http.c tls.c ../ssh/rng.c \
	  ../ssh/crypto/monocypher.c -L$(WATT)/lib -lwatt $(BEAR)/lib/libbearssl.a

$(B)/pkg.exe: $(SRC) http.h tls.h | $(B)
	$(DJ)gcc $(CFLAGS) -s -I$(WATT)/inc -I$(BEAR)/inc -I$(BEAR) -o $@ $(SRC) ../ssh/rng.c \
	  -L$(WATT)/lib -lwatt $(BEAR)/lib/libbearssl.a

host: $(B)/pkg-host $(B)/pkgsign

$(B)/pkg-host: $(SRC) http.h tls.h | $(B)
	gcc $(CFLAGS) -g -I$(BEAR)/inc -I$(BEAR) -o $@ $(SRC) $(BEAR)/lib/host/libbearssl.a

$(B)/pkgsign: pkgsign.c | $(B)
	gcc $(CFLAGS) -o $@ pkgsign.c ../ssh/crypto/monocypher.c ../ssh/crypto/monocypher-ed25519.c

$(B):
	mkdir -p $@

clean:
	rm -rf $(B)

.PHONY: all host clean
