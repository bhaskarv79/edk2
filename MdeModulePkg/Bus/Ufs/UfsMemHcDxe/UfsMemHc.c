/** @file
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL implementation for UfsMemHcDxe.

  Provides MMIO register access and DMA buffer management for a memory-mapped
  UFS host controller whose DMA is cache coherent with the CPU and reaches
  all of system memory, so every buffer is handed to the controller in place.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsMemHcDxe.h"

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
  )
{
  UFS_MEM_HC_PRIVATE_DATA  *Private;

  if ((This == NULL) || (MmioBar == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private  = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);
  *MmioBar = (UINTN)Private->MmioBase;

  return EFI_SUCCESS;
}

/**
  Provides the UFS controller-specific addresses needed to access system memory.

  The controller's DMA is cache coherent and reaches all of system memory,
  so every buffer is mapped in place and no token is needed.

  @param[in]      This           A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]      Operation      Indicates if the bus master is going to read or write to system memory.
  @param[in]      HostAddress    The system memory address to map to the UFS controller.
  @param[in, out] NumberOfBytes  On input the number of bytes to map. On output the number of bytes
                                 that were mapped.
  @param[out]     DeviceAddress  The resulting map address for the bus master UFS controller to use to
                                 access the hosts HostAddress.
  @param[out]     Mapping        A resulting value to pass to Unmap().

  @retval EFI_SUCCESS            The range was mapped for the returned NumberOfBytes.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.

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
  )
{
  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL) ||
      ((UINT32)Operation >= EdkiiUfsHcOperationMaximum))
  {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = NULL;

  return EFI_SUCCESS;
}

/**
  Completes the Map() operation and releases any corresponding resources.

  Map() maps every buffer in place without a token, so there is nothing to
  release.

  @param[in]  This     A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Mapping  The mapping value returned from Map(), always NULL.

  @retval EFI_SUCCESS            The range was unmapped.
  @retval EFI_INVALID_PARAMETER  This is NULL.

**/
EFI_STATUS
EFIAPI
UfsMemHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Allocates pages that are suitable for an EdkiiUfsHcOperationBusMasterCommonBuffer
  mapping.

  As with the PCI root bridge, the MEMORY_WRITE_COMBINE and MEMORY_CACHED
  attributes are accepted as hints and otherwise ignored.

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
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  if ((This == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MemoryType != EfiBootServicesData) &&
      (MemoryType != EfiRuntimeServicesData))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Attributes & ~(UINT64)(EFI_PCI_IO_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                              EFI_PCI_IO_ATTRIBUTE_MEMORY_CACHED)) != 0)
  {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->AllocatePages (AllocateAnyPages, MemoryType, Pages, &Address);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: AllocatePages (0x%Lx pages) failed - %r\n",
      __func__,
      (UINT64)Pages,
      Status
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  if ((This == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  Status = gBS->FreePages (Address, Pages);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: FreePages (0x%Lx, 0x%Lx pages) failed - %r\n",
      __func__,
      Address,
      (UINT64)Pages,
      Status
      ));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Flushes all posted write transactions from the UFS bus to attached UFS device.

  There is no bus between the CPU and the controller to flush, so this only
  orders the CPU's earlier memory and MMIO accesses before later ones.

  @param[in]  This  A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.

  @retval EFI_SUCCESS  The posted write transactions were flushed.

**/
EFI_STATUS
EFIAPI
UfsMemHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  MemoryFence ();
  return EFI_SUCCESS;
}

/**
  Check an MMIO access against the controller's register window.

  @param[in]   This     A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]   Width    Signifies the width of the memory operations.
  @param[in]   Offset   The offset of the first access in the register window.
  @param[in]   Count    The number of memory operations to perform.
  @param[in]   Buffer   The caller's data buffer.
  @param[out]  Address  The address of the first access.

  @retval EFI_SUCCESS            The access is valid.
  @retval EFI_UNSUPPORTED        Offset, Width and Count do not describe an aligned
                                 range within the register window.
  @retval EFI_INVALID_PARAMETER  This or Buffer is NULL, or Width is invalid.

**/
STATIC
EFI_STATUS
UfsMemHcCheckMmioAccess (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN  UINT64                                    Offset,
  IN  UINTN                                     Count,
  IN  VOID                                      *Buffer,
  OUT UINTN                                     *Address
  )
{
  UFS_MEM_HC_PRIVATE_DATA  *Private;
  UINT64                   Size;

  if ((This == NULL) || (Buffer == NULL) ||
      ((UINT32)Width >= EfiUfsHcWidthMaximum))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);
  Size    = LShiftU64 (1, Width);

  if (((Offset & (Size - 1)) != 0) ||
      (Count > DivU64x64Remainder (Private->MmioSize, Size, NULL)) ||
      (Offset > Private->MmioSize - MultU64x64 (Count, Size)))
  {
    return EFI_UNSUPPORTED;
  }

  *Address = (UINTN)(Private->MmioBase + Offset);
  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  UINTN       Address;
  UINTN       Index;

  Status = UfsMemHcCheckMmioAccess (This, Width, Offset, Count, Buffer, &Address);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
      case EfiUfsHcWidthUint8:
        ((UINT8 *)Buffer)[Index] = MmioRead8 (Address);
        break;
      case EfiUfsHcWidthUint16:
        ((UINT16 *)Buffer)[Index] = MmioRead16 (Address);
        break;
      case EfiUfsHcWidthUint32:
        ((UINT32 *)Buffer)[Index] = MmioRead32 (Address);
        break;
      default:
        ((UINT64 *)Buffer)[Index] = MmioRead64 (Address);
        break;
    }

    Address += (UINTN)1 << Width;
  }

  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS  Status;
  UINTN       Address;
  UINTN       Index;

  Status = UfsMemHcCheckMmioAccess (This, Width, Offset, Count, Buffer, &Address);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
      case EfiUfsHcWidthUint8:
        MmioWrite8 (Address, ((UINT8 *)Buffer)[Index]);
        break;
      case EfiUfsHcWidthUint16:
        MmioWrite16 (Address, ((UINT16 *)Buffer)[Index]);
        break;
      case EfiUfsHcWidthUint32:
        MmioWrite32 (Address, ((UINT32 *)Buffer)[Index]);
        break;
      default:
        MmioWrite64 (Address, ((UINT64 *)Buffer)[Index]);
        break;
    }

    Address += (UINTN)1 << Width;
  }

  return EFI_SUCCESS;
}
