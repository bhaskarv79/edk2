/** @file
  Register the UFS host controllers described in the device tree as
  non-discoverable UFS MMIO devices, for UfsMemHcDxe.

  Every node compatible with "jedec,ufs-2.0" whose status is okay is
  mapped as device memory and registered with its first "reg" entry as the
  UFSHCI register window. The
  DMA type comes from PcdUfsMmioDmaNonCoherent: a node marked
  "dma-coherent" is still registered as non-coherent when the PCD is set, so
  that the non-coherent DMA path can be tested on a coherent platform.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MapMmioLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/FdtClient.h>

#define UFS_MMIO_COMPATIBLE  "jedec,ufs-2.0"

/**
  Register one UFS controller node.

  @param[in] FdtClient  FDT client protocol.
  @param[in] Node       Offset of the controller node.

  @retval EFI_SUCCESS  The controller was registered.
  @retval Others       The node was skipped; the reason is logged.
**/
STATIC
EFI_STATUS
RegisterUfsNode (
  IN FDT_CLIENT_PROTOCOL  *FdtClient,
  IN INT32                Node
  )
{
  EFI_STATUS                        Status;
  CONST VOID                        *Reg;
  UINT32                            RegSize;
  UINT64                            Base;
  UINT64                            Size;
  NON_DISCOVERABLE_DEVICE_DMA_TYPE  DmaType;

  Status = FdtClient->GetNodeProperty (FdtClient, Node, "reg", &Reg, &RegSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no reg property: %r\n", gEfiCallerBaseName, Status));
    return Status;
  }

  //
  // The QEMU and Arm virt device trees use two address and two size cells.
  //
  if (RegSize < 2 * sizeof (UINT64)) {
    DEBUG ((DEBUG_ERROR, "%a: reg property too short (%u bytes)\n", gEfiCallerBaseName, RegSize));
    return EFI_INVALID_PARAMETER;
  }

  Base = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Reg));
  Size = SwapBytes64 (ReadUnaligned64 ((CONST UINT64 *)Reg + 1));
  if ((Base == 0) || (Size == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: invalid reg 0x%lx size 0x%lx\n", gEfiCallerBaseName, Base, Size));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Nothing else maps the controller's register window on this platform.
  //
  Status = MapMmioMemory (Base, Size, EFI_MEMORY_UC | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: mapping 0x%lx failed: %r\n", gEfiCallerBaseName, Base, Status));
    return Status;
  }

  DmaType = FeaturePcdGet (PcdUfsMmioDmaNonCoherent) ?
            NonDiscoverableDeviceDmaTypeNonCoherent :
            NonDiscoverableDeviceDmaTypeCoherent;

  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeUfsMmio,
             DmaType,
             NULL,
             NULL,
             1,
             Base,
             Size
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: registering 0x%lx failed: %r\n", gEfiCallerBaseName, Base, Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: UFS MMIO controller at 0x%lx size 0x%lx (%a DMA)\n",
    gEfiCallerBaseName,
    Base,
    Size,
    DmaType == NonDiscoverableDeviceDmaTypeNonCoherent ? "non-coherent" : "coherent"
    ));
  return EFI_SUCCESS;
}

/**
  Entry point: register every enabled UFS controller in the device tree.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to the EFI system table.

  @retval EFI_SUCCESS  Always; a platform without UFS registers nothing.
**/
EFI_STATUS
EFIAPI
UfsMmioDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  UINTN                Count;

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&FdtClient);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  Count = 0;
  for (Status = FdtClient->FindCompatibleNode (FdtClient, UFS_MMIO_COMPATIBLE, &Node);
       !EFI_ERROR (Status);
       Status = FdtClient->FindNextCompatibleNode (FdtClient, UFS_MMIO_COMPATIBLE, Node, &Node))
  {
    if (!EFI_ERROR (RegisterUfsNode (FdtClient, Node))) {
      Count++;
    }
  }

  if (Count == 0) {
    DEBUG ((DEBUG_INFO, "%a: no UFS controller in the device tree\n", gEfiCallerBaseName));
  }

  return EFI_SUCCESS;
}
