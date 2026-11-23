; NETCHK.COM - network test through the packet driver (no TCP/IP stack)
;
; Finds the packet driver at INT 60h, reads the MAC, registers a receiver
; for ARP (type 0806h), sends an ARP request for 10.0.2.2 (QEMU's user
; network gateway) from 10.0.2.15 and waits up to 3 s for the reply.
; Logs through the MDVMM monitor (INT 2Fh AX=DE01h); exit code 0 if the
; gateway answered.

        cpu     386
        org     100h

PKT_INT equ     60h

start:
        ; a packet driver at INT 60h?
        xor     ax, ax
        mov     es, ax
        les     di, [es:PKT_INT * 4]
        add     di, 3
        mov     si, sig
        mov     cx, 8
        cld
        repe    cmpsb
        jne     no_driver

        mov     ax, 01ffh               ; driver_info
        push    ds
        int     PKT_INT
        pop     ds
        jc      fail_info

        push    cs
        pop     es
        mov     di, my_mac              ; get_address
        mov     cx, 6
        mov     ah, 6
        int     PKT_INT
        jc      fail_info

        mov     ax, 0201h               ; access_type, class 1
        mov     bx, 0ffffh
        mov     dl, 0
        mov     si, arp_type
        mov     cx, 2
        push    cs
        pop     es
        mov     di, receiver
        int     PKT_INT
        jc      fail_access
        mov     [handle], ax

        ; the ARP request
        mov     si, my_mac
        mov     di, frame + 6           ; source MAC
        mov     cx, 6
        rep     movsb
        mov     si, my_mac
        mov     di, frame + 22          ; sender hardware address
        mov     cx, 6
        rep     movsb
        mov     si, frame
        mov     cx, 42
        mov     ah, 4                   ; send_pkt
        int     PKT_INT
        jc      fail_send

        ; wait for the answer (3 s)
        xor     ax, ax
        mov     es, ax
        mov     ebx, [es:46ch]
.wait:  cmp     byte [got], 0
        jne     .answered
        sti
        hlt
        mov     eax, [es:46ch]
        sub     eax, ebx
        cmp     eax, 55
        jb      .wait
        mov     dx, msg_noreply
        jmp     done_fail
.answered:
        ; "netchk: ARP reply from 10.0.2.2, MAC xx:xx:xx:xx:xx:xx"
        mov     si, reply_mac
        mov     di, msg_mac
        mov     cx, 6
.mac:   lodsb
        call    hex2
        inc     di
        loop    .mac
        mov     dx, msg_ok
        call    log
        call    release
        mov     ax, 4c00h
        int     21h

no_driver:
        mov     dx, msg_nodrv
        jmp     done_fail
fail_info:
        mov     dx, msg_info
        jmp     done_fail
fail_access:
        mov     dx, msg_access
        jmp     done_fail
fail_send:
        mov     dx, msg_send
done_fail:
        call    log
        call    release
        mov     ax, 4c01h
        int     21h

release:
        mov     bx, [handle]
        test    bx, bx
        jz      .r
        mov     ah, 3
        int     PKT_INT
.r:     ret

log:    push    dx                      ; to the monitor log and the screen
        mov     ax, 0de01h
        int     2fh
        pop     dx
        mov     si, dx                  ; print the ASCIZ string with INT 21h/02h
.p:     lodsb
        test    al, al
        jz      .e
        mov     dl, al
        mov     ah, 2
        int     21h
        jmp     .p
.e:     ret

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

; packet driver receiver: AX=0 -> ES:DI buffer (CX bytes), AX=1 -> filled
receiver:
        test    ax, ax
        jnz     .filled
        cmp     cx, 128
        ja      .drop
        push    cs
        pop     es
        mov     di, rxbuf
        retf
.drop:  xor     di, di
        mov     es, di
        retf
.filled:                                ; DS:SI = rxbuf
        cmp     word [cs:rxbuf + 20], 0200h      ; ARP reply (opcode 2, big endian)
        jne     .ret
        cmp     dword [cs:rxbuf + 28], 0202000ah ; sender 10.0.2.2
        jne     .ret
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     si, rxbuf + 22
        mov     di, reply_mac
        mov     cx, 6
        cld
        rep     movsb
        mov     byte [got], 1
        pop     es
        pop     ds
        pop     di
        pop     si
.ret:   retf

sig         db  'PKT DRVR'
arp_type    db  08h, 06h
handle      dw  0
got         db  0
my_mac      times 6 db 0
reply_mac   times 6 db 0

frame:      db  0ffh, 0ffh, 0ffh, 0ffh, 0ffh, 0ffh      ; to everyone
            times 6 db 0                                ; from
            db  08h, 06h                                ; ARP
            db  0, 1, 08h, 0, 6, 4, 0, 1                ; Ethernet, IPv4, request
            times 6 db 0                                ; sender MAC
            db  10, 0, 2, 15                            ; sender IP
            times 6 db 0                                ; target MAC
            db  10, 0, 2, 2                             ; target IP

msg_ok      db  'netchk: ARP reply from 10.0.2.2, MAC '
msg_mac     db  '00:00:00:00:00:00', 13, 10, 0
msg_noreply db  'netchk: no ARP reply from 10.0.2.2', 13, 10, 0
msg_nodrv   db  'netchk: no packet driver at INT 60h', 13, 10, 0
msg_info    db  'netchk: packet driver does not answer', 13, 10, 0
msg_access  db  'netchk: access_type failed', 13, 10, 0
msg_send    db  'netchk: send_pkt failed', 13, 10, 0

rxbuf       times 128 db 0
