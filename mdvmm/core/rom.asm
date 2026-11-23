; MDVMM virtual BIOS ROM (UEFI boot), mapped at F000:0000.
;
; There is no PC BIOS on a UEFI machine; the monitor provides one. BIOS
; services are breakpoints: the interrupt vector points at a HLT, which
; traps into the monitor (bios.c). The monitor does the work on the V86
; registers and continues after the HLT, at a RETF 2 that keeps the flags
; it set (CF, ZF). Hardware interrupt handlers are real 16-bit code with
; a breakpoint inside, so DOS and TSRs can hook and chain every vector.
;
; Offset 0 holds a table of the addresses the monitor needs, in the order
; of enum rom_id in bios.c.

%include "bootinfo.inc"

VBE_MODE        equ 141h                ; the framebuffer's mode (bios.c)

        cpu     386
        bits    16
        org     0

header:
        db      'MDBR'
        dw      (table_end - table) / 2
table:
        dw      int10, int11, int12, int13, int14, int15, int16, int17
        dw      int18, int19, int1a, irq0_bp, irq1_bp, int16_wait
        dw      dpt, config_table, stub_bp, stub_entry, stub_psp, stub_retf
        dw      irq0, irq1, irq_master, irq_slave, dummy_iret
        dw      vbe_oem, vbe_vendor, vbe_product, vbe_rev, vbe_modes
        dw      int33
table_end:

; ---- BIOS services: HLT (to the monitor), then RETF 2 ----
int10:  hlt
        retf    2
int11:  hlt
        retf    2
int12:  hlt
        retf    2
int13:  hlt
        retf    2
int14:  hlt
        retf    2
int15:  hlt
        retf    2
int16:  hlt
        retf    2
int17:  hlt
        retf    2
int18:  hlt
        retf    2
int19:  hlt
        retf    2
int1a:  hlt
        retf    2
int33:  hlt                             ; mouse driver
        retf    2

; INT 16h read with an empty buffer: the monitor continues here
int16_wait:
        sti
        hlt                             ; a real idle HLT: wait for a key
        jmp     int16

; ---- hardware interrupts ----
irq0:                                   ; INT 08h: timer
irq0_bp:
        hlt                             ; monitor: BIOS tick count
        int     1ch
        push    ax
        mov     al, 20h
        out     20h, al
        pop     ax
        iret

irq1:                                   ; INT 09h: keyboard
        push    ax
        in      al, 60h
        mov     ah, 4fh                 ; keyboard intercept (KEYB etc.)
        stc
        int     15h
        jnc     irq1_done
irq1_bp:
        hlt                             ; monitor: translate scancode in AL
irq1_done:
        cli
        mov     al, 20h
        out     20h, al
        pop     ax
        iret

irq_master:
        push    ax
        mov     al, 20h
        out     20h, al
        pop     ax
        iret

irq_slave:
        push    ax
        mov     al, 20h
        out     0a0h, al
        out     20h, al
        pop     ax
dummy_iret:
        iret

; ---- DPMI V86 stubs (as in the DOS loader's resident part) ----
stub_bp:
        times   BP_COUNT hlt
        align   2
stub_psp        dw      0

stub_entry:
        push    ax
        push    bx
        mov     ah, 62h                 ; current PSP, for the client's ES
        int     21h
        mov     [cs:stub_psp], bx
        pop     bx
        pop     ax
        jmp     stub_bp + BP_DPMI_ENTRY

stub_retf:
        retf

; ---- tables ----
; diskette parameter table (INT 1Eh), 1.44 MB
dpt:    db      0afh, 02h, 25h, 02h, 18, 1bh, 0ffh, 6ch, 0f6h, 0fh, 08h

; INT 15h AH=C0h system configuration
config_table:
        dw      8                       ; bytes following
        db      0fch                    ; model: AT
        db      01h                     ; submodel
        db      0                       ; revision
        db      01110000b               ; 2nd PIC, RTC, INT 15h/4Fh hook
        db      0, 0, 0, 0

; VBE controller information (INT 10h AX=4F00h)
vbe_oem     db  'modernDOS MDVMM GOP', 0
vbe_vendor  db  'modernDOS', 0
vbe_product db  'UEFI framebuffer', 0
vbe_rev     db  '1.0', 0
        align   2
vbe_modes   dw  VBE_MODE, 0ffffh

; (the reset vector, BIOS date and model byte at F000:FFF0 are written
; by bios_init: this image is kept small, it is part of MDVMM.COM)
