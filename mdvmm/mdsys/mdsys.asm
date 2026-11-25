; MDSYS.COM - modernDOS system control (through the MDVMM monitor)
;
;   MDSYS            show the state (kind of boot, trial)
;   MDSYS KEEP       confirm a trial boot: no automatic restart
;   MDSYS REBOOT     restart the machine
;   MDSYS DISK       exit code 0 if hard disks can be written, 1 if they are
;                    read-only (a start from a stick); prints nothing
;   MDSYS WRITE      exit code 0 if a file can be made in C:\MDOS\TEMP, 1 if
;                    not (the system runs from a CD); prints nothing and
;                    needs no monitor
;
; Exit code: 0 normal boot, 1 unconfirmed trial boot, 2 confirmed trial
; boot, 255 no monitor (so batch files can tell).

        cpu     386
        bits    16
        org     100h

start:
        mov     si, 81h
.sp0:   lodsb
        cmp     al, ' '
        je      .sp0
        and     al, 0dfh
        cmp     al, 'W'
        je      wtest
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

; can the system disk be written? A critical error (write protection) must
; fail the call, not ask "Abort, Retry".
wtest:  mov     dx, int24
        mov     ax, 2524h
        int     21h
        mov     dx, wname
        xor     cx, cx
        mov     ah, 3ch                 ; create
        int     21h
        jc      .no
        mov     bx, ax
        mov     dx, wname
        mov     cx, 1
        mov     ah, 40h                 ; a byte: the create alone may sit in a buffer
        int     21h
        pushf
        mov     ah, 3eh
        int     21h
        pushf
        mov     dx, wname
        mov     ah, 41h
        int     21h
        popf
        jc      .no2
        popf
        jc      .no
        mov     ax, 4c00h
        int     21h
.no2:   popf
.no:    mov     ax, 4c01h
        int     21h
int24:  mov     al, 3                   ; fail the call
        iret

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

wname       db  'C:\MDOS\TEMP\W.$$$', 0
msg_uefi    db  'UEFI boot. $'
msg_dos     db  'DOS boot. $'
msg_normal  db  'Installed system.', 13, 10, '$'
msg_trial   db  'TRIAL boot: restarts in ten minutes unless confirmed with MDSYS KEEP.', 13, 10, '$'
msg_kept    db  'Trial boot, confirmed: it stays until the next restart.', 13, 10, '$'
msg_reboot  db  'Restarting...', 13, 10, '$'
msg_nomon   db  'MDVMM is not running.', 13, 10, '$'
msg_usage   db  'Usage: MDSYS [KEEP | REBOOT | DISK | WRITE]', 13, 10, '$'
