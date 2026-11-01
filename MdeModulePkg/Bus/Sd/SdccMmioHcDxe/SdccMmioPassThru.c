/** @file
  EFI_SD_MMC_PASS_THRU_PROTOCOL vtable, TRB lifecycle, and async queue
  for SdccMmioHcDxe — the generic Qualcomm SDCC5 MMIO SDHCI HC driver.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

#define SD_CMD11_VOLTAGE_SWITCH  11u

/**
  Issue ACMD6 to switch the SD card to 4-bit bus width, then set HOST_CTRL1[BIT1]
  to tell the host controller to sample all four DAT lines.

  SD Physical Layer Spec §4.3.10: the host sends CMD55 (APP_CMD, arg=RCA<<16)
  followed by ACMD6 (SET_BUS_WIDTH, arg=0x2 for 4-bit).  The card switches
  DAT[3:1] from Hi-Z to active drive.  The host register must be updated
  immediately after the card acknowledges ACMD6, before any data transfer.

  @param[in]  Private  Driver instance private data.
  @param[in]  Rca      Card's Relative Card Address (from CMD3 R6 bits[31:16]).

  @retval EFI_SUCCESS     Card and host both switched to 4-bit.
  @retval Others          CMD55 or ACMD6 failed; host register not changed.
**/
STATIC EFI_STATUS
SdccHcAcmd6BusWidth4 (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN UINT16                 Rca
  )
{
  EFI_STATUS            Status;
  EFI_SD_MMC_STATUS_BLOCK  Sts;
  UINTN                 Base;

  Base = (UINTN)Private->Config.BaseAddress;

  // CMD55 (APP_CMD): arg = RCA << 16, Ac, R1
  Status = SdccEmmcCmd (Private, SD_APP_CMD,
                        SdMmcCommandTypeAc, SdMmcResponseTypeR1,
                        (UINT32)Rca << 16, NULL, 0, &Sts, 100000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ACMD6: CMD55 failed %r\n", Status));
    return Status;
  }

  // ACMD6 (SET_BUS_WIDTH): arg = 0x2 (10b = 4-bit), Ac, R1
  Status = SdccEmmcCmd (Private, SD_SET_BUS_WIDTH,
                        SdMmcCommandTypeAc, SdMmcResponseTypeR1,
                        0x2u, NULL, 0, &Sts, 100000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ACMD6: ACMD6 failed %r\n", Status));
    return Status;
  }

  // Card now drives DAT[3:1]; update the host controller to match.
  MmioWrite8 (Base + SDCC_HC_HOST_CTRL1,
              MmioRead8 (Base + SDCC_HC_HOST_CTRL1) | SDCC_HC_CTRL1_BUS_WIDTH_4);

  DEBUG ((DEBUG_WARN, "[SdccHci] ACMD6: 4-bit bus width set "
         "Rca=0x%x HOST_CTRL1=0x%x\n",
         Rca, MmioRead8 (Base + SDCC_HC_HOST_CTRL1)));
  return EFI_SUCCESS;
}

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

/** Free a TRB and all associated DMA resources. **/
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
  TPL_CALLBACK callback: drops to TPL_APPLICATION to call ConnectController,
  which must not be invoked above TPL_APPLICATION (UEFI spec §6.3).
  Signalled by SdccEnumerateDevice on card insertion/removal state change.
