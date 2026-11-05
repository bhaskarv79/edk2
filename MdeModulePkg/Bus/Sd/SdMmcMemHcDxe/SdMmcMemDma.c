/** @file
  DMA buffer management for SdMmcMemHcDxe.

  Takes the place of the EFI_PCI_IO_PROTOCOL Map(), Unmap(), AllocateBuffer()
  and FreeBuffer() services SdMmcPciHcDxe uses. The non-discoverable device's
  DmaType selects between coherent DMA and non-coherent DMA, where common
  buffers are mapped uncached and streaming buffers get CPU cache
  maintenance. Controllers without 64-bit addressing have their DMA buffers
  kept below 4 GB.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "SdMmcMemHcDxe.h"

/**
  Allocate pages the controller can reach by DMA.

  @param[in]   HcIo     The host controller I/O context.
  @param[in]   Pages    The number of pages to allocate.
  @param[out]  Address  The base address of the allocated pages.

  @retval EFI_SUCCESS  The pages were allocated.
  @retval other        AllocatePages() failed.

**/
STATIC
EFI_STATUS
SdMmcHcIoAllocateDmaPages (
  IN  SD_MMC_HC_IO          *HcIo,
  IN  UINTN                 Pages,
  OUT EFI_PHYSICAL_ADDRESS  *Address
  )
{
  if (HcIo->Dma64) {
    *Address = MAX_UINT64;
    return gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, Address);
  }

  *Address = BASE_4GB - 1;
  return gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, Address);
}

