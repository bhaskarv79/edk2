/** @file
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL implementation for UfsMemHcDxe.

  Provides MMIO register access and DMA buffer management for a memory-mapped
  UFS host controller.  Cache maintenance uses CacheMaintenanceLib so this
  implementation is suitable for non-coherent DMA platforms (ARM, etc.).

  Copyright (c) 2024, EDK2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include "UfsMemHcDxe.h"
#include <Protocol/Cpu.h>
#include <Library/DxeServicesTableLib.h>

//
// Conservative fallback used only if EFI_CPU_ARCH_PROTOCOL is unavailable or
// reports 0 -- the largest data cache line size in common use, so the
// alignment check below never under-estimates the true line size.
//
#define UFS_MEM_HC_DEFAULT_DMA_ALIGNMENT  64

STATIC UINT32  mUfsMemHcDmaBufferAlignment = 0;

/**
  Returns the platform's required DMA buffer alignment (typically the CPU
  data cache line size), caching the value after the first successful
  lookup.

  @return The alignment, in bytes.  Always a power of two.

**/
STATIC
UINT32
UfsMemHcGetDmaBufferAlignment (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;

  if (mUfsMemHcDmaBufferAlignment != 0) {
    return mUfsMemHcDmaBufferAlignment;
  }

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (!EFI_ERROR (Status) && (Cpu->DmaBufferAlignment != 0)) {
    mUfsMemHcDmaBufferAlignment = Cpu->DmaBufferAlignment;
  } else {
    mUfsMemHcDmaBufferAlignment = UFS_MEM_HC_DEFAULT_DMA_ALIGNMENT;
  }

  return mUfsMemHcDmaBufferAlignment;
}

/**
  Returns the MMIO base address of the UFS host controller.

  @param[in]   This     Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[out]  MmioBar  Receives the MMIO base address.

  @retval EFI_SUCCESS            MmioBar filled in.
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
  *MmioBar = (UINTN)Private->DevProtocol->HciMmioBase;

  return EFI_SUCCESS;
}

/**
  Maps a host memory region for DMA and performs pre-DMA cache maintenance.

  For CommonBuffer operations (UTRL/UPIU descriptors already mapped as UC via
  AllocateBuffer) no cache work is needed and the mapping token is NULL.

  For BusMasterRead and BusMasterWrite a UFS_MEM_HC_MAP_INFO token is
  allocated and WriteBackDataCacheRange() is called to flush dirty CPU cache
  lines to DRAM before the DMA engine accesses the buffer.  If HostAddress or
  NumberOfBytes does not fill whole CPU cache lines, the transfer is
  double-buffered into a cache-line-aligned allocation instead -- running
  cache maintenance directly on a partial cache line risks corrupting
  unrelated data sharing that line, the same hazard EmbeddedPkg's
  NonCoherentDmaLib double-buffers to avoid.

  @param[in]     This           Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]     Operation      DMA direction.
  @param[in]     HostAddress    Host virtual address of the buffer.
  @param[in,out] NumberOfBytes  Bytes to map; unchanged on return.
  @param[out]    DeviceAddress  Bus/physical address for the DMA engine.
  @param[out]    Mapping        Opaque token passed to Unmap().

  @retval EFI_SUCCESS            Buffer mapped.
  @retval EFI_INVALID_PARAMETER  A required pointer parameter is NULL.
  @retval EFI_OUT_OF_RESOURCES   Cannot allocate the map-info token or the
                                 double-buffer.

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
  UFS_MEM_HC_MAP_INFO  *MapInfo;
  UINT32               Alignment;
  UINTN                AlignedSize;
  VOID                 *DmaBuffer;

  AlignedSize = 0;
  DmaBuffer   = NULL;

  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // CommonBuffer: UTRL/UPIU descriptors are already UC (set in AllocateBuffer).
  // No cache maintenance is required; return an identity mapping with NULL token.
  //
  if (Operation == EdkiiUfsHcOperationBusMasterCommonBuffer) {
    *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
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

  Alignment = UfsMemHcGetDmaBufferAlignment ();

  if ((((UINTN)HostAddress & (Alignment - 1)) != 0) ||
      ((*NumberOfBytes & (Alignment - 1)) != 0))
  {
    //
    // HostAddress/NumberOfBytes do not fill whole cache lines -- double
    // buffer into a cache-line-aligned allocation so the WriteBack/Invalidate
    // below never touches memory outside this transfer.
    //
    AlignedSize = ALIGN_VALUE (*NumberOfBytes, Alignment);
    DmaBuffer   = AllocateAlignedPages (EFI_SIZE_TO_PAGES (AlignedSize), Alignment);
    if (DmaBuffer == NULL) {
      FreePool (MapInfo);
      return EFI_OUT_OF_RESOURCES;
    }

    if (Operation == EdkiiUfsHcOperationBusMasterRead) {
      CopyMem (DmaBuffer, HostAddress, *NumberOfBytes);
    }

    MapInfo->DoubleBuffer  = TRUE;
    MapInfo->BufferAddress = DmaBuffer;
    *DeviceAddress         = (EFI_PHYSICAL_ADDRESS)(UINTN)DmaBuffer;
  } else {
    //
    // Identity map: the UFS controller and CPU share the same physical address
    // space on non-IOMMU platforms.
    //
    *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  }

  *Mapping = MapInfo;

  //
  // Flush CPU write-back cache to DRAM so the DMA engine sees the current
  // buffer contents (required for both read and write DMA directions to avoid
  // a stale dirty line being written back mid-DMA).
  //
  WriteBackDataCacheRange (
    MapInfo->DoubleBuffer ? MapInfo->BufferAddress : HostAddress,
    MapInfo->DoubleBuffer ? AlignedSize : *NumberOfBytes
    );

  return EFI_SUCCESS;
}

/**
  Completes a Map() operation and performs post-DMA cache maintenance.

  For BusMasterWrite (device → host, e.g. READ response data), invalidates the
  cache lines covering the buffer so that the CPU reads DMA-written data from
  DRAM rather than stale cached content.  If Map() double-buffered the
  transfer (see UfsMemHcMap()), copies the result back into the caller's
  buffer and frees the double buffer.  Frees the map-info token.

  @param[in]  This     Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]  Mapping  Token returned by Map().  NULL means CommonBuffer.

  @retval EFI_SUCCESS            Unmapped successfully.
  @retval EFI_INVALID_PARAMETER  This is NULL.

**/
EFI_STATUS
EFIAPI
UfsMemHcUnmap (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  VOID                                *Mapping
  )
{
  UFS_MEM_HC_MAP_INFO  *MapInfo;
  UINTN                AlignedSize;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // NULL token means CommonBuffer — already UC, nothing to do.
  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }

  MapInfo = (UFS_MEM_HC_MAP_INFO *)Mapping;

  if (MapInfo->DoubleBuffer) {
    AlignedSize = ALIGN_VALUE (MapInfo->NumberOfBytes, UfsMemHcGetDmaBufferAlignment ());

    //
    // For device-to-host DMA (BusMasterWrite), invalidate the double buffer's
    // cache lines so the CopyMem below reads DMA-written data from DRAM, then
    // copy the result back into the caller's original buffer.
    //
    if (MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) {
      InvalidateDataCacheRange (MapInfo->BufferAddress, AlignedSize);
      CopyMem (MapInfo->HostAddress, MapInfo->BufferAddress, MapInfo->NumberOfBytes);
    }

    FreeAlignedPages (MapInfo->BufferAddress, EFI_SIZE_TO_PAGES (AlignedSize));
  } else if (MapInfo->Operation == EdkiiUfsHcOperationBusMasterWrite) {
    //
    // For device-to-host DMA (BusMasterWrite), invalidate the CPU cache lines
    // covering the buffer so the next CPU read fetches from DRAM.
    //
    InvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->NumberOfBytes);
  }

  FreePool (MapInfo);
  return EFI_SUCCESS;
}

