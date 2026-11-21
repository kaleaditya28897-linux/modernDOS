; MDVMM boot information - must match bootinfo.h
BOOTINFO_MAGIC   equ 0x4D56444D
BOOTINFO_VERSION equ 2

struc bootinfo
  .magic      resd 1
  .version    resd 1
  .pool_phys  resd 1
  .pool_size  resd 1
  .core_size  resd 1
  .trampoline resd 1
  .eflags     resd 1
  .ip         resw 1
  .cs         resw 1
  .sp         resw 1
  .ss         resw 1
  .ds         resw 1
  .es         resw 1
  .fs         resw 1
  .gs         resw 1
  .psp        resw 1
  .stub_seg   resw 1
  .stub_bp    resw 1
  .stub_entry resw 1
  .stub_psp   resw 1
  .stub_retf  resw 1
endstruc

; MDVMM INT 2Fh API (AH = MDVMM_MPX)
MDVMM_MPX        equ 0xDE
MDVMM_SIGNATURE  equ 0x4D44      ; "MD" returned in BX by AX=DE00h

; V86 breakpoints (HLT at stub_bp + n)
BP_DPMI_ENTRY    equ 0
BP_RM_RETURN     equ 1
BP_RAW_TO_PM     equ 2
BP_CALLBACK      equ 3            ; 16 real mode callbacks
BP_COUNT         equ 19
