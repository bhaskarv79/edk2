/** @file
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL implementation for UfsMemHcDxe.

  Provides MMIO register access and DMA buffer management for a memory-mapped
  UFS host controller. The non-discoverable device's DmaType selects between
  coherent DMA and non-coherent DMA, where common buffers are mapped uncached
  and streaming buffers get CPU cache maintenance. Controllers without 64-bit
  addressing have their DMA buffers kept below 4 GB.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsMemHcDxe.h"

/**
  Allocate pages the controller can reach by DMA.

  @param[in]   Private     The controller private data.
  @param[in]   MemoryType  The type of memory to allocate.
  @param[in]   Pages       The number of pages to allocate.
  @param[out]  Address     The base address of the allocated pages.

  @retval EFI_SUCCESS  The pages were allocated.
  @retval other        AllocatePages() failed.

**/
STATIC
EFI_STATUS
UfsMemHcAllocateDmaPages (
  IN  UFS_MEM_HC_PRIVATE_DATA  *Private,
  IN  EFI_MEMORY_TYPE          MemoryType,
  IN  UINTN                    Pages,
  OUT EFI_PHYSICAL_ADDRESS     *Address
  )
{
  if (Private->Dma64) {
    *Address = MAX_UINT64;
    return gBS->AllocatePages (AllocateAnyPages, MemoryType, Pages, Address);
  }

  *Address = BASE_4GB - 1;
  return gBS->AllocatePages (AllocateMaxAddress, MemoryType, Pages, Address);
}

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
  Decide whether a Map() buffer must be bounced through a page aligned
  buffer the controller can reach, rather than used in place.

  @param[in]  Private        The controller private data.
  @param[in]  Operation      Indicates if the bus master is going to read or write to system memory.
  @param[in]  Address        The system memory address to map to the UFS controller.
  @param[in]  NumberOfBytes  The number of bytes to map.
  @param[in]  Above4GB       TRUE if Address..Address+NumberOfBytes is not entirely
                             below 4 GB on a controller without 64-bit addressing.

  @retval TRUE   The buffer must be bounced.
  @retval FALSE  The buffer can be used in place.

**/
STATIC
BOOLEAN
UfsMemHcNeedBounce (
  IN  UFS_MEM_HC_PRIVATE_DATA              *Private,
  IN  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN  EFI_PHYSICAL_ADDRESS                 Address,
  IN  UINTN                                NumberOfBytes,
  IN  BOOLEAN                              Above4GB
  )
{
  if (NumberOfBytes == 0) {
    return FALSE;
  }

  if (Above4GB) {
    return TRUE;
  }

  //
  // Invalidating cache lines that the buffer only partly covers could
  // discard dirty data belonging to whatever shares those lines, so a
  // device-to-host buffer must fill whole cache lines to be used in place.
  // Writing back a partial line is harmless, so host-to-device buffers do
  // not need this.
  //
  if (!Private->DmaCoherent &&
      (Operation == EdkiiUfsHcOperationBusMasterWrite) &&
      (((Address | NumberOfBytes) & (Private->DmaAlignment - 1)) != 0))
  {
    return TRUE;
  }

  return FALSE;
}

/**
  Allocate a page aligned bounce buffer the controller can reach for a
  Map() call and prime it with the caller's data.

  @param[in]      Private        The controller private data.
  @param[in]      Operation      Indicates if the bus master is going to read or write to system memory.
  @param[in]      HostAddress    The system memory address being mapped.
  @param[in]      NumberOfBytes  The number of bytes to map.
  @param[in, out] MapInfo        The mapping being built; BouncePages and BounceBuffer
                                 are filled in on success.
  @param[out]     DmaBuffer      The bounce buffer the controller should use.

  @retval EFI_SUCCESS           The bounce buffer was allocated and primed.
  @retval EFI_OUT_OF_RESOURCES  The bounce buffer could not be allocated.

**/
STATIC
EFI_STATUS
UfsMemHcBounceBuffer (
  IN     UFS_MEM_HC_PRIVATE_DATA              *Private,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN     UINTN                                NumberOfBytes,
  IN OUT UFS_MEM_HC_MAP_INFO                  *MapInfo,
  OUT    VOID                                 **DmaBuffer
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  BounceAddress;

  MapInfo->BouncePages = EFI_SIZE_TO_PAGES (NumberOfBytes);
  Status               = UfsMemHcAllocateDmaPages (
                           Private,
                           EfiBootServicesData,
                           MapInfo->BouncePages,
                           &BounceAddress
                           );
  if (EFI_ERROR (Status)) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->BounceBuffer = (VOID *)(UINTN)BounceAddress;
  *DmaBuffer            = MapInfo->BounceBuffer;

  if (Operation == EdkiiUfsHcOperationBusMasterRead) {
    CopyMem (*DmaBuffer, HostAddress, NumberOfBytes);
  } else {
    //
    // Unmap() copies NumberOfBytes back to the caller even if the
    // controller wrote less, so do not leave stale page contents there.
    //
    ZeroMem (*DmaBuffer, NumberOfBytes);
  }

  return EFI_SUCCESS;
}

