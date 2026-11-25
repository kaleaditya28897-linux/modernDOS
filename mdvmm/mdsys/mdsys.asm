; MDSYS.COM - modernDOS system control (through the MDVMM monitor)
;
;   MDSYS            show the state (kind of boot, trial)
;   MDSYS KEEP       confirm a trial boot: no automatic restart
;   MDSYS REBOOT     restart the machine
;   MDSYS DISK       exit code 0 if hard disks can be written, 1 if they are
;                    read-only (a start from a stick); prints nothing
;
; Exit code: 0 normal boot, 1 unconfirmed trial boot, 2 confirmed trial
; boot, 255 no monitor (so batch files can tell).

        cpu     386
        bits    16
        org     100h

start:
        mov     ax, 0de00h
        int     2fh
        cmp     al, 0ffh
        jne     nomon
        mov     si, 81h
.sp:    lodsb
        cmp     al, ' '
        je      .sp
        cmp     al, 13
        je      state
        and     al, 0dfh
        cmp     al, 'K'
        je      keep
        cmp     al, 'R'
        je      reboot
        cmp     al, 'D'
        je      disk
        mov     dx, msg_usage
        call    print
        mov     ax, 4cffh
        int     21h

keep:   mov     ax, 0de16h
        int     2fh
state:  mov     ax, 0de15h
        int     2fh
        jc      nomon
        mov     bx, ax
        mov     dx, msg_dos
        test    bl, 1
        jz      .p
        mov     dx, msg_uefi
.p:     call    print
        test    bl, 2
        jnz     .trial
        mov     dx, msg_normal
        call    print
        mov     ax, 4c00h
        int     21h
.trial: test    bl, 4
        jnz     .kept
        mov     dx, msg_trial
        call    print
        mov     ax, 4c01h
        int     21h
.kept:  mov     dx, msg_kept
        call    print
        mov     ax, 4c02h
        int     21h

disk:   mov     ax, 0de15h
        int     2fh
        jc      nomon
        test    al, 8
        mov     ax, 4c00h
        jnz     .w
        inc     ax
.w:     int     21h

reboot: mov     dx, msg_reboot
        call    print
        mov     ah, 0dh                 ; DOS: write its buffers to the disks
        int     21h
        mov     ax, 0de17h
        int     2fh
        ; not reached with the monitor; without: the keyboard controller
        mov     al, 0feh
        out     64h, al
        jmp     $

nomon:  mov     dx, msg_nomon
        call    print
        mov     ax, 4cffh
        int     21h

print:  mov     ah, 9
        int     21h
        ret

msg_uefi    db  'UEFI boot. $'
msg_dos     db  'DOS boot. $'
msg_normal  db  'Installed system.', 13, 10, '$'
msg_trial   db  'TRIAL boot: restarts in ten minutes unless confirmed with MDSYS KEEP.', 13, 10, '$'
msg_kept    db  'Trial boot, confirmed: it stays until the next restart.', 13, 10, '$'
msg_reboot  db  'Restarting...', 13, 10, '$'
msg_nomon   db  'MDVMM is not running.', 13, 10, '$'
msg_usage   db  'Usage: MDSYS [KEEP | REBOOT | DISK]', 13, 10, '$'
