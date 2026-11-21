; XMSTEST.COM - XMS round trip: under MDVMM the XMS driver's moves go
; through the monitor's INT 15h AH=87h emulation. Exit 0 on success.
        cpu     386
        org     100h
SIZE    equ     4096
        mov     ax, 4300h
        int     2fh
        cmp     al, 80h
        jne     fail
        mov     ax, 4310h
        int     2fh
        mov     [xms], bx
        mov     [xms + 2], es
        push    cs
        pop     es
        mov     ah, 09h                 ; allocate 64K
        mov     dx, 64
        call    far [xms]
        test    ax, ax
        jz      fail
        mov     [handle], dx
        ; pattern
        mov     di, buf
        mov     cx, SIZE
        xor     ax, ax
.fill:  mov     [di], al
        add     al, 7
        inc     di
        loop    .fill
        ; conventional -> EMB offset 8000h
        mov     word [mv + 4], 0
        mov     word [mv + 6], buf
        mov     [mv + 8], cs
        mov     ax, [handle]
        mov     [mv + 10], ax
        mov     dword [mv + 12], 8000h
        call    move
        ; clear buffer
        mov     di, buf
        mov     cx, SIZE
        xor     al, al
        rep     stosb
        ; EMB -> conventional
        mov     ax, [handle]
        mov     [mv + 4], ax
        mov     dword [mv + 6], 8000h
        mov     word [mv + 10], 0
        mov     word [mv + 12], buf
        mov     [mv + 14], cs
        call    move
        ; verify
        mov     si, buf
        mov     cx, SIZE
        xor     al, al
.chk:   cmp     [si], al
        jne     fail
        add     al, 7
        inc     si
        loop    .chk
        mov     ah, 0ah                 ; free
        mov     dx, [handle]
        call    far [xms]
        mov     ax, 0de01h              ; log (ignored without MDVMM)
        mov     dx, msg_ok
        int     2fh
        mov     ax, 4c00h
        int     21h
move:   mov     dword [mv], SIZE
        mov     si, mv
        mov     ah, 0bh
        call    far [xms]
        test    ax, ax
        jz      fail
        ret
fail:   mov     ax, 4c01h
        int     21h
msg_ok  db      'xmstest: 4K round trip through XMS ok', 10, 0
xms     dd      0
handle  dw      0
mv      times 16 db 0
buf:
