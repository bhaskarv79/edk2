/** @file
  Private header for the MMIO-based UFS Host Controller DXE driver.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#pragma once

#include <PiDxe.h>

#include <IndustryStandard/Acpi.h>

#include <Guid/NonDiscoverableDevice.h>

#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/Cpu.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/PciIo.h>
#include <Protocol/UfsHostController.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

extern EFI_DRIVER_BINDING_PROTOCOL   gUfsMemHcDriverBinding;
extern EFI_COMPONENT_NAME_PROTOCOL   gUfsMemHcComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  gUfsMemHcComponentName2;

//
// Size of the UFSHCI register map the generic UFS stack accesses.
//
#define UFS_MEM_HC_MIN_MMIO_SIZE  0x100

#define UFS_MEM_HC_PRIVATE_DATA_SIGNATURE  SIGNATURE_32 ('U', 'F', 'S', 'M')

///
/// Map token passed back through Mapping in UfsMemHcMap() and consumed by
/// UfsMemHcUnmap().
///
/// When a transfer cannot be handed to the controller in place, Map()
/// bounces it through BounceBuffer instead. That happens on a non-coherent
/// platform if a device-to-host buffer does not fill whole CPU cache lines.
/// Invalidating a partial line could otherwise discard dirty data belonging
/// to unrelated memory sharing that line.
///
typedef struct {
  VOID                                   *HostAddress;
  UINTN                                  NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION    Operation;
  VOID                                   *BounceBuffer;
  UINTN                                  BouncePages;
} UFS_MEM_HC_MAP_INFO;

///
/// Per-controller private context allocated in Start() and freed in Stop().
///
typedef struct {
  UINT32                                Signature;

  ///
  /// Produced protocol. Embedded so CR() can recover Private from UfsHc.
  ///
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL    UfsHc;

  ///
  /// Non-discoverable device protocol installed by the platform driver.
  ///
  NON_DISCOVERABLE_DEVICE               *Device;

  ///
  /// UFSHCI register window, taken from Device->Resources.
  ///
  EFI_PHYSICAL_ADDRESS                  MmioBase;
  UINT64                                MmioSize;

  ///
  /// TRUE if DMA is cache coherent (Device->DmaType).
  ///
  BOOLEAN                               DmaCoherent;

  ///
  /// CPU data cache line size used for non-coherent DMA alignment.
  ///
  UINT32                                DmaAlignment;
} UFS_MEM_HC_PRIVATE_DATA;

#define UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC(a) \
  CR (a, UFS_MEM_HC_PRIVATE_DATA, UfsHc, UFS_MEM_HC_PRIVATE_DATA_SIGNATURE)

//
// HC protocol function declarations (implemented in UfsMemHc.c)
//

/**
  Get the MMIO base of the UFS host controller.

  @param[in]   This     A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[out]  MmioBar  The MMIO base address of the UFS host controller.

  @retval EFI_SUCCESS            The operation succeeds.
  @retval EFI_INVALID_PARAMETER  This or MmioBar is NULL.

**/
EFI_STATUS
EFIAPI
UfsMemHcGetMmioBar (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT    UINTN                               *MmioBar
  );

/**
  Provides the UFS controller-specific addresses needed to access system memory.

  @param[in]      This           A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]      Operation      Indicates if the bus master is going to read or write to system memory.
  @param[in]      HostAddress    The system memory address to map to the UFS controller.
  @param[in, out] NumberOfBytes  On input the number of bytes to map. On output the number of bytes
                                 that were mapped.
  @param[out]     DeviceAddress  The resulting map address for the bus master UFS controller to use to
                                 access the hosts HostAddress.
  @param[out]     Mapping        A resulting value to pass to Unmap().

  @retval EFI_SUCCESS            The range was mapped for the returned NumberOfBytes.
  @retval EFI_UNSUPPORTED        The HostAddress cannot be mapped as a common buffer.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES   The request could not be completed due to a lack of resources.

**/
EFI_STATUS
EFIAPI
UfsMemHcMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  );

/**
  Completes the Map() operation and releases any corresponding resources.

  @param[in]  This     A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Mapping  The mapping value returned from Map().

  @retval EFI_SUCCESS            The range was unmapped.
  @retval EFI_INVALID_PARAMETER  This is NULL.

**/
EFI_STATUS
EFIAPI
UfsMemHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  );

