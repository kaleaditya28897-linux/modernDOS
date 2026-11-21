; POWEROFF.COM - switch the (virtual) machine off: APM, then ACPI ports
        cpu     386
        org     100h
        mov     ax, 5301h               ; APM real mode connect
        xor     bx, bx
        int     15h
        mov     ax, 530eh               ; APM driver version 1.2
        xor     bx, bx
        mov     cx, 0102h
        int     15h
        mov     ax, 5307h               ; set power state: off
        mov     bx, 1
        mov     cx, 3
        int     15h
        mov     dx, 604h                ; QEMU PIIX4 PM1a_CNT: SLP_EN
        mov     ax, 2000h
        out     dx, ax
        mov     dx, 0b004h              ; older QEMU / Bochs
        out     dx, ax
.hang:  cli
        hlt
        jmp     .hang
