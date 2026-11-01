/** @file
  EFI_SD_MMC_PASS_THRU_PROTOCOL vtable, TRB lifecycle, and async queue
  for SdccMmioHcDxe — the generic Qualcomm SDCC5 MMIO SDHCI HC driver.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

#define SD_CMD11_VOLTAGE_SWITCH  11u

// ─── TRB lifecycle ─────────────────────────────────────────────────────────

SDCC_HC_TRB *
SdccCreateTrb (
  IN SDCC_HC_PRIVATE_DATA                  *Private,
  IN UINT8                                  Slot,
  IN EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet,
  IN EFI_EVENT                              Event
  )
{
  SDCC_HC_TRB  *Trb;
  EFI_STATUS    Status;
  BOOLEAN       HasData;

  Trb = AllocateZeroPool (sizeof (SDCC_HC_TRB));
  if (Trb == NULL) {
    return NULL;
  }

  Trb->Signature = SDCC_TRB_SIGNATURE;
  Trb->Private   = Private;
  Trb->Slot      = Slot;
  Trb->Packet    = Packet;
  Trb->BlockSize = 0x200u;
  Trb->Event     = Event;
  Trb->Retries   = SDCC_TRB_RETRIES;
  Trb->Timeout   = Packet->Timeout;
  HasData        = FALSE;

  if (Packet->InDataBuffer != NULL) {
    Trb->Data    = Packet->InDataBuffer;
    Trb->DataLen = Packet->InTransferLength;
    Trb->Read    = TRUE;
    HasData      = TRUE;
  } else if (Packet->OutDataBuffer != NULL) {
    Trb->Data    = Packet->OutDataBuffer;
    Trb->DataLen = Packet->OutTransferLength;
    Trb->Read    = FALSE;
    HasData      = TRUE;
  }

  if (!HasData) {
    return Trb;
  }

  Status = SdccDmaMap (Trb);
  if (EFI_ERROR (Status)) {
    FreePool (Trb);
    return NULL;
  }

  Status = SdccBuildAdmaDescTable (Private, Trb);
  if (EFI_ERROR (Status)) {
    SdccDmaUnmap (Trb);
    FreePool (Trb);
    return NULL;
  }

  return Trb;
}

VOID
SdccFreeTrb (
  IN SDCC_HC_TRB  *Trb
  )
{
  if (Trb == NULL) {
    return;
  }
  SdccDmaUnmap (Trb);
  SdccFreeAdmaDescTable (Trb);
  FreePool (Trb);
}

// ─── Async TRB queue ───────────────────────────────────────────────────────

/**
  1 ms periodic timer callback — drains the head of the async TRB queue.
**/
VOID
EFIAPI
SdccProcessAsyncTaskList (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  LIST_ENTRY            *Link;
  SDCC_HC_TRB           *Trb;
  EFI_STATUS             Status;
  EFI_EVENT              TrbEvent;

  Private = (SDCC_HC_PRIVATE_DATA *)Context;
  Status  = EFI_SUCCESS;
  Trb     = NULL;

  Link = GetFirstNode (&Private->Queue);
  if (!IsNull (&Private->Queue, Link)) {
    Trb = SDCC_TRB_FROM_LIST (Link);

    if (!Private->MediaPresent) {
      Status = EFI_NO_MEDIA;
      goto Done;
    }

    if (!Trb->Started) {
      Status = SdccCheckTrbEnv (Private, Trb);
      if (!EFI_ERROR (Status)) {
        Trb->Started = TRUE;
        Status       = SdccExecTrb (Private, Trb);
        if (EFI_ERROR (Status)) {
          goto Done;
        }
      } else {
        goto Done;
      }
    }

    Status = SdccCheckTrbResult (Private, Trb);
  }

Done:
  if ((Trb != NULL) && (Status == EFI_NOT_READY)) {
    if ((Trb->Packet->Timeout != 0) && (Trb->Timeout == 0)) {
      RemoveEntryList (Link);
      Trb->Packet->TransactionStatus = EFI_TIMEOUT;
      TrbEvent = Trb->Event;
      SdccFreeTrb (Trb);
      gBS->SignalEvent (TrbEvent);
    } else if (Trb->Packet->Timeout != 0) {
      Trb->Timeout--;
    }
  } else if ((Trb != NULL) && (Status == EFI_CRC_ERROR) && (Trb->Retries > 0)) {
    Trb->Retries--;
    Trb->Started = FALSE;
    Trb->Timeout = Trb->Packet->Timeout;  // fresh deadline for each retry attempt
  } else if (Trb != NULL) {
    RemoveEntryList (Link);
    Trb->Packet->TransactionStatus = Status;
    TrbEvent = Trb->Event;
    SdccFreeTrb (Trb);
    gBS->SignalEvent (TrbEvent);
  }
}