/**
  Perform the CPU cache maintenance a Map()'d buffer needs on a
  non-coherent platform before the controller accesses it.

  @param[in]  Private        The controller private data.
  @param[in]  Operation      Indicates if the bus master is going to read or write to system memory.
  @param[in]  DmaBuffer      The buffer the controller will access, either in place or bounced.
  @param[in]  NumberOfBytes  The number of bytes the controller will access.

**/
STATIC
VOID
UfsMemHcFlushForDevice (
  IN  UFS_MEM_HC_PRIVATE_DATA              *Private,
  IN  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN  VOID                                 *DmaBuffer,
  IN  UINTN                                NumberOfBytes
  )
{
  if (Private->DmaCoherent) {
    return;
  }

  if (Operation == EdkiiUfsHcOperationBusMasterRead) {
    WriteBackDataCacheRange (DmaBuffer, NumberOfBytes);
  } else {
    //
    // DmaBuffer is cache line aligned here, either in place or as a page
    // aligned bounce buffer, so rounding the length up stays within it.
    //
    WriteBackInvalidateDataCacheRange (
      DmaBuffer,
      ALIGN_VALUE (NumberOfBytes, Private->DmaAlignment)
      );
  }
}

/**
  Provides the UFS controller-specific addresses needed to access system memory.

  Common buffers must come from AllocateBuffer() and are mapped in place.

  Bus master read and write buffers are mapped in place when the controller
  can reach them and, on a non-coherent platform, when cache maintenance on
  them cannot touch unrelated data. Otherwise they are bounced through a
  page-aligned buffer the controller can reach. On a non-coherent platform,
  a bus master read buffer is written back from the CPU cache, and a bus
  master write buffer is written back and invalidated.

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
  )
{
  EFI_STATUS                       Status;
  UFS_MEM_HC_PRIVATE_DATA          *Private;
  UFS_MEM_HC_MAP_INFO              *MapInfo;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_PHYSICAL_ADDRESS             End;
  BOOLEAN                          Above4GB;
  VOID                             *DmaBuffer;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL) ||
      ((UINT32)Operation >= EdkiiUfsHcOperationMaximum))
  {
    return EFI_INVALID_PARAMETER;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);
  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  //
  // Without 64-bit addressing the whole buffer must lie below 4 GB.
  //
  Above4GB = FALSE;
  if (!Private->Dma64) {
    End      = Address + *NumberOfBytes;
    Above4GB = (BOOLEAN)((End < Address) || (End > SIZE_4GB));
  }

  if (Operation == EdkiiUfsHcOperationBusMasterCommonBuffer) {
    //
    // A common buffer is shared with the controller for its whole lifetime,
    // so it cannot be bounced. AllocateBuffer() places it where the
    // controller can reach it and, if DMA is non-coherent, maps it uncached.
    //
    if (Above4GB) {
      return EFI_UNSUPPORTED;
    }

    DEBUG_CODE_BEGIN ();
    if (!Private->DmaCoherent) {
      Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
      ASSERT_EFI_ERROR (Status);
      ASSERT (
        EFI_ERROR (Status) ||
        ((GcdDescriptor.Attributes & (EFI_MEMORY_WB | EFI_MEMORY_WT)) == 0)
        );
    }

    DEBUG_CODE_END ();

    *DeviceAddress = Address;
    *Mapping       = NULL;
    return EFI_SUCCESS;
  }

  MapInfo = AllocateZeroPool (sizeof (UFS_MEM_HC_MAP_INFO));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->HostAddress   = HostAddress;
  MapInfo->NumberOfBytes = *NumberOfBytes;
  MapInfo->Operation     = Operation;

  DmaBuffer = HostAddress;
  if (UfsMemHcNeedBounce (Private, Operation, Address, *NumberOfBytes, Above4GB)) {
    Status = UfsMemHcBounceBuffer (Private, Operation, HostAddress, *NumberOfBytes, MapInfo, &DmaBuffer);
    if (EFI_ERROR (Status)) {
      FreePool (MapInfo);
      return Status;
    }
  }

  UfsMemHcFlushForDevice (Private, Operation, DmaBuffer, *NumberOfBytes);

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)DmaBuffer;
  *Mapping       = MapInfo;

  return EFI_SUCCESS;
}

/**
  Completes the Map() operation and releases any corresponding resources.

  For a bus master write on a non-coherent platform, the CPU cache lines
  covering the buffer are invalidated so the CPU reads what the controller
  wrote. If Map() bounced the transfer, the data is copied back to the
  caller's buffer and the bounce buffer is freed.

  @param[in]  This     A pointer to the EDKII_UFS_HOST_CONTROLLER_PROTOCOL instance.
  @param[in]  Mapping  The mapping value returned from Map(). NULL for a
                       common buffer.

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
  UFS_MEM_HC_PRIVATE_DATA  *Private;
  UFS_MEM_HC_MAP_INFO      *MapInfo;
  VOID                     *DmaBuffer;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Common buffers are mapped in place without a token.
  //
  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);
  MapInfo = (UFS_MEM_HC_MAP_INFO *)Mapping;

  DmaBuffer = (MapInfo->BounceBuffer != NULL) ? MapInfo->BounceBuffer : MapInfo->HostAddress;

  if (MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) {
    if (!Private->DmaCoherent) {
      InvalidateDataCacheRange (
        DmaBuffer,
        ALIGN_VALUE (MapInfo->NumberOfBytes, Private->DmaAlignment)
        );
    }

    if (MapInfo->BounceBuffer != NULL) {
      CopyMem (MapInfo->HostAddress, MapInfo->BounceBuffer, MapInfo->NumberOfBytes);
    }
  }

  if (MapInfo->BounceBuffer != NULL) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)MapInfo->BounceBuffer, MapInfo->BouncePages);
  }

  FreePool (MapInfo);
  return EFI_SUCCESS;
}

/**
  Allocates pages that are suitable for an EdkiiUfsHcOperationBusMasterCommonBuffer
  mapping.

  The pages are placed where the controller can reach them. If DMA is
  non-coherent they are also remapped uncached, which is what lets Map()
  share them with the controller without cache maintenance.

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
  @retval EFI_OUT_OF_RESOURCES   The memory pages could not be allocated, or could
                                 not be remapped uncached for non-coherent DMA.

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
  EFI_STATUS                       Status;
  UFS_MEM_HC_PRIVATE_DATA          *Private;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

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

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);

  Status = UfsMemHcAllocateDmaPages (Private, MemoryType, Pages, &Address);
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

  if (!Private->DmaCoherent) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
    if (!EFI_ERROR (Status) && ((GcdDescriptor.Capabilities & EFI_MEMORY_UC) == 0)) {
      Status = EFI_UNSUPPORTED;
    }

    if (!EFI_ERROR (Status)) {
      Status = gDS->SetMemorySpaceAttributes (
                      Address,
                      EFI_PAGES_TO_SIZE (Pages),
                      (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
                      );
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: cannot remap 0x%Lx uncached - %r\n",
        __func__,
        Address,
        Status
        ));
      gBS->FreePages (Address, Pages);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;
}

/**
  Frees memory that was allocated with AllocateBuffer().

  If DMA is non-coherent, the pages are remapped write-back before they are
  freed, so they can be reused as ordinary cached memory.

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
  EFI_STATUS                       Status;
  UFS_MEM_HC_PRIVATE_DATA          *Private;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if ((This == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);
  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  if (!Private->DmaCoherent) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
    if (!EFI_ERROR (Status)) {
      if ((GcdDescriptor.Capabilities & EFI_MEMORY_WB) != 0) {
        Status = gDS->SetMemorySpaceAttributes (
                        Address,
                        EFI_PAGES_TO_SIZE (Pages),
                        (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WB
                        );
      } else {
        //
        // Report it like any other remap failure below: the pages are
        // still freed, but go back to the pool uncached.
        //
        Status = EFI_UNSUPPORTED;
      }
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_WARN,
        "%a: cannot remap 0x%Lx write-back - %r\n",
        __func__,
        Address,
        Status
        ));
    }
  }

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
