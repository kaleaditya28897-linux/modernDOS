# modernDOS networking, SSH, installation (milestone M6)

## The layers

| Layer | What | Where |
|---|---|---|
| Network cards | UEFI boots: **iPXE's drivers** built into the monitor (Marvell Yukon/Yukon-2, Intel 8254x/8257x/I210 and 8255x, Realtek 8139/8169/8111, Broadcom tg3 and b44, nForce, Atheros L1E, JMicron, VIA, SiS 190, AMD PCnet, NatSemi, 3c90x, virtio, vmxnet3). DOS boots: a small native Intel e1000 driver (MDVMM.COM has no room for more) | `mdvmm/ipxe/`, `mdvmm/core/ipxenic.c`, `e1000.c`, `nic.c` |
| Monitor calls | INT 2Fh AX=DE10h info, DE11h send, DE12h receive, DE13h driver state to the log | `mdvmm/core/nic.c` |
| Packet driver | `PKTDRV.COM`: a standard packet driver (class 1) on those calls | `mdvmm/pktdrv/` |
| TCP/IP | Watt-32 (DJGPP library): DHCP, DNS, BSD sockets; `WATTCP.CFG` | build image, `build/watt32.sh` |
| SSH | `SSHD.EXE` (shell, exec, sftp/scp), `SSH.EXE` (client with a terminal emulator) | `net/ssh/` |

### iPXE's drivers in the monitor

iPXE's drivers are written for a polled, single-threaded environment, and
its 32-bit memory model ("physical = virtual + a fixed offset") is true
for the monitor's pool. `mdvmm/ipxe/build.sh` compiles the drivers named
in `drivers.list` and the parts of iPXE's core in `core.list` with a small
platform configuration (`overlay/`), links them with `glue.c` into one
object and makes every name but `ipxe_*` local. The source tree is a
pinned snapshot in the build image (`build/ipxe.sh`). To add a driver: add
its file to `drivers.list`; the link step lists what else it needs.
`make -C mdvmm IPXE_DEBUG=sky2` sends that driver's debug messages to the
monitor's log (`MDLOG`).

## Using it

`NET` (NET.BAT) starts job slots for remote shells, a local prompt in
another session (Ctrl+Alt+F4), `PKTDRV` and `SSHD`. Logins need
`AUTHKEYS.TXT` (OpenSSH public keys) or a password (`SSHD /PASSWD`).
From another machine: `ssh dos@host`, `sftp`, `scp`; drives are `/A`, `/C`.
`MDLOG` shows the monitor's log, `MDLOG /N` with the network card's state.

A personal image has one's own keys built in:

    mkdir -p _output/personal
    head -c 32 /dev/urandom > _output/personal/HOSTKEY.ED    # the host key
    cp ~/.ssh/id_ed25519.pub _output/personal/AUTHKEYS.TXT
    scripts/mkiso.sh 0.0.2-dev-personal _output/personal

Never publish a personal image: it holds the host's private key.

## Installing on a machine's disk

A stick made from a personal image has `\MDOS\INSTALL`: at start the
loader offers to install (press Y). It copies `\EFI\BOOT\BOOTX64.EFI` and
`\MDOS` to a writable FAT volume of the machine, normally the EFI
system partition of its disk, with the firmware's own file system driver.
Nothing else on the disk is changed; another fallback loader is kept as
`BOOTX64.OLD`. On Apple firmware the loader also makes it the default
start entry (otherwise: hold Option, "EFI Boot").

Installed, the EFI system partition is DOS drive `C:`: the virtual BIOS
gives DOS a made-up MBR for GPT disks that lists their FAT partitions.
`C:\MDOS\SSH` keeps the SSH host key, `AUTHKEYS.TXT` and `WATTCP.CFG`
(copied there at the first start); the network and SSH server start by
themselves. `A:` stays a RAM disk loaded from `C:\MDOS\BOOT.IMG`.

## Updating a machine nobody sits at

    scripts/mdpush.sh <host>            # upload a build, restart into it as a trial
    scripts/mdpush.sh <host> status
    scripts/mdpush.sh <host> keep       # the trial becomes the installed build

A build is two files, `BOOTX64.EFI` and `BOOT.IMG`. They go to
`C:\MDOS\TRY`; `TRYBOOT` creates `\MDOS\TRYONCE` and restarts.
The installed loader removes that file and starts the loader in `TRY`,
once. A trial that is not confirmed (`KEEP`, or `MDSYS KEEP`) within ten
minutes restarts the machine: back into the installed build. So does a
monitor panic during a trial (after a minute). `KEEP` copies the trial's
files over the installed ones.

What this does not cover: a trial that hangs with interrupts off inside
the monitor, and power loss while `KEEP` copies (the stick is the rescue).

## One card, several sessions

The packet driver is loaded before the DOS sessions are forked, so every
session has it, and every program brings its own TCP/IP stack (Watt-32)
and gets the card's one address from DHCP. The monitor sorts the frames
(`mdvmm/core/nic.c`): TCP and UDP of a connection go to the session that
has sent on it, TCP to a port a session has accepted on goes there,
everything else (ARP, DHCP, ICMP, a first SYN) to every session whose
program polls. The answers stacks give to segments that were not theirs
(TCP resets, ICMP unreachable) are dropped on the way out. So the SSH
server, a browser and PKG work side by side. Limit: two sessions that
pick the same local port for the same server at the same time collide.

## Packages

`PKG` (`net/pkg/`, UEFI boot disk) installs packages from a repository on
a web server:

    PKG UPDATE            fetch the repository's package list
    PKG LIST              packages, [*] = installed
    PKG INSTALL name      download, check, unpack
    PKG REMOVE name

`PKG.CFG` (next to PKG.EXE, or in `%PKGDIR%`; `C:\MDOS` when installed)
names the repository, its public key and where packages go:

    repo = http://192.168.1.10:8080/
    key  = <64 hex digits>
    root = C:\

A repository may be `http://` or `https://`; trust comes from signatures
either way: `INDEX.TXT` (name,
version, file, size, SHA-256, description per package) is signed with the
repository's ed25519 key (`INDEX.SIG`); PKG uses a list only if the
signature fits the key in PKG.CFG, and a package only if its SHA-256 is
the one in the list. Packages are uncompressed tar archives; names that
would leave the install root are skipped. A repository is made with

    scripts/mkrepo.sh <packages> <repository>     # prints the public key
    python3 -m http.server 8080 -d <repository>

from a directory per package (`<name>/PKGINFO` with `version=` and
`description=`, the files under `<name>/root/`).

Tests: `tests/net`, `tests/ssh`, `tests/usb`, `tests/install`, `tests/pkg`
(all in `scripts/mdtest.sh`).