**/
VOID
EFIAPI
SdccDeferredConnect (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SDCC_HC_PRIVATE_DATA  *Private = (SDCC_HC_PRIVATE_DATA *)Context;

  DEBUG ((DEBUG_WARN, "[SdccHcDxe] DeferredConnect fired handle=%p\n", Private->Handle));

  //
  // This callback fires at TPL_CALLBACK.  ConnectController must be called
  // at TPL_APPLICATION.  gBS->RestoreTPL() takes no return value; we must
  // re-raise back to TPL_CALLBACK explicitly after the call.
  //
  gBS->RestoreTPL (TPL_APPLICATION);
  gBS->ConnectController (Private->Handle, NULL, NULL, TRUE);
  gBS->RaiseTPL (TPL_CALLBACK);
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

  if (EFI_ERROR (SdccHcCardDetect (Base, Private->Config.NoCdPin, &Present))) {
    return;
  }

  //
  // NoCdPin platforms use DAT[3:0] line levels as a presence proxy.  During an
  // active SD I/O transaction the card drives those lines LOW for data-0 bits,
  // making them momentarily read as absent.  Debounce: require SDCC_ABSENCE_DEBOUNCE_COUNT
  // consecutive polls of Present=FALSE before treating it as a real removal.
  // A real removal is sustained; an I/O-driven LOW lasts only microseconds.
  //
  if (Private->Config.NoCdPin && !Present && Private->MediaPresent) {
    Private->AbsenceCount++;
    if (Private->AbsenceCount < SDCC_ABSENCE_DEBOUNCE_COUNT) {
      DEBUG ((DEBUG_INFO,
              "[SdccHcDxe] EnumerateDevice: NoCdPin transient absent (%u/%u) handle=%p\n",
              (UINT32)Private->AbsenceCount, (UINT32)SDCC_ABSENCE_DEBOUNCE_COUNT,
              Private->Handle));
      return;
    }
    // AbsenceCount reached threshold — treat as real removal below.
  } else if (Present) {
    Private->AbsenceCount = 0;
  }

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] EnumerateDevice: Present=%d MediaPresent=%d handle=%p\n",
          (UINT32)Present, (UINT32)Private->MediaPresent, Private->Handle));
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

  // Signal the TPL_CALLBACK event; SdccDeferredConnect will drop to
  // TPL_APPLICATION before calling ConnectController (UEFI spec §6.3).
  gBS->SignalEvent (Private->DeferredConnectEvent);
}

// ─── EFI_SD_MMC_PASS_THRU_PROTOCOL ─────────────────────────────────────────

