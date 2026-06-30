/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_UEFI_MIN_H
#define EMBER_UEFI_MIN_H

#include <stdint.h>
#include <stddef.h>

/* Basic EFI types. */
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;

#define EFI_SUCCESS 0
#define EFI_ERROR_MASK 0x8000000000000000ULL
#define EFI_ERROR(x) (((x) & EFI_ERROR_MASK) != 0)

/* EFI GUID. */
typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t Data4[8];
} EFI_GUID;

/* Memory types. */
typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* Memory descriptor. */
typedef struct {
	uint32_t Type;
	uint32_t Pad;
	uint64_t PhysicalStart;
	uint64_t VirtualStart;
	uint64_t NumberOfPages;
	uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Table header. */
typedef struct {
	uint64_t Signature;
	uint32_t Revision;
	uint32_t HeaderSize;
	uint32_t CRC32;
	uint32_t Reserved;
} EFI_TABLE_HEADER;

/* Simple text output (optional) */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS(*Reset) (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL * This,
			    uint8_t ExtendedVerification);
	EFI_STATUS(*OutputString) (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL * This,
				   uint16_t * String);
	void *TestString;
	void *QueryMode;
	void *SetMode;
	void *SetAttribute;
	void *ClearScreen;
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
};

/* Graphics Output Protocol. */
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	uint32_t RedMask;
	uint32_t GreenMask;
	uint32_t BlueMask;
	uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	uint32_t Version;
	uint32_t HorizontalResolution;
	uint32_t VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	uint32_t MaxMode;
	uint32_t Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	size_t SizeOfInfo;
	uint64_t FrameBufferBase;
	size_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS(*QueryMode) (EFI_GRAPHICS_OUTPUT_PROTOCOL * This,
				uint32_t ModeNumber, size_t * SizeOfInfo,
				EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ** Info);
	EFI_STATUS(*SetMode) (EFI_GRAPHICS_OUTPUT_PROTOCOL * This,
			      uint32_t ModeNumber);
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* Boot services. */
typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *RaiseTPL;
	void *RestoreTPL;
	void *AllocatePages;
	void *FreePages;
	 EFI_STATUS(*GetMemoryMap) (size_t * MemoryMapSize,
				    EFI_MEMORY_DESCRIPTOR * MemoryMap,
				    size_t * MapKey, size_t * DescriptorSize,
				    uint32_t * DescriptorVersion);
	 EFI_STATUS(*AllocatePool) (int PoolType, size_t Size, void **Buffer);
	 EFI_STATUS(*FreePool) (void *Buffer);

	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	 EFI_STATUS(*HandleProtocol) (EFI_HANDLE Handle, EFI_GUID * Protocol,
				      void **Interface);
	void *Reserved;
	 EFI_STATUS(*RegisterProtocolNotify) (EFI_GUID * Protocol, void *Event,
					      void **Registration);
	 EFI_STATUS(*LocateHandle) (int SearchType, EFI_GUID * Protocol,
				    void *SearchKey, size_t * BufferSize,
				    EFI_HANDLE * Buffer);
	 EFI_STATUS(*LocateDevicePath) (EFI_GUID * Protocol, void **DevicePath,
					EFI_HANDLE * Device);
	 EFI_STATUS(*InstallConfigurationTable) (EFI_GUID * Guid, void *Table);

	 EFI_STATUS(*LoadImage) (uint8_t BootPolicy,
				 EFI_HANDLE ParentImageHandle, void *DevicePath,
				 void *SourceBuffer, size_t SourceSize,
				 EFI_HANDLE * ImageHandle);
	 EFI_STATUS(*StartImage) (EFI_HANDLE ImageHandle, size_t * ExitDataSize,
				  uint16_t ** ExitData);
	 EFI_STATUS(*Exit) (EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
			    size_t ExitDataSize, uint16_t * ExitData);
	 EFI_STATUS(*UnloadImage) (EFI_HANDLE ImageHandle);
	 EFI_STATUS(*ExitBootServices) (EFI_HANDLE ImageHandle, size_t MapKey);

	void *GetNextMonotonicCount;
	void *Stall;
	void *SetWatchdogTimer;

	 EFI_STATUS(*ConnectController) (EFI_HANDLE ControllerHandle,
					 void *DriverImageHandle,
					 void *RemainingDevicePath,
					 uint8_t Recursive);
	 EFI_STATUS(*DisconnectController) (EFI_HANDLE ControllerHandle,
					    void *DriverImageHandle,
					    void *ChildHandle);
	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;

	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	 EFI_STATUS(*LocateProtocol) (EFI_GUID * Protocol, void *Registration,
				      void **Interface);
	 EFI_STATUS(*InstallMultipleProtocolInterfaces) (void);
	 EFI_STATUS(*UninstallMultipleProtocolInterfaces) (void);

	void *CalculateCrc32;
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* Configuration table entry. */
typedef struct {
	EFI_GUID VendorGuid;
	void *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* System table. */
typedef struct {
	EFI_TABLE_HEADER Hdr;
	uint16_t *FirmwareVendor;
	uint32_t FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	size_t NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* Loaded Image Protocol (minimal fields) */
typedef struct {
	uint32_t Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	void *FilePath;
	void *Reserved;
	uint32_t LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	uint64_t ImageSize;
	uint32_t ImageCodeType;
	uint32_t ImageDataType;
	 EFI_STATUS(*Unload) (EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* EFI File Protocol. */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
	uint64_t Revision;
	 EFI_STATUS(*Open) (EFI_FILE_PROTOCOL * This,
			    EFI_FILE_PROTOCOL ** NewHandle, uint16_t * FileName,
			    uint64_t OpenMode, uint64_t Attributes);
	 EFI_STATUS(*Close) (EFI_FILE_PROTOCOL * This);
	void *Delete;
	 EFI_STATUS(*Read) (EFI_FILE_PROTOCOL * This, uint64_t * BufferSize,
			    void *Buffer);
	void *Write;
	void *GetPosition;
	void *SetPosition;
	 EFI_STATUS(*GetInfo) (EFI_FILE_PROTOCOL * This,
			       EFI_GUID * InformationType,
			       uint64_t * BufferSize, void *Buffer);
	void *SetInfo;
	void *Flush;
};

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

/* Simple File System Protocol. */
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	uint64_t Revision;
	 EFI_STATUS(*OpenVolume) (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL * This,
				  EFI_FILE_PROTOCOL ** Root);
};

/* EFI_FILE_INFO -- only the fields we need (size at offset 0) */
typedef struct {
	uint64_t Size;		/* Size of this struct including name. */
	uint64_t FileSize;
	uint64_t PhysicalSize;
	/* Remaining fields omitted. */
} EFI_FILE_INFO;

/* GUIDs. */
static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID =
    { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72,
				   0x3B} };

static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
    { 0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51,
				   0x6A} };

static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID =
    { 0x0964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72,
				    0x3B} };

static const EFI_GUID EFI_FILE_INFO_GUID =
    { 0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72,
				   0x3B} };

#endif