/**
  Allocates pages that are suitable for an EdkiiUfsHcOperationBusMasterCommonBuffer
  mapping.

  @param[in]   This         A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]   Type         This parameter is not used and must be ignored.
  @param[in]   MemoryType   The type of memory to allocate, EfiBootServicesData or
                            EfiRuntimeServicesData.
  @param[in]   Pages        The number of pages to allocate.
  @param[out]  HostAddress  A pointer to store the base system memory address of the
                            allocated range.
  @param[in]   Attributes   The requested bit mask of attributes for the allocated range.

  @retval EFI_SUCCESS            The requested memory pages were allocated.
  @retval EFI_UNSUPPORTED        Attributes is unsupported. The only legal attribute bits are
                                 MEMORY_WRITE_COMBINE and MEMORY_CACHED.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES   The memory pages could not be allocated.

**/
EFI_STATUS
EFIAPI
UfsMemHcAllocateBuffer (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE                   Type,
  IN     EFI_MEMORY_TYPE                     MemoryType,
  IN     UINTN                               Pages,
  OUT    VOID                                **HostAddress,
  IN     UINT64                              Attributes
  );

/**
  Frees memory that was allocated with AllocateBuffer().

  @param[in]  This         A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Pages        The number of pages to free.
  @param[in]  HostAddress  The base system memory address of the allocated range.

  @retval EFI_SUCCESS            The requested memory pages were freed.
  @retval EFI_INVALID_PARAMETER  The memory range specified by HostAddress and Pages
                                 was not allocated with AllocateBuffer().

**/
EFI_STATUS
EFIAPI
UfsMemHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  );

/**
  Flushes all posted write transactions from the UFS bus to attached UFS device.

  @param[in]  This  A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.

  @retval EFI_SUCCESS  The posted write transactions were flushed.

**/
EFI_STATUS
EFIAPI
UfsMemHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  );

/**
  Enable a UFS bus driver to access UFS MMIO registers in the UFS Host Controller
  memory space.

  @param[in]      This    A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]      Width   Signifies the width of the memory operations.
  @param[in]      Offset  The offset within the UFS Host Controller MMIO space to start
                          the memory operation.
  @param[in]      Count   The number of memory operations to perform.
  @param[in, out] Buffer  For read operations, the destination buffer to store the
                          results.

  @retval EFI_SUCCESS            The data was read from the UFS host controller.
  @retval EFI_UNSUPPORTED        The address range specified by Offset, Width, and Count
                                 is not valid for the UFS host controller memory space.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsMemHcMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

/**
  Enable a UFS bus driver to access UFS MMIO registers in the UFS Host Controller
  memory space.

  @param[in]      This    A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]      Width   Signifies the width of the memory operations.
  @param[in]      Offset  The offset within the UFS Host Controller MMIO space to start
                          the memory operation.
  @param[in]      Count   The number of memory operations to perform.
  @param[in, out] Buffer  For write operations, the source buffer to write data from.

  @retval EFI_SUCCESS            The data was written to the UFS host controller.
  @retval EFI_UNSUPPORTED        The address range specified by Offset, Width, and Count
                                 is not valid for the UFS host controller memory space.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.

**/
EFI_STATUS
EFIAPI
UfsMemHcMmioWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

//
// ComponentName function declarations (implemented in ComponentName.c)
//

/**
  Retrieves a Unicode string that is the user readable name of the driver.

  @param[in]   This        A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                           EFI_COMPONENT_NAME_PROTOCOL instance.
  @param[in]   Language    A pointer to a Null-terminated ASCII string array
                           indicating the language.
  @param[out]  DriverName  A pointer to the Unicode string to return.

  @retval EFI_SUCCESS            The Unicode string for the Driver specified by
                                 This and the language specified by Language
                                 was returned in DriverName.
  @retval EFI_INVALID_PARAMETER  Language or DriverName is NULL.
  @retval EFI_UNSUPPORTED        The driver specified by This does not support
                                 the language specified by Language.

**/
EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  );

/**
  Retrieves a Unicode string that is the user readable name of the controller
  that is being managed by a driver.

  @param[in]   This              A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                                 EFI_COMPONENT_NAME_PROTOCOL instance.
  @param[in]   ControllerHandle  The handle of a controller that the driver
                                 specified by This is managing.
  @param[in]   ChildHandle       The handle of the child controller. Must be NULL
                                 for this device driver.
  @param[in]   Language          A pointer to a Null-terminated ASCII string
                                 array indicating the language.
  @param[out]  ControllerName    A pointer to the Unicode string to return.

  @retval EFI_SUCCESS            The Unicode string for the controller was
                                 returned in ControllerName.
  @retval EFI_INVALID_PARAMETER  Language or ControllerName is NULL.
  @retval EFI_UNSUPPORTED        ChildHandle is not NULL, the driver is not
                                 managing ControllerHandle, or the language is
                                 not supported.

**/
EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  );
