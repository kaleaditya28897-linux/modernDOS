/* Minimal UEFI definitions for the MDVMM loader (x64), from the UEFI
 * specification. Only what the loader uses is typed; other table slots
 * are placeholders so the offsets match. */
#ifndef MDVMM_EFI_H
#define MDVMM_EFI_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;

#define EFI_SUCCESS            0
#define EFI_ERROR(s)           ((int64_t)(s) < 0)
#define EFI_BUFFER_TOO_SMALL   0x8000000000000005ull

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } EFI_GUID;

typedef struct {
  uint64_t Signature;
  uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* memory */
enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };
enum {
  EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
  EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
  EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
  EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
  EfiPalCode, EfiPersistentMemory
};

typedef struct {
  uint32_t Type, Pad;
  uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL, *RestoreTPL;
  EFI_STATUS (EFIAPI *AllocatePages)(int Type, int MemoryType, UINTN Pages, uint64_t *Memory);
  EFI_STATUS (EFIAPI *FreePages)(uint64_t Memory, UINTN Pages);
  EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map, UINTN *MapKey,
                                    UINTN *DescriptorSize, uint32_t *DescriptorVersion);
  EFI_STATUS (EFIAPI *AllocatePool)(int PoolType, UINTN Size, void **Buffer);
  EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
  void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
  void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
  EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
  void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath;
  void *InstallConfigurationTable, *LoadImage, *StartImage, *Exit, *UnloadImage;
  EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
  void *GetNextMonotonicCount, *Stall;
  EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t Code, UINTN DataSize, CHAR16 *Data);
  void *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol;
  void *OpenProtocolInformation, *ProtocolsPerHandle, *LocateHandleBuffer;
  EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
} EFI_BOOT_SERVICES;

typedef struct {
  EFI_GUID VendorGuid;
  void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor;
  uint32_t FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  void *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  void *StdErr;
  void *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  UINTN NumberOfTableEntries;
  EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
  uint32_t Revision;
  EFI_HANDLE ParentHandle;
  EFI_SYSTEM_TABLE *SystemTable;
  EFI_HANDLE DeviceHandle;
  void *FilePath, *Reserved;
  uint32_t LoadOptionsSize;
  void *LoadOptions, *ImageBase;
  uint64_t ImageSize;
  int ImageCodeType, ImageDataType;
  void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* files */
#define EFI_FILE_MODE_READ   1ull
#define EFI_FILE_MODE_WRITE  2ull
#define EFI_FILE_MODE_CREATE 0x8000000000000000ull

typedef struct EFI_FILE_PROTOCOL {
  uint64_t Revision;
  EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *This, struct EFI_FILE_PROTOCOL **New,
                            CHAR16 *Name, uint64_t Mode, uint64_t Attributes);
  EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *This);
  EFI_STATUS (EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *This);
  EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *This, UINTN *Size, void *Buffer);
  EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL *This, UINTN *Size, void *Buffer);
  EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *This, uint64_t *Position);
  EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *This, uint64_t Position);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  uint64_t Revision;
  EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* graphics */
enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor,
       PixelBitMask, PixelBltOnly };

typedef struct {
  uint32_t Version, HorizontalResolution, VerticalResolution;
  int PixelFormat;
  uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
  uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
  uint32_t MaxMode, Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINTN SizeOfInfo;
  uint64_t FrameBufferBase;
  UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
  EFI_STATUS (EFIAPI *QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t Mode,
                                 UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
  EFI_STATUS (EFIAPI *SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t Mode);
  void *Blt;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* UGA (EFI 1.1 graphics, before GOP; older Apple firmware) */
typedef struct EFI_UGA_DRAW_PROTOCOL {
  EFI_STATUS (EFIAPI *GetMode)(struct EFI_UGA_DRAW_PROTOCOL *This, uint32_t *Width,
                               uint32_t *Height, uint32_t *Depth, uint32_t *Refresh);
  void *SetMode, *Blt;
} EFI_UGA_DRAW_PROTOCOL;

#endif
