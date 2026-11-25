; Master boot record for the modernDOS BIOS disk image: starts the active
; partition's boot sector (LBA when the BIOS has it, else CHS), passing the
; drive in DL and the partition entry in DS:SI.

        cpu     8086
        org     600h

start:  cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 7c00h
        mov     ds, ax
        mov     es, ax
        sti
        cld
        mov     si, 7c00h
        mov     di, 600h
        mov     cx, 256
        rep movsw
        jmp     0:go

go:     mov     si, 600h + 1beh
        mov     cx, 4
.find:  test    byte [si], 80h
        jnz     .found
        add     si, 16
        loop    .find
        mov     si, msg_none
        jmp     fail

.found: mov     bp, si
        mov     [drive], dl
        mov     di, 3                   ; tries
.again: mov     ah, 41h                 ; LBA extensions?
        mov     bx, 55aah
        mov     dl, [drive]
        int     13h
        jc      .chs
        cmp     bx, 0aa55h
        jne     .chs
        test    cl, 1
        jz      .chs
        mov     si, dap
        mov     ax, [bp + 8]
        mov     [si + 8], ax
        mov     ax, [bp + 10]
        mov     [si + 10], ax
        mov     ah, 42h
        mov     dl, [drive]
        int     13h
        jnc     .loaded
.chs:   mov     ax, 0201h
        mov     bx, 7c00h
        mov     cx, [bp + 2]
        mov     dh, [bp + 1]
        mov     dl, [drive]
        int     13h
        jnc     .loaded
        xor     ax, ax                  ; reset, try again
        mov     dl, [drive]
        int     13h
        dec     di
        jnz     .again
        mov     si, msg_read
        jmp     fail
.loaded:
        cmp     word [7c00h + 510], 0aa55h
        mov     si, msg_boot
        jne     fail
        mov     si, bp
        mov     dl, [drive]
        jmp     0:7c00h

fail:   lodsb
        test    al, al
        jz      .stop
        mov     ah, 0eh
        mov     bx, 7
        int     10h
        jmp     fail
.stop:  xor     ax, ax
        int     16h
        int     18h

msg_none db     'No active partition.', 0
msg_read db     'Disk read error.', 0
msg_boot db     'No boot sector on the partition.', 0
drive   db      0
dap     db      16, 0
        dw      1                       ; sectors
        dw      7c00h, 0                ; to 0:7C00
        dd      0, 0                    ; LBA

        times   1b8h - ($ - $$) db 0