/**
  TPL_APPLICATION callback: calls ConnectController to bind drivers to the SD
  handle after a card insertion is detected.  Signalled by SdccEnumerateDevice.
**/
VOID
EFIAPI
SdccDeferredConnect (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SDCC_HC_PRIVATE_DATA  *Private = (SDCC_HC_PRIVATE_DATA *)Context;

  gBS->ConnectController (Private->Handle, NULL, NULL, TRUE);
}

/**
  100 ms periodic callback for SD card hot-plug detection (Sdc2 only).
**/
VOID
EFIAPI
SdccEnumerateDevice (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  BOOLEAN                Present;
  UINTN                  Base;
  EFI_TPL                OldTpl;
  LIST_ENTRY            *Link;
  SDCC_HC_TRB           *PendTrb;
  EFI_EVENT              TrbEvent;

  Private = (SDCC_HC_PRIVATE_DATA *)Context;
  Base    = (UINTN)Private->Config.BaseAddress;

  if (EFI_ERROR (SdccHcCardDetect (Base, &Present))) {
    return;
  }
  if (Present == Private->MediaPresent) {
    return;
  }
  Private->MediaPresent = Present;

  if (!Present) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    while (!IsListEmpty (&Private->Queue)) {
      Link    = GetFirstNode (&Private->Queue);
      PendTrb = SDCC_TRB_FROM_LIST (Link);
      RemoveEntryList (Link);
      PendTrb->Packet->TransactionStatus = EFI_NO_MEDIA;
      TrbEvent = PendTrb->Event;
      SdccFreeTrb (PendTrb);
      gBS->SignalEvent (TrbEvent);
    }
    gBS->RestoreTPL (OldTpl);
  }

  // Signal the TPL_APPLICATION event to call ConnectController safely.
  // ConnectController must not be called above TPL_APPLICATION (UEFI spec §6.3).
  gBS->SignalEvent (Private->DeferredConnectEvent);
}

// ─── EFI_SD_MMC_PASS_THRU_PROTOCOL ─────────────────────────────────────────

EFI_STATUS
EFIAPI
SdccPassThruPassThru (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL        *This,
  IN     UINT8                                 Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet,
  IN     EFI_EVENT                             Event OPTIONAL
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  SDCC_HC_TRB           *Trb;
  EFI_STATUS             Status;
  EFI_TPL                OldTpl;
  UINTN                  Base;
  UINT64                 Timeout;
  BOOLEAN                InfiniteWait;

  if ((This == NULL) || (Packet == NULL) || (Slot != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Packet->SdMmcCmdBlk == NULL) || (Packet->SdMmcStatusBlk == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SDCC_HC_PRIVATE_FROM_PASSTHRU (This);
  Base    = (UINTN)Private->Config.BaseAddress;

  //
  // Snapshot MediaPresent under TPL_CALLBACK to close the TOCTOU window with
  // SdccEnumerateDevice, which runs at TPL_CALLBACK and can flip MediaPresent
  // between a bare check and subsequent TRB operations.
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  if (!Private->MediaPresent) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NO_MEDIA;
  }
  gBS->RestoreTPL (OldTpl);

  Trb = SdccCreateTrb (Private, Slot, Packet, Event);
  if (Trb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Event != NULL) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    InsertTailList (&Private->Queue, &Trb->TrbList);
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }

  Timeout      = Packet->Timeout;
  InfiniteWait = (Timeout == 0);

  while (InfiniteWait || (Timeout > 0)) {
    Status = SdccCheckTrbEnv (Private, Trb);
    if (!EFI_ERROR (Status)) {
      break;
    }
    gBS->Stall (1);
    if (!InfiniteWait) {
      Timeout--;
    }
  }

  if (!InfiniteWait && (Timeout == 0)) {
    Packet->TransactionStatus = EFI_TIMEOUT;
    SdccFreeTrb (Trb);
    return EFI_TIMEOUT;
  }

  Status = SdccExecTrb (Private, Trb);
  if (EFI_ERROR (Status)) {
    Packet->TransactionStatus = Status;
    SdccFreeTrb (Trb);
    return Status;
  }

  Timeout      = Packet->Timeout;
  InfiniteWait = (Timeout == 0);

  while (InfiniteWait || (Timeout > 0)) {
    Status = SdccCheckTrbResult (Private, Trb);
    if (Status != EFI_NOT_READY) {
      break;
    }
    gBS->Stall (1);
    if (!InfiniteWait) {
      Timeout--;
    }
  }

  if (!InfiniteWait && (Timeout == 0)) {
    Status = EFI_TIMEOUT;
  }

  Packet->TransactionStatus = Status;

  if (!EFI_ERROR (Status) &&
      !Private->Config.IsEmmc &&
      (Packet->SdMmcCmdBlk->CommandIndex == SD_CMD11_VOLTAGE_SWITCH))
  {
    Status = SdccHcUhs1SignalSwitch (Base);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
    }
  }

  //
  // Log commands that matter for diagnosis: CMD6 (partition switch, mode switch),
  // CMD17/CMD18/CMD24/CMD25 (block reads/writes).  Errors always shown; success
  // shown only for CMD6 which is critical for partition visibility.
  //
  {
    UINT8  Idx = Packet->SdMmcCmdBlk->CommandIndex;
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccPassThru] CMD%d Arg=0x%08x Status=%r Resp0=0x%08x\n",
              Idx, Packet->SdMmcCmdBlk->CommandArgument, Status,
              Packet->SdMmcStatusBlk->Resp0));
    } else if (Idx == 6) {
      DEBUG ((DEBUG_INFO, "[SdccPassThru] CMD6 Arg=0x%08x Status=Success Resp0=0x%08x\n",
              Packet->SdMmcCmdBlk->CommandArgument,
              Packet->SdMmcStatusBlk->Resp0));
    }
  }

  SdccFreeTrb (Trb);
  return Status;
}

EFI_STATUS
EFIAPI
SdccPassThruGetNextSlot (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                          *Slot
  )
{
  if ((This == NULL) || (Slot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (*Slot == 0xFF) {
    *Slot = 0;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
SdccPassThruBuildDevicePath (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL   *This,
  IN     UINT8                            Slot,
  IN OUT EFI_DEVICE_PATH_PROTOCOL       **DevicePath
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  SD_DEVICE_PATH        *SdNode;
  EMMC_DEVICE_PATH      *EmmcNode;

  if ((This == NULL) || (DevicePath == NULL) || (Slot != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SDCC_HC_PRIVATE_FROM_PASSTHRU (This);

  if (Private->Config.IsEmmc) {
    EmmcNode = AllocatePool (sizeof (EMMC_DEVICE_PATH) + END_DEVICE_PATH_LENGTH);
    if (EmmcNode == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    EmmcNode->Header.Type    = MESSAGING_DEVICE_PATH;
    EmmcNode->Header.SubType = MSG_EMMC_DP;
    SetDevicePathNodeLength (&EmmcNode->Header, sizeof (EMMC_DEVICE_PATH));
    EmmcNode->SlotNumber = Slot;
    SetDevicePathEndNode ((VOID *)(EmmcNode + 1));
    *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)EmmcNode;
  } else {
    SdNode = AllocatePool (sizeof (SD_DEVICE_PATH) + END_DEVICE_PATH_LENGTH);
    if (SdNode == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    SdNode->Header.Type    = MESSAGING_DEVICE_PATH;
    SdNode->Header.SubType = MSG_SD_DP;
    SetDevicePathNodeLength (&SdNode->Header, sizeof (SD_DEVICE_PATH));
    SdNode->SlotNumber = Slot;
    SetDevicePathEndNode ((VOID *)(SdNode + 1));
    *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)SdNode;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SdccPassThruGetSlotNumber (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  OUT UINT8                          *Slot
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  SD_DEVICE_PATH        *SdNode;
  EMMC_DEVICE_PATH      *EmmcNode;

  if ((This == NULL) || (DevicePath == NULL) || (Slot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (DevicePath->Type != MESSAGING_DEVICE_PATH) {
    return EFI_UNSUPPORTED;
  }

  Private = SDCC_HC_PRIVATE_FROM_PASSTHRU (This);

  if (Private->Config.IsEmmc && (DevicePath->SubType == MSG_EMMC_DP)) {
    EmmcNode = (EMMC_DEVICE_PATH *)DevicePath;
    *Slot    = EmmcNode->SlotNumber;
    return EFI_SUCCESS;
  }
  if (!Private->Config.IsEmmc && (DevicePath->SubType == MSG_SD_DP)) {
    SdNode = (SD_DEVICE_PATH *)DevicePath;
    *Slot  = SdNode->SlotNumber;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

/**
  Reset the specified slot.
  For eMMC: returns EFI_UNSUPPORTED — the DLL-configured state must not be reset.
  For SD card: issues CMD/DAT soft reset.
**/
EFI_STATUS
EFIAPI
SdccPassThruResetDevice (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN UINT8                           Slot
  )
{
  SDCC_HC_PRIVATE_DATA  *Private;
  UINTN                  Base;
  UINT8                  SwRst;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private = SDCC_HC_PRIVATE_FROM_PASSTHRU (This);

  if (Private->Config.IsEmmc) {
    return EFI_UNSUPPORTED;
  }

  Base  = (UINTN)Private->Config.BaseAddress;
  SwRst = SDCC_HC_RST_CMD_LINE | SDCC_HC_RST_DAT_LINE;
  MmioWrite8 (Base + SDCC_HC_SW_RST, SwRst);
  return SdccWaitSet8 (Base, SDCC_HC_SW_RST, SwRst, 0, SDCC_HC_GENERIC_TIMEOUT_US);
}
