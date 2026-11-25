; MDVMM.COM's file layout: an MZ header that starts the loader exactly as
; DOS starts a .COM program (CS = DS = ES = SS = PSP, IP = 100h), then the
; loader (loader.asm, LSIZE bytes), then the 32-bit core as an overlay DOS
; does not load: the loader reads it from the file into extended memory.
; (A plain .COM file cannot be larger than 64 KB.)

HDR     equ     32

        db      'MZ'
        dw      (HDR + LSIZE) % 512             ; bytes in the last page
        dw      (HDR + LSIZE + 511) / 512       ; pages
        dw      0                               ; relocations
        dw      HDR / 16                        ; header paragraphs
        dw      (10000h - LSIZE) / 16           ; memory after the image: a full 64 KB segment
        dw      0ffffh                          ; all there is
        dw      0fff0h                          ; SS = PSP
        dw      0fffeh                          ; SP
        dw      0                               ; checksum
        dw      100h                            ; IP
        dw      0fff0h                          ; CS = PSP
        dw      1ch                             ; relocation table
        dw      0                               ; overlay number
        times   HDR - ($ - $$) db 0

        incbin  "loader.bin"
        incbin  "core_dos.bin"
