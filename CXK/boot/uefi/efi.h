/* /CXK/boot/uefi/efi.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * The subset of the UEFI spec the CXK boot stub actually uses.
 *
 * Deliberately not gnu-efi or EDK2. Those are large, opinionated about how you
 * build, and would add a dependency to a project that already juggles three
 * toolchains. Everything here is transcribed from the UEFI specification and is
 * small enough to audit in one sitting.
 *
 * ---- Layout is load-bearing ----------------------------------------------
 *
 * These structures are read from memory firmware laid out. Every function
 * pointer in a service table must appear in spec order even when unused, or
 * every field after it is read from the wrong offset - so unused entries are
 * void* placeholders rather than omitted. Do not "tidy" them away.
 *
 * ---- Calling convention ---------------------------------------------------
 *
 * UEFI on x86-64 uses the Microsoft x64 ABI, not SysV. A stub built with
 * clang -target x86_64-unknown-windows or with MSVC gets that by default. It
 * also needs -mno-red-zone (/Gs on MSVC): firmware interrupts run on the same
 * stack and will happily scribble over a red zone that the ABI says is yours.
 *
 * ---- Strings --------------------------------------------------------------
 *
 * UEFI strings are UTF-16. The compiler must make L"..." 16-bit: -fshort-wchar
 * on clang/gcc, which is the default under MSVC. Get this wrong and the stub
 * builds cleanly and prints nothing recognisable.
 */

#ifndef CXK_EFI_H
#define CXK_EFI_H

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed   long long  INT64;
typedef UINT64              UINTN;      /* x86-64 only; this stub is not 32-bit */
typedef UINT16              CHAR16;
typedef UINT64              EFI_STATUS;
typedef void               *EFI_HANDLE;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;

#define EFIAPI

#define EFI_SUCCESS               0ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_FOUND             0x800000000000000EULL
#define EFI_ERROR(s)              (((INT64)(s)) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ---- simple text output -------------------------------------------------- */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINT8 Extended);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET   Reset;
    EFI_TEXT_STRING  OutputString;
    void            *TestString;
    void            *QueryMode;
    void            *SetMode;
    void            *SetAttribute;
    void            *ClearScreen;
    void            *SetCursorPosition;
    void            *EnableCursor;
    void            *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- memory -------------------------------------------------------------- */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

/* EFI_MEMORY_TYPE - only the ones the stub classifies are named. */
#define EfiReservedMemoryType       0
#define EfiLoaderCode               1
#define EfiLoaderData               2
#define EfiBootServicesCode         3
#define EfiBootServicesData         4
#define EfiRuntimeServicesCode      5
#define EfiRuntimeServicesData      6
#define EfiConventionalMemory       7
#define EfiUnusableMemory           8
#define EfiACPIReclaimMemory        9
#define EfiACPIMemoryNVS           10
#define EfiMemoryMappedIO          11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode                 13
#define EfiPersistentMemory        14

typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- boot services ------------------------------------------------------
 * Order is the spec's. Unused entries are void* on purpose - see the note at
 * the top of this file. */
typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type, UINT32 MemoryType, UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    UINT32 PoolType, UINTN Size, void **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, void *Registration, void **Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

