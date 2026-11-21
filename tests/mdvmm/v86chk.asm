; V86CHK.COM - exit 0 if running in V86 mode under MDVMM
        cpu     386
        org     100h
        smsw    ax
        test    al, 1
        jz      .real
        mov     ax, 0de00h              ; MDVMM installation check
        int     2fh
        cmp     bx, 4d44h
        jne     .noapi
        mov     ax, 0de01h              ; log through the monitor
        mov     dx, msg_ok
        int     2fh
        mov     ax, 4c00h
        int     21h
.real:  mov     ax, 4c01h
        int     21h
.noapi: mov     ax, 4c02h
        int     21h
msg_ok  db      'v86chk: protected mode bit set, MDVMM API answers', 10, 0
