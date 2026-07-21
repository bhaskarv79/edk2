/** @file
  Private header for the generic MMIO-based UFS Host Controller DXE driver.

  Copyright (c) 2024, EDK2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef UFS_MEM_HC_DXE_H_
#define UFS_MEM_HC_DXE_H_

#include <PiDxe.h>
#include <Uefi.h>

#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsMemHcDevice.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>

extern EFI_DRIVER_BINDING_PROTOCOL   gUfsMemHcDriverBinding;
extern EFI_COMPONENT_NAME_PROTOCOL   gUfsMemHcComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  gUfsMemHcComponentName2;

#define UFS_MEM_HC_PRIVATE_DATA_SIGNATURE  SIGNATURE_32 ('U', 'F', 'S', 'M')

///
/// Map token passed back through Mapping in UfsMemHcMap() and consumed by
/// UfsMemHcUnmap().  Tracks the host address and byte count so Unmap() can
/// perform the correct post-DMA cache maintenance.
///
typedef struct {
  VOID                                   *HostAddress;
  UINTN                                  NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION    Operation;
} UFS_MEM_HC_MAP_INFO;

///
/// Per-controller private context allocated in Start() and freed in Stop().
///
typedef struct {
  UINT32                                Signature;

  ///
  /// Produced protocol — embedded so CR() can recover Private from UfsHc ptr.
  ///
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL    UfsHc;

  ///
  /// Pointer to the device protocol installed by the vendor platform driver.
  ///
  EDKII_UFS_MEM_HC_DEVICE_PROTOCOL      *DevProtocol;
} UFS_MEM_HC_PRIVATE_DATA;

#define UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC(a) \
  CR (a, UFS_MEM_HC_PRIVATE_DATA, UfsHc, UFS_MEM_HC_PRIVATE_DATA_SIGNATURE)

//
// DriverBinding function declarations
//
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UfsMemHcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
UfsMemHcDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  );

//
// HC protocol function declarations (implemented in UfsMemHc.c)
//
EFI_STATUS
EFIAPI
UfsMemHcGetMmioBar (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT    UINTN                               *MmioBar
  );

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

EFI_STATUS
EFIAPI
UfsMemHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  );

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

EFI_STATUS
EFIAPI
UfsMemHcFreeBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  UINTN                               Pages,
  IN  VOID                                *HostAddress
  );

EFI_STATUS
EFIAPI
UfsMemHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  );

EFI_STATUS
EFIAPI
UfsMemHcMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

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
EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  );

EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  );

#endif // UFS_MEM_HC_DXE_H_