typedef struct {
    EFI_TABLE_HEADER        Hdr;

    void                   *RaiseTPL;                       /*  24 */
    void                   *RestoreTPL;                     /*  32 */

    EFI_ALLOCATE_PAGES      AllocatePages;                  /*  40 */
    void                   *FreePages;                      /*  48 */
    EFI_GET_MEMORY_MAP      GetMemoryMap;                   /*  56 */
    EFI_ALLOCATE_POOL       AllocatePool;                   /*  64 */
    EFI_FREE_POOL           FreePool;                       /*  72 */

    void                   *CreateEvent;                    /*  80 */
    void                   *SetTimer;                       /*  88 */
    void                   *WaitForEvent;                   /*  96 */
    void                   *SignalEvent;                    /* 104 */
    void                   *CloseEvent;                     /* 112 */
    void                   *CheckEvent;                     /* 120 */

    void                   *InstallProtocolInterface;       /* 128 */
    void                   *ReinstallProtocolInterface;     /* 136 */
    void                   *UninstallProtocolInterface;     /* 144 */
    void                   *HandleProtocol;                 /* 152 */
    void                   *Reserved;                       /* 160 */
    void                   *RegisterProtocolNotify;         /* 168 */
    void                   *LocateHandle;                   /* 176 */
    void                   *LocateDevicePath;               /* 184 */
    void                   *InstallConfigurationTable;      /* 192 */

    void                   *LoadImage;                      /* 200 */
    void                   *StartImage;                     /* 208 */
    void                   *Exit;                           /* 216 */
    void                   *UnloadImage;                    /* 224 */
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;               /* 232 */

    void                   *GetNextMonotonicCount;          /* 240 */
    EFI_STALL               Stall;                          /* 248 */
    void                   *SetWatchdogTimer;               /* 256 */

    void                   *ConnectController;              /* 264 */
    void                   *DisconnectController;           /* 272 */

    void                   *OpenProtocol;                   /* 280 */
    void                   *CloseProtocol;                  /* 288 */
    void                   *OpenProtocolInformation;        /* 296 */

    void                   *ProtocolsPerHandle;             /* 304 */
    void                   *LocateHandleBuffer;             /* 312 */
    EFI_LOCATE_PROTOCOL     LocateProtocol;                 /* 320 */
    void                   *InstallMultipleProtocolInterfaces;   /* 328 */
    void                   *UninstallMultipleProtocolInterfaces; /* 336 */

    void                   *CalculateCrc32;                 /* 344 */
    void                   *CopyMem;                        /* 352 */
    void                   *SetMem;                         /* 360 */
    void                   *CreateEventEx;                  /* 368 */
} EFI_BOOT_SERVICES;

/* ---- runtime services (only GetVariable, for the Secure Boot flag) ------- */
typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes,
    UINTN *DataSize, void *Data);

typedef struct {
    EFI_TABLE_HEADER  Hdr;
    void             *GetTime;                  /*  24 */
    void             *SetTime;                  /*  32 */
    void             *GetWakeupTime;            /*  40 */
    void             *SetWakeupTime;            /*  48 */
    void             *SetVirtualAddressMap;     /*  56 */
    void             *ConvertPointer;           /*  64 */
    EFI_GET_VARIABLE  GetVariable;              /*  72 */
    void             *GetNextVariableName;      /*  80 */
    void             *SetVariable;              /*  88 */
    void             *GetNextHighMonotonicCount;/*  96 */
    void             *ResetSystem;              /* 104 */
} EFI_RUNTIME_SERVICES;

/* ---- configuration table ------------------------------------------------- */
typedef struct {
    EFI_GUID  VendorGuid;
    void     *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ---- system table -------------------------------------------------------- */
typedef struct {
    EFI_TABLE_HEADER                  Hdr;
    CHAR16                           *FirmwareVendor;       /*  24 */
    UINT32                            FirmwareRevision;     /*  32 */
    UINT32                            _pad;
    EFI_HANDLE                        ConsoleInHandle;      /*  40 */
    void                             *ConIn;                /*  48 */
    EFI_HANDLE                        ConsoleOutHandle;     /*  56 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;               /*  64 */
    EFI_HANDLE                        StandardErrorHandle;  /*  72 */
    void                             *StdErr;               /*  80 */
    EFI_RUNTIME_SERVICES             *RuntimeServices;      /*  88 */
    EFI_BOOT_SERVICES                *BootServices;         /*  96 */
    UINTN                             NumberOfTableEntries; /* 104 */
    EFI_CONFIGURATION_TABLE          *ConfigurationTable;   /* 112 */
} EFI_SYSTEM_TABLE;

/* ---- graphics output ----------------------------------------------------- */
#define PixelRedGreenBlueReserved8BitPerColor  0
#define PixelBlueGreenRedReserved8BitPerColor  1
#define PixelBitMask                           2
#define PixelBltOnly                           3

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32             Version;
    UINT32             HorizontalResolution;
    UINT32             VerticalResolution;
    UINT32             PixelFormat;
    EFI_PIXEL_BITMASK  PixelInformation;
    UINT32             PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber,
    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    SetMode;
    void                                    *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- GUIDs --------------------------------------------------------------- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d } }
#define SMBIOS3_TABLE_GUID \
    { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94 } }
#define SMBIOS_TABLE_GUID \
    { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d } }
#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c } }

#endif /* CXK_EFI_H */
