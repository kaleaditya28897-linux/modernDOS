; PKTDRV.COM - packet driver for the MDVMM network cards
;
; A Packet Driver Specification 1.11 driver (class 1, Ethernet) on top of
; the monitor's network API (INT 2Fh AX=DE10h-DE12h, mdvmm/core/nic.c), so
; DOS network software (Watt-32, mTCP, ...) works with the cards MDVMM has
; native drivers for, on DOS and UEFI boots.
;
;   PKTDRV [vector]      vector: 60..80 (hex), default 60
;
; Frames are fetched from the monitor on every timer tick (INT 08h), after
; every send and when DOS is idle (INT 28h), and handed to the receivers
; with the usual two upcalls (AX=0: get a buffer, AX=1: it is filled).

        cpu     386
        bits    16
        org     100h

MAX_HANDLES     equ 8
FRAME_MAX       equ 1514
H_SIZE          equ 16                  ; used, typelen, type[8], receiver(4), pad

start:  jmp     init

; ---------------------------------------------------------------------
; resident part
; ---------------------------------------------------------------------
pkt_entry:
        jmp     short pkt_dispatch
        nop
        db      'PKT DRVR', 0           ; signature at entry + 3
pkt_dispatch:
        cmp     ah, 1
        je      f_info
        cmp     ah, 2
        je      f_access
        cmp     ah, 3
        je      f_release
        cmp     ah, 4
        je      f_send
        cmp     ah, 5
        je      f_terminate
        cmp     ah, 6
        je      f_address
        cmp     ah, 7
        je      ok
        cmp     ah, 20
        je      ok
        cmp     ah, 21
        je      f_getmode
        cmp     ah, 24
        je      f_stats
        mov     dh, 11                  ; BAD_COMMAND
err:    stc
        retf    2
ok:     clc
        retf    2

f_info:                                 ; AL = FFh
        mov     bx, 1                   ; version
        mov     cx, 0100h               ; CH = class 1 (DIX Ethernet), CL = 0
        mov     dx, 0ffffh              ; type: generic
        push    cs
        pop     ds
        mov     si, drv_name
        mov     al, 1                   ; basic functions
        clc
        retf    2

f_access:                               ; AL class, BX type, DL number,
        cmp     al, 1                   ; DS:SI type, CX typelen, ES:DI receiver
        jne     .noclass
        cmp     cx, 8
        ja      .badtype
        push    bx
        push    di
        mov     bx, handles
        xor     di, di
.find:  cmp     byte [cs:bx], 0
        je      .free
        add     bx, H_SIZE
        inc     di
        cmp     di, MAX_HANDLES
        jb      .find
        pop     di
        pop     bx
        mov     dh, 9                   ; NO_SPACE
        jmp     err
.free:  mov     byte [cs:bx], 1
        mov     [cs:bx + 1], cl
        push    cx
        push    si
.copy:  jcxz    .copied
        lodsb
        mov     [cs:bx + 2], al
        inc     bx
        dec     cx
        jmp     .copy
.copied:
        pop     si
        pop     cx
        sub     bx, cx                  ; back to the entry
        mov     ax, di                  ; handle = index + 1
        inc     ax
        pop     di
        mov     [cs:bx + 10], di        ; receiver
        mov     [cs:bx + 12], es
        pop     bx
        clc
        retf    2
.noclass:
        mov     dh, 2                   ; NO_CLASS
        jmp     err
.badtype:
        mov     dh, 5                   ; BAD_TYPE
        jmp     err

; BX = handle -> BX = entry address, CF if bad
handle_entry:
        dec     bx
        cmp     bx, MAX_HANDLES
        jae     .bad
        shl     bx, 4                   ; * H_SIZE
        add     bx, handles
        cmp     byte [cs:bx], 0
        je      .bad
        clc
        ret
.bad:   stc
        ret

f_release:
f_terminate:
        call    handle_entry
        jc      .bad
        mov     byte [cs:bx], 0
        clc
        retf    2
.bad:   mov     dh, 1                   ; BAD_HANDLE
        jmp     err