/**
  Provides the controller-specific address needed to access system memory.

  Common buffers must come from SdMmcHcIoAllocateBuffer() and are mapped in
  place.

  Bus master read and write buffers are mapped in place when the controller
  can reach them and, on a non-coherent platform, when cache maintenance on
  them cannot touch unrelated data. Otherwise they are bounced through a
  page-aligned buffer the controller can reach. On a non-coherent platform,
  a bus master read buffer is written back from the CPU cache, and a bus
  master write buffer is written back and invalidated.

  @param[in]      HcIo           The host controller I/O context.
  @param[in]      Operation      The direction of the transfer.
  @param[in]      HostAddress    The system memory address to map.
  @param[in, out] NumberOfBytes  On input the number of bytes to map. On
                                 output the number of bytes that were mapped.
  @param[out]     DeviceAddress  The address the controller must use.
  @param[out]     Mapping        A value to pass to SdMmcHcIoUnmap().

  @retval EFI_SUCCESS            The range was mapped for NumberOfBytes.
  @retval EFI_UNSUPPORTED        HostAddress cannot be mapped as a common
                                 buffer.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES   A bounce buffer could not be allocated.

**/
EFI_STATUS
SdMmcHcIoMap (
  IN     SD_MMC_HC_IO            *HcIo,
  IN     SD_MMC_HC_IO_OPERATION  Operation,
  IN     VOID                    *HostAddress,
  IN OUT UINTN                   *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS    *DeviceAddress,
  OUT    VOID                    **Mapping
  )
{
  EFI_STATUS                       Status;
  SD_MMC_HC_MAP_INFO               *MapInfo;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_PHYSICAL_ADDRESS             End;
  EFI_PHYSICAL_ADDRESS             BounceAddress;
  BOOLEAN                          Above4GB;
  BOOLEAN                          NeedBounce;
  VOID                             *DmaBuffer;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if ((HcIo == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL) ||
      ((UINT32)Operation >= SdMmcHcIoOperationMaximum))
  {
    return EFI_INVALID_PARAMETER;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  //
  // Without 64-bit addressing the whole buffer must lie below 4 GB.
  //
  Above4GB = FALSE;
  if (!HcIo->Dma64) {
    End      = Address + *NumberOfBytes;
    Above4GB = (BOOLEAN)((End < Address) || (End > SIZE_4GB));
  }

  if (Operation == SdMmcHcIoOperationBusMasterCommonBuffer) {
    //
    // A common buffer is shared with the controller for its whole lifetime,
    // so it cannot be bounced. SdMmcHcIoAllocateBuffer() places it where the
    // controller can reach it and, if DMA is non-coherent, maps it uncached.
    //
    if (Above4GB) {
      return EFI_UNSUPPORTED;
    }

    DEBUG_CODE_BEGIN ();
    if (!HcIo->DmaCoherent) {
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

  //
  // Invalidating cache lines that the buffer only partly covers could
  // discard dirty data belonging to whatever shares those lines, so a
  // device-to-host buffer must fill whole cache lines to be used in place.
  // Writing back a partial line is harmless, so host-to-device buffers do
  // not need this.
  //
  NeedBounce = Above4GB;
  if (!HcIo->DmaCoherent &&
      (Operation == SdMmcHcIoOperationBusMasterWrite) &&
      (((Address | *NumberOfBytes) & (HcIo->DmaAlignment - 1)) != 0))
  {
    NeedBounce = TRUE;
  }

  if (*NumberOfBytes == 0) {
    NeedBounce = FALSE;
  }

  MapInfo = AllocateZeroPool (sizeof (SD_MMC_HC_MAP_INFO));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->HostAddress   = HostAddress;
  MapInfo->NumberOfBytes = *NumberOfBytes;
  MapInfo->Operation     = Operation;

  DmaBuffer = HostAddress;
  if (NeedBounce) {
    MapInfo->BouncePages = EFI_SIZE_TO_PAGES (*NumberOfBytes);
    Status               = SdMmcHcIoAllocateDmaPages (HcIo, MapInfo->BouncePages, &BounceAddress);
    if (EFI_ERROR (Status)) {
      FreePool (MapInfo);
      return EFI_OUT_OF_RESOURCES;
    }

    MapInfo->BounceBuffer = (VOID *)(UINTN)BounceAddress;
    DmaBuffer             = MapInfo->BounceBuffer;

    if (Operation == SdMmcHcIoOperationBusMasterRead) {
      CopyMem (DmaBuffer, HostAddress, *NumberOfBytes);
    } else {
      //
      // SdMmcHcIoUnmap() copies NumberOfBytes back to the caller even if the
      // controller wrote less, so do not leave stale page contents there.
      //
      ZeroMem (DmaBuffer, *NumberOfBytes);
    }
  }

  if (!HcIo->DmaCoherent) {
    if (Operation == SdMmcHcIoOperationBusMasterRead) {
      WriteBackDataCacheRange (DmaBuffer, *NumberOfBytes);
    } else {
      //
      // DmaBuffer is cache line aligned here, either in place or as a page
      // aligned bounce buffer, so rounding the length up stays within it.
      //
      WriteBackInvalidateDataCacheRange (
        DmaBuffer,
        ALIGN_VALUE (*NumberOfBytes, HcIo->DmaAlignment)
        );
    }
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)DmaBuffer;
  *Mapping       = MapInfo;

  return EFI_SUCCESS;
}

/**
  Completes a SdMmcHcIoMap() operation and releases its resources.

  For a bus master write on a non-coherent platform, the CPU cache lines
  covering the buffer are invalidated so the CPU reads what the controller
  wrote. If SdMmcHcIoMap() bounced the transfer, the data is copied back to
  the caller's buffer and the bounce buffer is freed.

  @param[in]  HcIo     The host controller I/O context.
  @param[in]  Mapping  The value returned by SdMmcHcIoMap(). NULL for a
                       common buffer.

  @retval EFI_SUCCESS            The range was unmapped.
  @retval EFI_INVALID_PARAMETER  HcIo is NULL.

**/
EFI_STATUS
SdMmcHcIoUnmap (
  IN SD_MMC_HC_IO  *HcIo,
  IN VOID          *Mapping
  )
{
  SD_MMC_HC_MAP_INFO  *MapInfo;
  VOID                *DmaBuffer;

  if (HcIo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Common buffers are mapped in place without a token.
  //
  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }

  MapInfo = (SD_MMC_HC_MAP_INFO *)Mapping;

  DmaBuffer = (MapInfo->BounceBuffer != NULL) ? MapInfo->BounceBuffer : MapInfo->HostAddress;

  if (MapInfo->Operation == SdMmcHcIoOperationBusMasterWrite) {
    if (!HcIo->DmaCoherent) {
      InvalidateDataCacheRange (
        DmaBuffer,
        ALIGN_VALUE (MapInfo->NumberOfBytes, HcIo->DmaAlignment)
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
  Allocates pages suitable for a SdMmcHcIoOperationBusMasterCommonBuffer
  mapping.

  The pages are placed where the controller can reach them. If DMA is
  non-coherent they are also remapped uncached, which is what lets
  SdMmcHcIoMap() share them with the controller without cache maintenance.

  @param[in]   HcIo         The host controller I/O context.
  @param[in]   Pages        The number of pages to allocate.
  @param[out]  HostAddress  The base address of the allocated range.

  @retval EFI_SUCCESS            The pages were allocated.
  @retval EFI_INVALID_PARAMETER  HcIo or HostAddress is NULL.
  @retval EFI_OUT_OF_RESOURCES   The pages could not be allocated, or could
                                 not be remapped uncached.

**/
EFI_STATUS
SdMmcHcIoAllocateBuffer (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINTN         Pages,
  OUT VOID          **HostAddress
  )
{
  EFI_STATUS                       Status;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if ((HcIo == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SdMmcHcIoAllocateDmaPages (HcIo, Pages, &Address);
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

  if (!HcIo->DmaCoherent) {
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
  Frees pages allocated with SdMmcHcIoAllocateBuffer().

  If DMA is non-coherent, the pages are remapped write-back before they are
  freed, so they can be reused as ordinary cached memory.

  @param[in]  HcIo         The host controller I/O context.
  @param[in]  Pages        The number of pages to free.
  @param[in]  HostAddress  The base address of the allocated range.

  @retval EFI_SUCCESS            The pages were freed.
  @retval EFI_INVALID_PARAMETER  HcIo or HostAddress is NULL, or the range
                                 was not allocated with
                                 SdMmcHcIoAllocateBuffer().

**/
EFI_STATUS
SdMmcHcIoFreeBuffer (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINTN         Pages,
  IN VOID          *HostAddress
  )
{
  EFI_STATUS                       Status;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if ((HcIo == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  if (!HcIo->DmaCoherent) {
    Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
    if (!EFI_ERROR (Status) && ((GcdDescriptor.Capabilities & EFI_MEMORY_WB) != 0)) {
      Status = gDS->SetMemorySpaceAttributes (
                      Address,
                      EFI_PAGES_TO_SIZE (Pages),
                      (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WB
                      );
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