/**
  Allocates DMA-safe pages and remaps them as uncached (EFI_MEMORY_UC).

  Uncached mapping ensures the UFS DMA engine and the CPU always see the same
  view of UTRL/UPIU descriptor memory without requiring explicit cache
  maintenance at every doorbell ring.  This implementation always produces an
  EFI_MEMORY_UC buffer and does not implement the optional
  MEMORY_WRITE_COMBINE / MEMORY_CACHED attributes the protocol allows a
  caller to request; a non-zero Attributes is rejected rather than silently
  honoring the default UC behavior instead of what was actually requested.

  @param[in]  This         Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]  Type         Allocation type (not used; AllocateAnyPages assumed).
  @param[in]  MemoryType   EfiBootServicesData or EfiRuntimeServicesData.
  @param[in]  Pages        Number of 4 KiB pages to allocate.
  @param[out] HostAddress  Receives the host virtual address of the allocation.
  @param[in]  Attributes   Must be 0; MEMORY_WRITE_COMBINE/MEMORY_CACHED are
                           not implemented by this driver.

  @retval EFI_SUCCESS            Pages allocated and remapped UC.
  @retval EFI_INVALID_PARAMETER  HostAddress is NULL or MemoryType is invalid.
  @retval EFI_UNSUPPORTED        Attributes is non-zero.
  @retval EFI_OUT_OF_RESOURCES   AllocatePages failed.
  @retval EFI_DEVICE_ERROR       Buffer could not be remapped uncached; the
                                 caller's CommonBuffer Map/Unmap path assumes
                                 AllocateBuffer memory is already coherent, so
                                 a cacheable buffer here would risk DMA
                                 corruption on non-coherent platforms.

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
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MemoryType != EfiBootServicesData) &&
      (MemoryType != EfiRuntimeServicesData))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Attributes != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] AllocateBuffer: Attributes=0x%lx not supported; this driver "
      "only produces EFI_MEMORY_UC common buffers\n",
      Attributes
      ));
    return EFI_UNSUPPORTED;
  }

  Address = MAX_UINT64;
  Status  = gBS->AllocatePages (
                   AllocateAnyPages,
                   MemoryType,
                   Pages,
                   &Address
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] AllocateBuffer: AllocatePages failed Pages=%d - %r\n",
      Pages,
      Status
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] AllocateBuffer: GetMemorySpaceDescriptor failed addr=0x%lx - %r\n",
      Address,
      Status
      ));
    gBS->FreePages (Address, Pages);
    return EFI_DEVICE_ERROR;
  }

  if ((GcdDescriptor.Capabilities & EFI_MEMORY_UC) == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] AllocateBuffer: EFI_MEMORY_UC not in GCD caps; refusing cacheable DMA buffer\n"
      ));
    gBS->FreePages (Address, Pages);
    return EFI_DEVICE_ERROR;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  Address,
                  EFI_PAGES_TO_SIZE (Pages),
                  (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] AllocateBuffer: UC remap failed addr=0x%lx - %r\n",
      Address,
      Status
      ));
    gBS->FreePages (Address, Pages);
    return EFI_DEVICE_ERROR;
  }

  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;
}

/**
  Frees pages previously allocated with AllocateBuffer().

  Restores the memory space attributes to write-back before returning pages to
  the pool so they are reusable for cached allocation later.

  @param[in]  This         Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]  Pages        Number of pages to free.
  @param[in]  HostAddress  Base address returned by AllocateBuffer().

  @retval EFI_SUCCESS            Pages freed.
  @retval EFI_INVALID_PARAMETER  HostAddress is NULL or FreePages failed.

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
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;

  Status = gDS->GetMemorySpaceDescriptor (Address, &GcdDescriptor);
  if (!EFI_ERROR (Status) &&
      ((GcdDescriptor.Attributes & EFI_CACHE_ATTRIBUTE_MASK) == EFI_MEMORY_UC))
  {
    gDS->SetMemorySpaceAttributes (
           Address,
           EFI_PAGES_TO_SIZE (Pages),
           (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WB
           );
  }

  Status = gBS->FreePages (Address, Pages);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] FreeBuffer: FreePages failed addr=0x%lx Pages=%d - %r\n",
      Address,
      Pages,
      Status
      ));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Flushes posted write transactions.

  Memory-mapped UFS controllers on ARM platforms do not maintain a separate
  posted-write queue visible to the driver; returning EFI_SUCCESS is correct.

  @param[in]  This  Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.

  @retval EFI_SUCCESS  Always.

**/
EFI_STATUS
EFIAPI
UfsMemHcFlush (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

/**
  Reads UFS host controller MMIO registers.

  @param[in]     This    Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]     Width   Register width (only EfiUfsHcWidthUint32 supported).
  @param[in]     Offset  Byte offset from the HCI MMIO base of the first register.
  @param[in]     Count   Number of consecutive 32-bit registers to read, starting
                         at Offset and advancing by sizeof(UINT32) each iteration.
  @param[in,out] Buffer  Destination buffer for read data.

  @retval EFI_SUCCESS            Data read.
  @retval EFI_INVALID_PARAMETER  This, Buffer is NULL, or Count is 0.
  @retval EFI_UNSUPPORTED        Width is not EfiUfsHcWidthUint32.

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
  UINTN                    Index;
  UFS_MEM_HC_PRIVATE_DATA  *Private;

  if ((This == NULL) || (Buffer == NULL) || (Count == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Width != EfiUfsHcWidthUint32) {
    return EFI_UNSUPPORTED;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);

  for (Index = 0; Index < Count; Index++) {
    *((UINT32 *)Buffer + Index) =
      MmioRead32 (
        (UINTN)Private->DevProtocol->HciMmioBase + (UINTN)Offset +
        Index * sizeof (UINT32)
        );
  }

  return EFI_SUCCESS;
}

/**
  Writes UFS host controller MMIO registers.

  @param[in]     This    Pointer to EDKII_UFS_HOST_CONTROLLER_PROTOCOL.
  @param[in]     Width   Register width (only EfiUfsHcWidthUint32 supported).
  @param[in]     Offset  Byte offset from the HCI MMIO base of the first register.
  @param[in]     Count   Number of consecutive 32-bit registers to write, starting
                         at Offset and advancing by sizeof(UINT32) each iteration.
  @param[in,out] Buffer  Source buffer of values to write.

  @retval EFI_SUCCESS            Data written.
  @retval EFI_INVALID_PARAMETER  This, Buffer is NULL, or Count is 0.
  @retval EFI_UNSUPPORTED        Width is not EfiUfsHcWidthUint32.

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
  UINTN                    Index;
  UFS_MEM_HC_PRIVATE_DATA  *Private;

  if ((This == NULL) || (Buffer == NULL) || (Count == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Width != EfiUfsHcWidthUint32) {
    return EFI_UNSUPPORTED;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (This);

  for (Index = 0; Index < Count; Index++) {
    MmioWrite32 (
      (UINTN)Private->DevProtocol->HciMmioBase + (UINTN)Offset +
      Index * sizeof (UINT32),
      *((UINT32 *)Buffer + Index)
      );
  }

  return EFI_SUCCESS;
}