f_send:                                 ; DS:SI frame, CX length
        push    ax
        mov     ax, 0de11h
        int     2fh
        pop     ax
        jc      .fail
        call    poll
        clc
        retf    2
.fail:  mov     dh, 12                  ; CANT_SEND
        jmp     err

f_address:                              ; ES:DI buffer, CX room -> CX = 6
        cmp     cx, 6
        jb      .small
        push    si
        push    ds
        push    cs
        pop     ds
        mov     si, mac
        mov     cx, 6
        cld
        rep     movsb
        sub     di, 6
        mov     cx, 6
        pop     ds
        pop     si
        clc
        retf    2
.small: mov     dh, 14                  ; NO_SPACE (buffer)
        jmp     err

f_getmode:
        mov     ax, 3                   ; own address and broadcasts
        clc
        retf    2

f_stats:
        push    cs
        pop     ds
        mov     si, stats
        clc
        retf    2

; ---------------------------------------------------------------------
; receiving
; ---------------------------------------------------------------------
int08:
        pushf
        call    far [cs:old08]
        call    poll
        iret

int28:
        call    poll
        jmp     far [cs:old28]

; fetch frames from the monitor and hand them to the receivers
poll:
        cmp     byte [cs:busy], 0
        jne     .ret
        ; no receiver here: leave the frames to the sessions that have one
        push    bx
        push    cx
        mov     bx, handles
        mov     cx, MAX_HANDLES
.used:  cmp     byte [cs:bx], 0
        jne     .some
        add     bx, H_SIZE
        loop    .used
        pop     cx
        pop     bx
        ret
.some:  pop     cx
        pop     bx
        mov     byte [cs:busy], 1
        pushad
        push    ds
        push    es
        mov     byte [cs:frames], 8     ; at most 8 per call
.next:  push    cs
        pop     es
        mov     di, rxbuf
        mov     cx, FRAME_MAX
        mov     ax, 0de12h
        int     2fh
        jc      .done
        jcxz    .done
        mov     [cs:rxlen], cx
        call    deliver
        dec     byte [cs:frames]
        jnz     .next
.done:  pop     es
        pop     ds
        popad
        mov     byte [cs:busy], 0
.ret:   ret

; the frame in rxbuf (rxlen bytes) to the first handle whose type matches
deliver:
        mov     bx, handles
        mov     dx, 1                   ; handle number
.h:     cmp     byte [cs:bx], 0
        je      .skip
        movzx   cx, byte [cs:bx + 1]
        jcxz    .match                  ; typelen 0: every frame
        push    cs
        pop     ds
        lea     si, [bx + 2]
        mov     di, rxbuf + 12
        push    cs
        pop     es
        cld
        repe    cmpsb
        je      .match
.skip:  add     bx, H_SIZE
        inc     dx
        cmp     dx, MAX_HANDLES
        jbe     .h
        ret
.match:
        mov     eax, [cs:bx + 10]       ; receiver
        mov     [cs:receiver], eax
        mov     [cs:curhandle], dx
        ; first upcall: AX=0, BX=handle, CX=length -> ES:DI buffer or 0:0
        xor     ax, ax
        mov     bx, dx
        mov     cx, [cs:rxlen]
        xor     di, di
        mov     es, di
        call    far [cs:receiver]
        mov     ax, es
        or      ax, di
        jz      .dropped
        ; copy, then the second upcall: AX=1, DS:SI = the buffer
        push    es
        push    di
        push    cs
        pop     ds
        mov     si, rxbuf
        mov     cx, [cs:rxlen]
        cld
        rep     movsb
        pop     si
        pop     ds
        mov     ax, 1
        mov     bx, [cs:curhandle]
        mov     cx, [cs:rxlen]
        call    far [cs:receiver]
.dropped:
        ret

drv_name    db  'MDVMM NIC', 0
old08       dd  0
old28       dd  0
receiver    dd  0
curhandle   dw  0
rxlen       dw  0
busy        db  0
frames      db  0
mac         times 6 db 0
stats       times 15 dd 0
            align 2