/**
  Submit an SD/MMC command packet to the specified slot.

  Builds a TRB from the packet, executes it synchronously (Event==NULL) or
  queues it for async completion (Event!=NULL).  On return from a synchronous
  call the SdMmcStatusBlk fields are populated with the device response.

  @param[in]      This    PassThru protocol instance.
  @param[in]      Slot    Zero-based slot number.
  @param[in,out]  Packet  Command/response/data descriptor.
  @param[in]      Event   Optional async completion event; NULL = synchronous.

  @retval EFI_SUCCESS            Command completed without error.
  @retval EFI_INVALID_PARAMETER  Packet is NULL or Slot is invalid.
  @retval EFI_DEVICE_ERROR       Command failed or timed out.
  @retval EFI_OUT_OF_RESOURCES   TRB allocation failed.
  @retval EFI_NOT_READY          Controller busy; retry.
**/
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
  // Raise to TPL_CALLBACK -- the same level SdccEnumerateDevice's hot-plug
  // timer runs at -- and hold it through MediaPresent snapshot, dispatch,
  // AND completion polling.  A bare snapshot alone does not close the race:
  // SdccEnumerateDevice can still flip MediaPresent (and drain the async
  // queue) while this synchronous command is executing/polling below, since
  // nothing else was blocking it from preempting during that window.
  // Held only through TRB creation for the async path (Event != NULL);
  // actual execution there happens later under SdccProcessAsyncTaskList.
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  if (!Private->MediaPresent) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NO_MEDIA;
  }

  Trb = SdccCreateTrb (Private, Slot, Packet, Event);
  if (Trb == NULL) {
    gBS->RestoreTPL (OldTpl);
    return EFI_OUT_OF_RESOURCES;
  }

  if (Event != NULL) {
    gBS->RestoreTPL (OldTpl);
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    InsertTailList (&Private->Queue, &Trb->TrbList);
    gBS->RestoreTPL (OldTpl);
    return EFI_SUCCESS;
  }

  Timeout      = Packet->Timeout;
  InfiniteWait = (Timeout == 0);

  //
  // Pre-command CMD_INHIBIT poll.  Log state for CMD3 (SD card only) to
  // diagnose card-not-responding failures.  For CMD18 (SD read), issue CMD13
  // (SEND_STATUS) to log the card's CURRENT_STATE before the transfer: if the
  // card is not in Tran state (bits[12:9] != 4) the data transfer will fail.
  //
  if (!Private->Config.IsEmmc && (Packet->SdMmcCmdBlk->CommandIndex == 18)) {
    EFI_SD_MMC_STATUS_BLOCK  DiagSts;

    ZeroMem (&DiagSts, sizeof DiagSts);
    if (!EFI_ERROR (SdccEmmcCmd (
                      Private, 13,
                      SdMmcCommandTypeAc, SdMmcResponseTypeR1,
                      (UINT32)Private->SdRca << 16,
                      NULL, 0, &DiagSts, 500000u)))
    {
      DEBUG ((DEBUG_WARN,
              "[SdccHci] CMD18 pre-check: CMD13 Resp0=0x%08x CURRENT_STATE=%d\n",
              DiagSts.Resp0, (DiagSts.Resp0 >> 9) & 0xFu));
    } else {
      DEBUG ((DEBUG_WARN, "[SdccHci] CMD18 pre-check: CMD13 failed\n"));
    }
  }

  if (!Private->Config.IsEmmc && (Packet->SdMmcCmdBlk->CommandIndex == 3)) {
    DEBUG ((DEBUG_WARN,
            "[SdccHci] CMD3 pre-fire: PRESENT_STATE=0x%x NOR_INT_STS=0x%x ERR_INT_STS=0x%x\n",
            MmioRead32 (Base + SDCC_HC_PRESENT_STATE),
            MmioRead16 (Base + SDCC_HC_NOR_INT_STS),
            MmioRead16 (Base + SDCC_HC_ERR_INT_STS)));
  }

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
    if (!Private->Config.IsEmmc && (Packet->SdMmcCmdBlk->CommandIndex == 3)) {
      DEBUG ((DEBUG_WARN,
              "[SdccHci] CMD3 TrbEnv TIMEOUT: PRESENT_STATE=0x%x NOR=0x%x ERR=0x%x\n",
              MmioRead32 (Base + SDCC_HC_PRESENT_STATE),
              MmioRead16 (Base + SDCC_HC_NOR_INT_STS),
              MmioRead16 (Base + SDCC_HC_ERR_INT_STS)));
    }
    Packet->TransactionStatus = EFI_TIMEOUT;
    SdccFreeTrb (Trb);
    gBS->RestoreTPL (OldTpl);
    return EFI_TIMEOUT;
  }

  Status = SdccExecTrb (Private, Trb);
  if (EFI_ERROR (Status)) {
    Packet->TransactionStatus = Status;
    SdccFreeTrb (Trb);
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  if (!Private->Config.IsEmmc && (Packet->SdMmcCmdBlk->CommandIndex == 3)) {
    DEBUG ((DEBUG_WARN,
            "[SdccHci] CMD3 fired: PRESENT_STATE=0x%x NOR_INT_STS=0x%x\n",
            MmioRead32 (Base + SDCC_HC_PRESENT_STATE),
            MmioRead16 (Base + SDCC_HC_NOR_INT_STS)));
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

  //
  // Log CMD18 result only on timeout to avoid per-sector noise on success.
  // Include ADMA diagnostic registers so a DMA stall can be distinguished
  // from a descriptor address issue:
  //   ADMA_ERR_STS (0x54): bits[1:0]=ADMA state, bit[2]=length mismatch
  //   VS_ADMA_ERR_ADDR0 (0x214): vendor last-fetched ADMA address
  //   AdmaDescPhy: physical address of the ADMA descriptor table
  //   Desc[0]: first descriptor entry (flags, size, lower addr, upper addr)
  //
  if (!Private->Config.IsEmmc &&
      (Packet->SdMmcCmdBlk->CommandIndex == 18) &&
      EFI_ERROR (Status))
  {
    SDCC_ADMA2_64_DESC  *Desc0;

    Desc0 = (Trb->AdmaDesc != NULL) ? (SDCC_ADMA2_64_DESC *)Trb->AdmaDesc : NULL;
    DEBUG ((DEBUG_WARN,
            "[SdccHci] CMD18 TIMEOUT: NOR=0x%x ERR=0x%x PRESENT=0x%x\n",
            MmioRead16 (Base + SDCC_HC_NOR_INT_STS),
            MmioRead16 (Base + SDCC_HC_ERR_INT_STS),
            MmioRead32 (Base + SDCC_HC_PRESENT_STATE)));
    DEBUG ((DEBUG_WARN,
            "[SdccHci] CMD18 DMA: ADMA_ERR_STS=0x%x VS_ADMA_ERR_ADDR=0x%x AdmaDescPhy=0x%lx\n",
            MmioRead32 (Base + SDCC_HC_ADMA_ERR_STS),
            SdccPlatformReadAdmaErrAddr (&Private->Config),
            (UINT64)Trb->AdmaDescPhy));
    if (Desc0 != NULL) {
      DEBUG ((DEBUG_WARN,
              "[SdccHci] CMD18 Desc[0]: Flags=0x%04x Len=0x%04x AddrLo=0x%08x AddrHi=0x%08x\n",
              (UINT32)Desc0->Valid | ((UINT32)Desc0->End << 1) |
                ((UINT32)Desc0->Int << 2) | ((UINT32)Desc0->Act << 4),
              (UINT32)Desc0->Length,
              (UINT32)Desc0->LowerAddress,
              (UINT32)Desc0->UpperAddress));
    }

    //
    // CMD18 timed out: ADMA_ERR_STS=ST_TFR means the ADMA engine fetched the
    // descriptor and started waiting for card data that never arrived.  The
    // card accepted CMD18 (CMD_COMPLETE fired, NOR bit0=1) but is not in Tran
    // state and will not drive DAT lines.
    //
    // Recovery per SDHCI spec §3.7.2:
    //   1. SW_RST_DAT_LINE — resets the host's DAT state machine and ADMA
    //      engine, clearing DAT_INHIBIT so the next command can be submitted.
    //      SW_RST_CMD_LINE is EL2-blocked on Sdc2 but SW_RST_DAT_LINE is not.
    //   2. CMD12 (STOP_TRANSMISSION) — returns the SD card from Sending-Data
    //      or Receive-Data state to Transfer (Tran) state so the subsequent
    //      CMD18 retry sees a card that is ready to send data.
    //
    MmioWrite8 (Base + SDCC_HC_SW_RST, SDCC_HC_RST_DAT_LINE);
    SdccWaitSet8 (Base, SDCC_HC_SW_RST, SDCC_HC_RST_DAT_LINE, 0,
                  SDCC_HC_GENERIC_TIMEOUT_US);
    DEBUG ((DEBUG_WARN, "[SdccHci] CMD18 recover: SW_RST_DAT_LINE done\n"));

    {
      EFI_SD_MMC_STATUS_BLOCK  Cmd12Sts;

      ZeroMem (&Cmd12Sts, sizeof Cmd12Sts);
      if (!EFI_ERROR (SdccEmmcCmd (
                        Private, 12,
                        SdMmcCommandTypeAc, SdMmcResponseTypeR1b,
                        0u, NULL, 0, &Cmd12Sts, 500000u)))
      {
        DEBUG ((DEBUG_WARN,
                "[SdccHci] CMD18 recover: CMD12 OK Resp0=0x%08x CURRENT_STATE=%d\n",
                Cmd12Sts.Resp0, (Cmd12Sts.Resp0 >> 9) & 0xFu));
      } else {
        DEBUG ((DEBUG_WARN, "[SdccHci] CMD18 recover: CMD12 failed (card may already be in Tran)\n"));
      }
    }
  }

  if (!Private->Config.IsEmmc && (Packet->SdMmcCmdBlk->CommandIndex == 3)) {
    DEBUG ((DEBUG_WARN,
            "[SdccHci] CMD3 result: Status=%r NOR=0x%x ERR=0x%x PRESENT=0x%x\n",
            Status,
            MmioRead16 (Base + SDCC_HC_NOR_INT_STS),
            MmioRead16 (Base + SDCC_HC_ERR_INT_STS),
            MmioRead32 (Base + SDCC_HC_PRESENT_STATE)));
    if (!EFI_ERROR (Status)) {
      // R6 format: bits[31:16] = new RCA, bits[15:0] = card status
      Private->SdRca = (UINT16)(Packet->SdMmcStatusBlk->Resp0 >> 16);
      DEBUG ((DEBUG_WARN, "[SdccHci] CMD3: captured SdRca=0x%x\n", Private->SdRca));
    }
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
  // After CMD13 (SEND_STATUS) succeeds the card is in Transfer state and
  // the identification sequence is complete.  Switch to 4-bit bus width
  // exactly once by issuing ACMD6 to the card and setting HOST_CTRL1[BIT1].
  // Guard with SdBusWidthSwitched so the hook is a no-op for all subsequent
  // CMD13 calls (PartitionDxe, error handling, etc.).
  //
  if (!EFI_ERROR (Status) &&
      !Private->Config.IsEmmc &&
      !Private->SdBusWidthSwitched &&
      (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_STATUS) &&
      (Private->SdRca != 0))
  {
    EFI_STATUS  AcmdStatus;

    AcmdStatus = SdccHcAcmd6BusWidth4 (Private, Private->SdRca);
    if (!EFI_ERROR (AcmdStatus)) {
      Private->SdBusWidthSwitched = TRUE;
    } else {
      DEBUG ((DEBUG_WARN, "[SdccHci] 4-bit switch failed %r — staying at 1-bit\n",
              AcmdStatus));
    }
  }

  //
  // Log commands that matter for diagnosis: CMD6 (partition switch, mode switch),
  // CMD17/CMD18/CMD24/CMD25 (block reads/writes).  Errors always shown; success
  // shown only for CMD6 which is critical for partition visibility.
  //
  {
    UINT8  Idx;

    Idx = Packet->SdMmcCmdBlk->CommandIndex;
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
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Enumerate valid slot numbers for this controller.

  Call with *Slot = 0xFF to get the first slot.  On each subsequent call
  pass the previously returned *Slot to advance to the next one.
  Returns EFI_NOT_FOUND when no more slots exist.

  @param[in]      This   PassThru protocol instance.
  @param[in,out]  Slot   On entry the current slot (0xFF = start);
                         on exit the next valid slot number.

  @retval EFI_SUCCESS            Next slot number returned in *Slot.
  @retval EFI_INVALID_PARAMETER  This or Slot is NULL.
  @retval EFI_NOT_FOUND          No more slots.
**/
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

/**
  Build an EFI device path node for a given slot.

  Allocates and returns a device path ending with the appropriate eMMC or SD
  messaging node followed by an EndOfPath node.  Caller frees the buffer.

  @param[in]   This         PassThru protocol instance.
  @param[in]   Slot         Slot number; must be 0 for this single-slot HC.
  @param[out]  DevicePath   On success, points to the newly allocated path.

  @retval EFI_SUCCESS            Device path allocated and returned.
  @retval EFI_INVALID_PARAMETER  This or DevicePath is NULL, or Slot != 0.
  @retval EFI_OUT_OF_RESOURCES   Pool allocation failed.
**/
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

/**
  Reverse-map a device path node back to a slot number.

  Accepts a messaging device path node (eMMC or SD) and returns the slot
  number encoded within it.

  @param[in]   This        PassThru protocol instance.
  @param[in]   DevicePath  Device path node produced by BuildDevicePath.
  @param[out]  Slot        Slot number decoded from DevicePath.

  @retval EFI_SUCCESS            Slot number returned.
  @retval EFI_INVALID_PARAMETER  This, DevicePath, or Slot is NULL.
  @retval EFI_UNSUPPORTED        DevicePath type or subtype does not match
                                 this controller's media type.
**/
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
