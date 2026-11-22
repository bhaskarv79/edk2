/** @file
  Lab UEFI Shell application: Block I/O data test on a UFS logical unit.

  Finds the Block I/O of UFS LUN 1 (the lab scratch LU), then for transfers
  of one block, 128 KiB and 1 MiB at the first, middle and last LBAs writes
  a pattern, flushes, reads back into a cleared buffer and compares. The
  buffers are taken above 4 GiB when there is memory there, so that on a
  controller without 64-bit addressing the UFS stack has to map them below
  4 GiB.

  Prints "UfsBlkTest: PASS <n> cases" or "UfsBlkTest: FAIL ..." and returns
  EFI_SUCCESS only if every case passes.

  Lab code, not intended for upstream.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define UFS_BLK_TEST_LUN  1

STATIC CONST UINTN  mSizes[] = { 0, SIZE_128KB, SIZE_1MB };  // 0: one block

/**
  Find the Block I/O of a UFS logical unit (not a partition).

  @param[in]  Lun     Logical unit number.
  @param[out] BlockIo Block I/O of that unit.

  @retval EFI_SUCCESS    Found.
  @retval EFI_NOT_FOUND  No such unit.
**/
STATIC
EFI_STATUS
FindUfsLun (
  IN  UINT8                  Lun,
  OUT EFI_BLOCK_IO_PROTOCOL  **BlockIo
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_BLOCK_IO_PROTOCOL     *Bio;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Node)) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio)) ||
        Bio->Media->LogicalPartition)
    {
      continue;
    }

    for ( ; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) && (DevicePathSubType (Node) == MSG_UFS_DP) &&
          (((UFS_DEVICE_PATH *)Node)->Lun == Lun))
      {
        *BlockIo = Bio;
        Status   = EFI_SUCCESS;
        break;
      }
    }

    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Allocate pages above 4 GiB when the memory map has free memory there, so
  that a controller without 64-bit addressing needs bounce buffers;
  otherwise allocate anywhere.

  @param[in] Pages  Number of pages.

  @return The buffer, or NULL.
**/
STATIC
VOID *
AllocateHighPages (
  IN UINTN  Pages
  )
{
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  EFI_PHYSICAL_ADDRESS   Address;
  EFI_PHYSICAL_ADDRESS   Start;
  EFI_PHYSICAL_ADDRESS   End;
  EFI_STATUS             Status;
  UINTN                  Offset;

  MapSize = 0;
  Map     = NULL;
  Status  = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    MapSize += 8 * DescSize;
    Map      = AllocatePool (MapSize);
    if (Map != NULL) {
      Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
    }
  }

  if (!EFI_ERROR (Status) && (Map != NULL)) {
    for (Offset = 0; Offset < MapSize; Offset += DescSize) {
      Desc  = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      Start = MAX (Desc->PhysicalStart, BASE_4GB);
      End   = Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages);
      if ((Desc->Type != EfiConventionalMemory) || (End <= Start) ||
          (End - Start < EFI_PAGES_TO_SIZE (Pages)))
      {
        continue;
      }

      Address = Start;
      if (!EFI_ERROR (gBS->AllocatePages (AllocateAddress, EfiBootServicesData, Pages, &Address))) {
        FreePool (Map);
        return (VOID *)(UINTN)Address;
      }
    }
  }

  if (Map != NULL) {
    FreePool (Map);
  }

  return AllocatePages (Pages);
}

/**
  Fill a buffer with a pattern that depends on the LBA and a seed.

  @param[out] Buffer     Buffer.
  @param[in]  Size       Size in bytes, a multiple of 4.
  @param[in]  Lba        First LBA of the transfer.
  @param[in]  Seed       Case number.
**/
STATIC
VOID
FillPattern (
  OUT UINT32  *Buffer,
  IN  UINTN   Size,
  IN  EFI_LBA Lba,
  IN  UINT32  Seed
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size / sizeof (UINT32); Index++) {
    Buffer[Index] = (UINT32)(Lba * 0x9E3779B1u) ^ (UINT32)Index ^ (Seed << 24);
  }
}

/**
  Shell application entry point.

  @param[in] ImageHandle  Image handle.
  @param[in] SystemTable  System table.

  @retval EFI_SUCCESS  Every case passed.
  @retval Others       A case failed or no LU was found.
**/
EFI_STATUS
EFIAPI
UfsBlkTestMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *Bio;
  UINT32                 BlockSize;
  EFI_LBA                LastLba;
  EFI_LBA                Lbas[3];
  UINTN                  SizeIndex;
  UINTN                  LbaIndex;
  UINTN                  Size;
  UINTN                  Pages;
  UINT32                 *Write;
  UINT32                 *Read;
  UINT32                 Cases;

  Status = FindUfsLun (UFS_BLK_TEST_LUN, &Bio);
  if (EFI_ERROR (Status)) {
    Print (L"UfsBlkTest: FAIL no UFS LUN %u\n", UFS_BLK_TEST_LUN);
    return Status;
  }

  BlockSize = Bio->Media->BlockSize;
  LastLba   = Bio->Media->LastBlock;
  Print (L"UfsBlkTest: LUN %u, %u-byte blocks, %lu blocks, read-only %d\n", UFS_BLK_TEST_LUN, BlockSize, LastLba + 1, Bio->Media->ReadOnly);
  if (Bio->Media->ReadOnly || (BlockSize == 0) || (LastLba < SIZE_1MB / BlockSize * 3)) {
    Print (L"UfsBlkTest: FAIL unusable LU\n");
    return EFI_UNSUPPORTED;
  }

  Pages = EFI_SIZE_TO_PAGES (SIZE_1MB);
  Write = AllocateHighPages (Pages);
  Read  = AllocateHighPages (Pages);
  if ((Write == NULL) || (Read == NULL)) {
    Print (L"UfsBlkTest: FAIL out of memory\n");
    return EFI_OUT_OF_RESOURCES;
  }

  Print (L"UfsBlkTest: buffers at 0x%lx and 0x%lx\n", (UINT64)(UINTN)Write, (UINT64)(UINTN)Read);
  Cases = 0;
  for (SizeIndex = 0; SizeIndex < ARRAY_SIZE (mSizes); SizeIndex++) {
    Size    = (mSizes[SizeIndex] == 0) ? BlockSize : mSizes[SizeIndex];
    Lbas[0] = 0;
    Lbas[1] = (LastLba + 1) / 2;
    Lbas[2] = LastLba + 1 - Size / BlockSize;
    for (LbaIndex = 0; LbaIndex < ARRAY_SIZE (Lbas); LbaIndex++) {
      FillPattern (Write, Size, Lbas[LbaIndex], Cases + 1);
      Status = Bio->WriteBlocks (Bio, Bio->Media->MediaId, Lbas[LbaIndex], Size, Write);
      if (!EFI_ERROR (Status)) {
        Status = Bio->FlushBlocks (Bio);
      }

      if (!EFI_ERROR (Status)) {
        ZeroMem (Read, Size);
        Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lbas[LbaIndex], Size, Read);
      }

      if (EFI_ERROR (Status) || (CompareMem (Write, Read, Size) != 0)) {
        Print (L"UfsBlkTest: FAIL %lu bytes at LBA %lu: %r\n", (UINT64)Size, Lbas[LbaIndex], Status);
        return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
      }

      Cases++;
    }
  }

  FreePages (Write, Pages);
  FreePages (Read, Pages);
  Print (L"UfsBlkTest: PASS %u cases\n", Cases);
  return EFI_SUCCESS;
}