handles     times MAX_HANDLES * H_SIZE db 0
rxbuf       times FRAME_MAX + 2 db 0
resident_end:

; ---------------------------------------------------------------------
; transient part
; ---------------------------------------------------------------------
init:
        mov     dx, msg_banner
        call    print
        ; vector from the command line (hex), default 60h
        mov     si, 81h
        xor     bx, bx
.skip:  lodsb
        cmp     al, ' '
        je      .skip
        cmp     al, 13
        je      .parsed
.digit: cmp     al, '0'
        jb      .parsed
        cmp     al, '9'
        jbe     .num
        or      al, 20h
        cmp     al, 'a'
        jb      .parsed
        cmp     al, 'f'
        ja      .parsed
        sub     al, 'a' - 10 - '0'
.num:   sub     al, '0'
        shl     bx, 4
        add     bl, al
        lodsb
        jmp     .digit
.parsed:
        test    bx, bx
        jnz     .have
        mov     bx, 60h
.have:  cmp     bx, 60h
        jb      .badvec
        cmp     bx, 80h
        ja      .badvec
        mov     [vector], bl

        ; is there a packet driver already?
        xor     ax, ax
        mov     es, ax
        shl     bx, 2
        les     di, [es:bx]
        mov     ax, es
        or      ax, di
        jz      .free
        add     di, 3
        mov     si, sig
        mov     cx, 8
        repe    cmpsb
        je      .inuse
.free:
        ; the monitor's network card
        push    cs
        pop     es
        mov     di, mac
        mov     ax, 0de10h
        int     2fh
        jc      .nocard

        ; vectors
        mov     ax, 3508h
        int     21h
        mov     [old08], bx
        mov     [old08 + 2], es
        mov     ax, 3528h
        int     21h
        mov     [old28], bx
        mov     [old28 + 2], es
        mov     al, [vector]
        mov     ah, 25h
        mov     dx, pkt_entry
        int     21h
        mov     ax, 2508h
        mov     dx, int08
        int     21h
        mov     ax, 2528h
        mov     dx, int28
        int     21h

        ; report: "Packet driver at INT 60h, MAC xx:xx:..."
        mov     al, [vector]
        mov     di, msg_vec
        call    hex2
        mov     si, mac
        mov     di, msg_mac
        mov     cx, 6
.mac:   lodsb
        call    hex2
        inc     di
        loop    .mac
        mov     dx, msg_ok
        call    print
        mov     ax, 0de01h              ; and in the monitor's log
        mov     dx, msg_ok
        int     2fh

        mov     es, [2ch]               ; free the environment
        mov     ah, 49h
        int     21h
        mov     dx, (resident_end - $$ + 100h + 15) >> 4
        mov     ax, 3100h
        int     21h

.badvec:
        mov     dx, msg_badvec
        jmp     fail
.inuse:
        mov     dx, msg_inuse
        jmp     fail
.nocard:
        mov     dx, msg_nocard
fail:   call    print
        mov     ax, 4c01h
        int     21h

; AL -> two hex digits at DI
hex2:   push    ax
        shr     al, 4
        call    .d
        pop     ax
        and     al, 0fh
.d:     add     al, '0'
        cmp     al, '9'
        jbe     .s
        add     al, 'A' - '9' - 1
.s:     mov     [di], al
        inc     di
        ret

print:  mov     ah, 9
        int     21h
        ret

vector      db  0
sig         db  'PKT DRVR'
msg_banner  db  'PKTDRV - packet driver for the MDVMM network cards', 13, 10, '$'
msg_ok      db  'Packet driver at INT '
msg_vec     db  '00h, MAC '
msg_mac     db  '00:00:00:00:00:00', 13, 10, 0, '$'    ; ASCIZ for the log, $ for DOS
msg_badvec  db  'Usage: PKTDRV [vector]   (60..80 hex, default 60)', 13, 10, '$'
msg_inuse   db  'There is a packet driver at that vector already.', 13, 10, '$'
msg_nocard  db  'No network card (MDVMM must be running, with a supported card).', 13, 10, '$'
