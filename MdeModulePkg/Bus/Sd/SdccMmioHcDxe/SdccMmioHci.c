/** @file
  SDHCI register access, command dispatch, and ADMA2 engine for
  SdccMmioHcDxe — the generic Qualcomm SDCC5 MMIO SDHCI host controller driver.

  All register access uses the standard IoLib (MmioRead/MmioWrite variants) so
  so the driver does not depend on PCI IO or any Qualcomm-specific library.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

//
// SDCC_ADMA_MAX_DESC_COUNT and SDCC_ADMA2_64_DESC are defined in SdccMmioHcDxe.h.
// The descriptor table is embedded in SDCC_HC_PRIVATE_DATA (Private->AdmaDescTable)
// so each controller instance owns its own table — see M4/M5 in readiness report.

// ─── MMIO primitives ──────────────────────────────────────────────────────────

STATIC VOID     SdccWrite8  (UINTN B, UINT32 O, UINT8  V) { MmioWrite8  (B+O, V); }
STATIC UINT8    SdccRead8   (UINTN B, UINT32 O)           { return MmioRead8  (B+O); }
STATIC VOID     SdccWrite16 (UINTN B, UINT32 O, UINT16 V) { MmioWrite16 (B+O, V); }
STATIC UINT16   SdccRead16  (UINTN B, UINT32 O)           { return MmioRead16 (B+O); }
STATIC VOID     SdccWrite32 (UINTN B, UINT32 O, UINT32 V) { MmioWrite32 (B+O, V); }
STATIC UINT32   SdccRead32  (UINTN B, UINT32 O)           { return MmioRead32 (B+O); }

STATIC VOID SdccOr16  (UINTN B, UINT32 O, UINT16 M) {
  SdccWrite16 (B, O, SdccRead16 (B, O) | M);
}
STATIC VOID SdccAnd16 (UINTN B, UINT32 O, UINT16 M) {
  SdccWrite16 (B, O, SdccRead16 (B, O) & M);
}

// ─── Wait helpers ─────────────────────────────────────────────────────────────

STATIC
EFI_STATUS
SdccWaitSet32 (
  IN UINTN   Base,
  IN UINT32  Off,
  IN UINT32  Mask,
  IN UINT32  Expected,
  IN UINT64  TimeoutUs
  )
{
  UINT64  Rem = TimeoutUs;

  while (TRUE) {
    if ((SdccRead32 (Base, Off) & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    if (Rem == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
    if (Rem != MAX_UINT64) {
      Rem--;
    }
  }
}

EFI_STATUS
SdccWaitSet8 (
  IN UINTN   Base,
  IN UINT32  Off,
  IN UINT8   Mask,
  IN UINT8   Expected,
  IN UINT64  TimeoutUs
  )
{
  UINT64  Rem = TimeoutUs;

  while (TRUE) {
    if ((SdccRead8 (Base, Off) & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    if (Rem == 0) {
      return EFI_TIMEOUT;
    }
    MicroSecondDelay (1);
    if (Rem != MAX_UINT64) {
      Rem--;
    }
  }
}

// ─── GCD / MMIO window mapping ────────────────────────────────────────────────

/**
  Add BaseAddress/Size to the GCD as Device Memory (UC) and set EFI_MEMORY_UC.

  SDCC MMIO regions are not pre-mapped by the UEFI image loader.  Without this
  call the first MmioRead triggers a translation fault.

  Individual GCD call failures are logged as warnings but not propagated; the
  function always returns EFI_SUCCESS so callers can proceed optimistically.

  @param[in]  BaseAddress  Physical base address of the MMIO window.
  @param[in]  Size         Size of the MMIO window in bytes.

  @retval EFI_SUCCESS          Attributes set successfully.
  @retval EFI_ACCESS_DENIED    SetMemorySpaceAttributes failed.
  @retval other                SetMemorySpaceAttributes error.
**/
EFI_STATUS
SdccHcMapMmio (
  IN EFI_PHYSICAL_ADDRESS  BaseAddress,
  IN UINTN                 Size
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;

  Status = gDS->GetMemorySpaceDescriptor (BaseAddress, &Desc);
  if (EFI_ERROR (Status) || (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent)) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo,
                                  BaseAddress, Size, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHci] AddMemorySpace 0x%lx: %r (may already exist)\n",
              BaseAddress, Status));
    }
  }

  Status = gDS->SetMemorySpaceAttributes (BaseAddress, Size, EFI_MEMORY_UC);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] SetMemorySpaceAttributes 0x%lx: %r\n",
            BaseAddress, Status));
    return Status;
  }
  return EFI_SUCCESS;
}

// ─── Controller version + capabilities ───────────────────────────────────────

STATIC
EFI_STATUS
SdccHcReadVersion (
  IN  UINTN    Base,
  OUT UINT16  *Version
  )
{
  if (Version == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Version = (UINT16)(SdccRead16 (Base, SDCC_HC_CTRL_VER) & 0xFFu);
  return EFI_SUCCESS;
}

EFI_STATUS
SdccHcReadCapability (
  IN  UINTN          Base,
  OUT SDCC_SLOT_CAP  *Cap
  )
{
  if (Cap == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Cap->Raw = (UINT64)SdccRead32 (Base, SDCC_HC_CAP) |
             ((UINT64)SdccRead32 (Base, SDCC_HC_CAP + 4u) << 32);
  return EFI_SUCCESS;
}

STATIC
VOID
SdccHcEnableInterrupts (
  IN UINTN  Base
  )
{
  SdccWrite16 (Base, SDCC_HC_NOR_INT_STS_EN, 0xFFFFu);
  SdccWrite16 (Base, SDCC_HC_ERR_INT_STS_EN, 0xFFFFu);
}

// ─── Reset ────────────────────────────────────────────────────────────────────

/**
  Software-reset all SDHCI internal state.

  Follows HPG §7.3.8.3:
    1. Mask PWRCTL IRQ
    2. Assert SW_RST_ALL
    3. ACK the BUS_OFF PWRCTL event
    4. Poll until SW_RST_ALL deasserts
    5. Unmask PWRCTL
    6. Re-enable interrupt status
**/
EFI_STATUS
SdccHcReset (
  IN UINTN  Base
  )
{
  EFI_STATUS  Status;
  UINT32      Retry;

  SdccWrite32 (Base, SDCC_HC_PWRCTL_MASK, 0x0u);
  SdccWrite8  (Base, SDCC_HC_SW_RST, SDCC_HC_RST_ALL);

  //
  // Drain the PWRCTL event generated by SW_RST_ALL (typically BUS_OFF, bits[1:0]).
  //
  // HPG §7.3.8 ACK sequence (matches Linux sdhci_msm_handle_pwr_irq):
  //   1. Read STATUS to capture pending events.
  //   2. Write CLEAR with those bits to acknowledge them.
  //   3. Poll STATUS until the bits clear (async, up to 10 × 10 µs).
  //   4. Write CTL with success bits derived from the original STATUS:
  //      bits[1:0] (BUS events) → CTL BIT0 (BUS_ON_OFF_SUCCESS)
  //      bits[3:2] (IO  events) → CTL BIT2 (IO_SUCCESS)
  //
  {
    UINT32  CapturedStatus;
    UINT32  CtlAck = 0;

    CapturedStatus = SdccRead32 (Base, SDCC_HC_PWRCTL_STATUS);
    if (CapturedStatus != 0) {
      SdccWrite32 (Base, SDCC_HC_PWRCTL_CLEAR, CapturedStatus);
      Retry = 10;
      while (Retry-- > 0) {
        if ((SdccRead32 (Base, SDCC_HC_PWRCTL_STATUS) & CapturedStatus) == 0) {
          break;
        }
        MicroSecondDelay (10);
      }
      if ((CapturedStatus & 0x3u) != 0) {
        CtlAck |= BIT0;  // BUS_ON_OFF_SUCCESS
      }
      if ((CapturedStatus & 0xCu) != 0) {
        CtlAck |= BIT2;  // IO_SUCCESS
      }
      SdccWrite32 (Base, SDCC_HC_PWRCTL_CTL, CtlAck);
    }
  }

  Status = SdccWaitSet8 (Base, SDCC_HC_SW_RST, SDCC_HC_RST_ALL, 0,
                          SDCC_HC_RESET_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] Reset timeout SW_RST=0x%x PWRCTL=0x%x\n",
            SdccRead8  (Base, SDCC_HC_SW_RST),
            SdccRead32 (Base, SDCC_HC_PWRCTL_STATUS)));
    return Status;
  }

  SdccWrite32 (Base, SDCC_HC_PWRCTL_MASK, 0xFu);
  SdccHcEnableInterrupts (Base);
  return EFI_SUCCESS;
}

// ─── V4 / 64-bit ADMA mode ────────────────────────────────────────────────────

/**
  Enable SDHCI v4 mode with 64-bit ADMA addressing and 26-bit data length.

  No-op for controller versions below v4.00.  For v4.00 enables 64-bit address
  mode if the capability advertises a 64-bit system bus.  For v4.10+ enables
  64-bit addressing and 26-bit ADMA data length unconditionally.

  @param[in]  Base               SDCC5 SDHCI register window base address.
  @param[in]  ControllerVersion  SDHCI controller version (from CTRL_VER[7:0]).
  @param[in]  Cap                Capability register snapshot.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
SdccHcEnableV4 (
  IN UINTN          Base,
  IN UINT16         ControllerVersion,
  IN SDCC_SLOT_CAP  Cap
  )
{
  UINT16  Ctrl2;

  if (ControllerVersion < SDCC_HC_CTRL_VER_400) {
    return EFI_SUCCESS;
  }

  Ctrl2 = SDCC_HC_CTRL2_V4_EN;

  if (ControllerVersion == SDCC_HC_CTRL_VER_400) {
    if (Cap.SysBus64V3) {
      Ctrl2 |= SDCC_HC_CTRL2_64_ADDR_EN;
    }
  } else {
    Ctrl2 |= SDCC_HC_CTRL2_64_ADDR_EN | SDCC_HC_CTRL2_26_DATA_LEN;
  }

  SdccOr16 (Base, SDCC_HC_HOST_CTRL2, Ctrl2);
  return EFI_SUCCESS;
}

// ─── Clock ────────────────────────────────────────────────────────────────────

STATIC
EFI_STATUS
SdccHcStopClock (
  IN UINTN  Base
  )
{
  EFI_STATUS  Status;

  Status = SdccWaitSet32 (Base, SDCC_HC_PRESENT_STATE,
                           SDCC_HC_STATE_CMD_INHIBIT | SDCC_HC_STATE_DAT_INHIBIT,
                           0, SDCC_HC_GENERIC_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  SdccAnd16 (Base, SDCC_HC_CLOCK_CTRL, (UINT16)~BIT2);
  return EFI_SUCCESS;
}

/**
  Program the SDHCI clock divisor for TargetFreqKHz.

  Computes the smallest power-of-2 divisor N such that
  BaseClkFreqKHz / N ≤ TargetFreqKHz, then writes CLOCK_CTRL.
  Both v1/v2 (8-bit) and v3+ (10-bit) divisor encodings are handled.

  @param[in]  Base               SDCC5 SDHCI register window base address.
  @param[in]  ControllerVersion  SDHCI controller version (from CTRL_VER[7:0]).
  @param[in]  BaseClkFreqKHz     Base clock frequency in kHz (actual apps_clk, not CAPS value).
  @param[in]  TargetFreqKHz      Desired SDCLK frequency in kHz.

  @retval EFI_SUCCESS            Clock programmed and stable.
  @retval EFI_INVALID_PARAMETER  BaseClkFreqKHz is zero.
  @retval EFI_TIMEOUT            Internal clock did not stabilise within 150 ms.
  @retval Others                 Propagated from SdccHcStopClock().
**/
EFI_STATUS
SdccHcClockSupply (
  IN UINTN   Base,
  IN UINT16  ControllerVersion,
  IN UINT32  BaseClkFreqKHz,
  IN UINT32  TargetFreqKHz
  )
{
  EFI_STATUS  Status;
  UINT32      Divisor;
  UINT16      ClkCtrl;

  Status = SdccHcStopClock (Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (BaseClkFreqKHz == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (TargetFreqKHz >= BaseClkFreqKHz) {
    Divisor = 0;  // bypass: SDCLK = BaseClk
  } else {
    //
    // Find the smallest power-of-2 N such that BaseClk / (2*N) <= Target.
    // The loop exits with N in Divisor; this value is written directly to
    // CLOCK_CTRL[15:8] per SDHCI spec §2.2.14: SDCLK = BaseClk / (2*N).
    // Do NOT double Divisor after the loop — N is already the register value.
    //
    Divisor = 1;
    while ((BaseClkFreqKHz / (Divisor * 2)) > TargetFreqKHz) {
      Divisor <<= 1;
      if (Divisor > 0x3FFu) {
        Divisor = 0x3FFu;
        break;
      }
    }
    // Divisor holds N; SDCLK = BaseClkFreqKHz / (2*N) <= TargetFreqKHz.
  }

  if (ControllerVersion >= SDCC_HC_CTRL_VER_300) {
    ClkCtrl = (UINT16)(((Divisor & 0xFFu) << 8) | (((Divisor >> 8) & 0x3u) << 6));
  } else {
    ClkCtrl = (UINT16)((Divisor & 0xFFu) << 8);
  }

  ClkCtrl |= BIT0;
  SdccWrite16 (Base, SDCC_HC_CLOCK_CTRL, ClkCtrl);

  Status = SdccWaitSet8 (Base, SDCC_HC_CLOCK_CTRL, BIT1, BIT1,
                          SDCC_HC_CLK_STABLE_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] Clock stable timeout (target=%dkHz)\n", TargetFreqKHz));
    return Status;
  }

  SdccOr16 (Base, SDCC_HC_CLOCK_CTRL, BIT2);
  return EFI_SUCCESS;
}

// ─── Power ────────────────────────────────────────────────────────────────────

/**
  Power on the SD bus at the highest voltage advertised in the capability register.

  Selects 3.3 V, 3.0 V, or 1.8 V in that priority order.  Returns
  EFI_DEVICE_ERROR if none of the voltage bits are set (malformed capability).

  @param[in]  Base  SDCC5 SDHCI register window base address.
  @param[in]  Cap   Capability register snapshot (from SdccHcReadCapability).

  @retval EFI_SUCCESS       Bus powered on at the selected voltage.
  @retval EFI_DEVICE_ERROR  No supported voltage found in capability register.
**/
EFI_STATUS
SdccHcInitPower (
  IN UINTN          Base,
  IN SDCC_SLOT_CAP  Cap
  )
{
  UINT8  PwrCtrl;

  SdccWrite8 (Base, SDCC_HC_POWER_CTRL, 0);

  if (Cap.Voltage33) {
    PwrCtrl = SDCC_HC_POWER_3P3V;
  } else if (Cap.Voltage30) {
    PwrCtrl = SDCC_HC_POWER_3P0V;
  } else if (Cap.Voltage18) {
    PwrCtrl = SDCC_HC_POWER_1P8V;
  } else {
    return EFI_DEVICE_ERROR;
  }

  SdccWrite8 (Base, SDCC_HC_POWER_CTRL, PwrCtrl | SDCC_HC_POWER_ON);
  return EFI_SUCCESS;
}

// ─── Timeout ──────────────────────────────────────────────────────────────────

/**
  Program TIMEOUT_CTRL to the maximum counter value (0x0E = ~1 second).

  Must be called after every software reset.  Without this, CMD13 and other
  commands that rely on the data-timeout counter will time out immediately.

  @param[in]  Base  SDCC5 SDHCI register window base address.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
SdccHcInitTimeout (
  IN UINTN  Base
  )
{
  SdccWrite8 (Base, SDCC_HC_TIMEOUT_CTRL, 0x0Eu);
  return EFI_SUCCESS;
}

// ─── Card detect ──────────────────────────────────────────────────────────────

/**
  Read PRESENT_STATE to determine whether a card is inserted and stable.

  @param[in]  Base     SDCC5 SDHCI register window base address.
  @param[out] Present  Set to TRUE if a card is present and electrically stable;
                       FALSE otherwise.

  @retval EFI_SUCCESS            Present was populated successfully.
  @retval EFI_INVALID_PARAMETER  Present is NULL.
**/
EFI_STATUS
SdccHcCardDetect (
  IN  UINTN    Base,
  OUT BOOLEAN *Present
  )
{
  UINT32  State;

  if (Present == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  State    = SdccRead32 (Base, SDCC_HC_PRESENT_STATE);
  *Present = ((State & SDCC_HC_STATE_CARD_STABLE) &&
              (State & SDCC_HC_STATE_CARD_PRESENT)) ? TRUE : FALSE;
  return EFI_SUCCESS;
}

// ─── Cold init (SD card, Sdc2) ───────────────────────────────────────────────

/**
  Full cold-initialization for an SD card controller instance.

  Only proceeds if the SDHCI peripheral is already mapped in GCD.  This guards
  against the case where the platform clock driver has not yet enabled the
  controller's AHB clock — any register access before that causes a bus abort.

  Steps: software reset → read version + capability → power on → 400 kHz clock
  → timeout register → V4/64-bit ADMA mode → card detect.

  @param[in]  Private  Driver instance private data.  Config.BaseAddress must
                       contain the controller's MMIO base address.

  @retval EFI_SUCCESS         Initialization succeeded; Private->MediaPresent updated.
  @retval EFI_NOT_READY       MMIO window not yet mapped in GCD (clocks not enabled).
  @retval EFI_DEVICE_ERROR    CapsBaseClkFreqKHz is 0 (PCD not configured).
  @retval Others              Propagated from SdccHcReset(), SdccHcReadVersion(),
                              SdccHcReadCapability(), SdccHcInitPower(),
                              SdccHcClockSupply(), SdccHcEnableV4(), or
                              SdccHcCardDetect().
**/
EFI_STATUS
SdccHcColdInit (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                       Status;
  UINTN                            Base;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDesc;

  Base = (UINTN)Private->Config.BaseAddress;

  Status = gDS->GetMemorySpaceDescriptor ((EFI_PHYSICAL_ADDRESS)Base, &GcdDesc);
  if (EFI_ERROR (Status) ||
      (GcdDesc.GcdMemoryType == EfiGcdMemoryTypeNonExistent))
  {
    DEBUG ((DEBUG_INFO, "[SdccHci] 0x%lx not in GCD — ColdInit skipped\n",
            (UINT64)Base));
    return EFI_NOT_READY;
  }

  Status = SdccHcReset (Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdccHcReadVersion (Base, &Private->ControllerVersion);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdccHcReadCapability (Base, &Private->Capability);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Use the platform-configured base clock.  The CAPS value is not always
  // reliable on SDCC5 (SDCC5 reports a software-overridden value).
  //
  if (Private->Config.CapsBaseClkFreqKHz == 0) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] CapsBaseClkFreqKHz=0 — check PCD\n"));
    return EFI_DEVICE_ERROR;
  }

  Status = SdccHcInitPower (Base, Private->Capability);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdccHcClockSupply (Base, Private->ControllerVersion,
                               Private->Config.CapsBaseClkFreqKHz,
                               SDCC_HC_IDENTIFY_CLOCK_KHZ);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SdccHcInitTimeout (Base);

  Status = SdccHcEnableV4 (Base, Private->ControllerVersion, Private->Capability);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return SdccHcCardDetect (Base, &Private->MediaPresent);
}

// ─── UHS-I 1.8V signal voltage switch ────────────────────────────────────────

/**
  Perform the SDHCI host-controller side of the UHS-I 1.8V signal switch.

  Called after a successful CMD11 response, following SDHCI Simplified Spec
  v3.0 §3.6.1:
    1. Stop SD clock
    2. Verify DAT[3:0] pulled low by card
    3. Set 1.8V Signaling Enable
    4. Wait 5 ms for regulator
    5. Re-enable clock
    6. Verify DAT[3:0] return high
**/
EFI_STATUS
SdccHcUhs1SignalSwitch (
  IN UINTN  Base
  )
{
  EFI_STATUS  Status;

  SdccAnd16 (Base, SDCC_HC_CLOCK_CTRL, (UINT16)~BIT2);

  Status = SdccWaitSet32 (Base, SDCC_HC_PRESENT_STATE,
                           SDCC_HC_STATE_DAT_0_3_MASK, 0,
                           SDCC_HC_UHS1_SWITCH_DELAY_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] UHS1: DAT lines not low after CMD11\n"));
    return EFI_DEVICE_ERROR;
  }

  SdccOr16 (Base, SDCC_HC_HOST_CTRL2, SDCC_HC_CTRL2_1P8V_EN);
  MicroSecondDelay (SDCC_HC_UHS1_SWITCH_DELAY_US);
  SdccOr16 (Base, SDCC_HC_CLOCK_CTRL, BIT2);
  MicroSecondDelay (1000u);

  Status = SdccWaitSet32 (Base, SDCC_HC_PRESENT_STATE,
                           SDCC_HC_STATE_DAT_0_3_MASK,
                           SDCC_HC_STATE_DAT_0_3_MASK,
                           SDCC_HC_UHS1_SWITCH_DELAY_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] UHS1: DAT lines did not return high — reverting\n"));
    SdccAnd16 (Base, SDCC_HC_HOST_CTRL2, (UINT16)~SDCC_HC_CTRL2_1P8V_EN);
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

// ─── DMA — identity mapping with cache maintenance ───────────────────────────

/**
  Map a data buffer for ADMA2 using identity (PA==VA) mapping.

  Flush the cache before submission so the ADMA engine reads current data.
  Invalidate after completion (SdccDmaUnmap) so the CPU sees device-written data.
**/
EFI_STATUS
SdccDmaMap (
  IN SDCC_HC_TRB  *Trb
  )
{
  Trb->DataPhy          = (EFI_PHYSICAL_ADDRESS)(UINTN)Trb->Data;
  Trb->DataCacheFlushed = TRUE;
  WriteBackDataCacheRange (Trb->Data, Trb->DataLen);
  return EFI_SUCCESS;
}

VOID
SdccDmaUnmap (
  IN SDCC_HC_TRB  *Trb
  )
{
  if (!Trb->DataCacheFlushed) {
    return;
  }
  if (Trb->Read && (Trb->Data != NULL) && (Trb->DataLen > 0)) {
    InvalidateDataCacheRange (Trb->Data, Trb->DataLen);
  }
  Trb->DataCacheFlushed = FALSE;
}

// ─── ADMA2 descriptor table ───────────────────────────────────────────────────

/**
  Fill the static ADMA2 64-bit descriptor table.

  Qualcomm SDCC5 descriptor bit layout (differs from SDHCI standard spec):
    Valid = bit[0], End = bit[1], Int = bit[2], Act = bits[5:4] (0b10 = TRAN)
  This matches the Agatti reference sSDHCADMADescriptor struct.
**/
EFI_STATUS
SdccBuildAdmaDescTable (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN OUT SDCC_HC_TRB       *Trb
  )
{
  UINTN                 NumEntries;
  UINTN                 Remaining;
  EFI_PHYSICAL_ADDRESS  DataPhy;
  UINTN                 i;
  UINTN                 ChunkSize;

  // ADMA2 §1.14: a descriptor with Length=0 means 65536 bytes DMA, not zero.
  // Callers must never pass an empty buffer; catch it early.
  ASSERT (Trb->DataLen > 0);

  NumEntries = (Trb->DataLen + SDCC_ADMA2_MAX_DATA_16B - 1) / SDCC_ADMA2_MAX_DATA_16B;
  if (NumEntries == 0) {
    NumEntries = 1;
  }
  if (NumEntries > SDCC_ADMA_MAX_DESC_COUNT) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] Too many ADMA descriptors: %u\n",
            (UINT32)NumEntries));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Zero the ENTIRE table, not just NumEntries.  A prior transfer may have used
  // more descriptors, leaving a stale End=1 marker mid-table.  The ADMA engine
  // walks until it sees End=1, so a stale marker would silently truncate this
  // transfer.  Zeroing all 512 entries (~6 KB) is cheap and eliminates the risk.
  //
  ZeroMem (Private->AdmaDescTable, sizeof (Private->AdmaDescTable));

  Remaining = Trb->DataLen;
  DataPhy   = Trb->DataPhy;

  for (i = 0; i < NumEntries; i++) {
    ChunkSize                                = MIN (Remaining, SDCC_ADMA2_MAX_DATA_16B);
    Private->AdmaDescTable[i].Valid          = 1;
    Private->AdmaDescTable[i].Act            = SDCC_ADMA2_ACT_TRAN;
    Private->AdmaDescTable[i].Length         = (UINT16)(ChunkSize == SIZE_64KB
                                                           ? 0
                                                           : (UINT16)ChunkSize);
    Private->AdmaDescTable[i].LowerAddress   = (UINT32)(DataPhy & 0xFFFFFFFFu);
    Private->AdmaDescTable[i].UpperAddress   = (UINT32)(DataPhy >> 32);
    DataPhy   += ChunkSize;
    Remaining -= ChunkSize;
  }
  Private->AdmaDescTable[NumEntries - 1].End = 1;

  //
  // Flush the ENTIRE table (not just the active entries) so the ADMA engine
  // sees zeros beyond the terminal End=1 descriptor.  ZeroMem cleared the
  // entries in the CPU cache; without flushing them here the DRAM copy still
  // holds data from any prior larger transfer, and the ADMA engine (which reads
  // DRAM directly) would walk past the new End=1 into stale Valid=1 descriptors.
  //
  WriteBackDataCacheRange (Private->AdmaDescTable, sizeof (Private->AdmaDescTable));

  Trb->AdmaDesc    = Private->AdmaDescTable;
  Trb->AdmaDescPhy = (EFI_PHYSICAL_ADDRESS)(UINTN)Private->AdmaDescTable;
  Trb->AdmaDescPages = 0;

  return EFI_SUCCESS;
}

VOID
SdccFreeAdmaDescTable (
  IN SDCC_HC_TRB  *Trb
  )
{
  if (Trb->AdmaDesc == NULL) {
    return;
  }
  //
  // The descriptor table is embedded in SDCC_HC_PRIVATE_DATA (AdmaDescPages==0
  // always); it is freed when the private data struct is freed.  Just clear the
  // TRB pointer so callers do not access a stale reference.
  //
  ASSERT (Trb->AdmaDescPages == 0);
  Trb->AdmaDesc      = NULL;
  Trb->AdmaDescPhy   = 0;
  Trb->AdmaDescPages = 0;
}

// ─── CMD/DAT soft reset after error ──────────────────────────────────────────

STATIC
EFI_STATUS
SdccHcSoftwareReset (
  IN UINTN   Base,
  IN UINT16  ErrIntStatus
  )
{
  UINT8  SwRst = 0;

  if ((ErrIntStatus & 0x000Fu) != 0) {
    SwRst |= SDCC_HC_RST_CMD_LINE;
  }
  if ((ErrIntStatus & BIT8) != 0) {
    // AutoCMD12/23 error — SDHCI spec §3.10.2 requires CMD_LINE reset
    SwRst |= SDCC_HC_RST_CMD_LINE;
  }
  if ((ErrIntStatus & 0x0070u) != 0) {
    SwRst |= SDCC_HC_RST_DAT_LINE;
  }
  if ((ErrIntStatus & BIT9) != 0) {
    SwRst |= SDCC_HC_RST_DAT_LINE;
    DEBUG ((DEBUG_ERROR, "[SdccHci] ADMA_ERR_STS=0x%08x\n",
            MmioRead32 (Base + SDCC_HC_ADMA_ERR_STS)));
  }
  if (SwRst == 0) {
    return EFI_SUCCESS;
  }
  SdccWrite8 (Base, SDCC_HC_SW_RST, SwRst);
  return SdccWaitSet8 (Base, SDCC_HC_SW_RST, SwRst, 0, SDCC_HC_GENERIC_TIMEOUT_US);
}

// ─── Command environment check ────────────────────────────────────────────────

EFI_STATUS
SdccCheckTrbEnv (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN SDCC_HC_TRB           *Trb
  )
{
  UINT32  State;
  UINT32  Mask;

  State = SdccRead32 ((UINTN)Private->Config.BaseAddress, SDCC_HC_PRESENT_STATE);
  Mask  = SDCC_HC_STATE_CMD_INHIBIT;

  if ((Trb->Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1b) ||
      (Trb->Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR5b) ||
      (Trb->Packet->InDataBuffer  != NULL) ||
      (Trb->Packet->OutDataBuffer != NULL))
  {
    Mask |= SDCC_HC_STATE_DAT_INHIBIT;
  }

  return ((State & Mask) == 0) ? EFI_SUCCESS : EFI_NOT_READY;
}

// ─── Command submission ───────────────────────────────────────────────────────

/**
  Submit a TRB to the SDHCI command engine.

  Register write sequence follows the SDHCI Simplified Spec §3.7:
    ADMA_SYS_ADDR, BLK_SIZE, BLK_COUNT, ARG1, TRANS_MODE, COMMAND.
  HOST_CTRL1 DMA-select bits are set before writing COMMAND.

  Note on ADMA_SYS_ADDR write order: HIGH (0x5C) before LOW (0x58) — matches
  the Agatti HAL call order (HAL_sdhci_SetADMASystemAddress64bit then
  HAL_sdhci_SetADMASystemAddress).
**/
EFI_STATUS
SdccExecTrb (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN SDCC_HC_TRB           *Trb
  )
{
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet;
  UINTN                                 Base;
  UINT16                                TransMode;
  UINT16                                Cmd;
  UINT32                                BlkCount;
  UINT16                                BlkSize;
  UINT8                                 HostCtrl1;
  UINT64                                AdmaAddr;
  BOOLEAN                               HasData;

  Packet  = Trb->Packet;
  Base    = (UINTN)Private->Config.BaseAddress;
  HasData = (Packet->InDataBuffer != NULL) || (Packet->OutDataBuffer != NULL);

  SdccWrite16 (Base, SDCC_HC_ERR_INT_STS, 0xFFFFu);
  SdccWrite16 (Base, SDCC_HC_NOR_INT_STS, 0xFF3Fu);

  HostCtrl1 = (UINT8)(SdccRead8 (Base, SDCC_HC_HOST_CTRL1) & ~(UINT8)(BIT4 | BIT3));
  if (HasData && (Trb->AdmaDesc != NULL)) {
    HostCtrl1 |= SDCC_HC_CTRL1_DMA_SEL_ADMA64;
  }
  SdccWrite8 (Base, SDCC_HC_HOST_CTRL1, HostCtrl1 | SDCC_HC_CTRL1_LED);

  if (HasData && (Trb->AdmaDesc != NULL)) {
    AdmaAddr = (UINT64)Trb->AdmaDescPhy;
    MmioWrite32 (Base + SDCC_HC_ADMA_SYS_ADDR + 4u, (UINT32)(AdmaAddr >> 32));
    MmioWrite32 (Base + SDCC_HC_ADMA_SYS_ADDR,      (UINT32)(AdmaAddr & 0xFFFFFFFFu));
  }

  BlkSize  = HasData ? Trb->BlockSize : 0u;
  BlkCount = HasData ? (Trb->DataLen / Trb->BlockSize) : 0u;
  SdccWrite16 (Base, SDCC_HC_BLK_SIZE, BlkSize);

  if (Private->ControllerVersion >= SDCC_HC_CTRL_VER_410) {
    SdccWrite32 (Base, SDCC_HC_SDMA_ADDR, BlkCount);
  } else {
    SdccWrite16 (Base, SDCC_HC_BLK_COUNT, (UINT16)BlkCount);
  }

  SdccWrite32 (Base, SDCC_HC_ARG1, Packet->SdMmcCmdBlk->CommandArgument);

  TransMode = 0;
  if (HasData) {
    if (Trb->AdmaDesc != NULL) {
      TransMode |= BIT0;
    }
    if (Trb->Read) {
      TransMode |= BIT4;
    }
    if (BlkCount > 1) {
      TransMode |= BIT5 | BIT1;
    }
    if (!Private->Config.IsEmmc && (BlkCount > 1)) {
      TransMode |= BIT2;   // Auto CMD12 for SD multi-block
    }
  }
  SdccWrite16 (Base, SDCC_HC_TRANS_MOD, TransMode);

  Cmd = (UINT16)(LShiftU64 (Packet->SdMmcCmdBlk->CommandIndex, 8));
  if (Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeAdtc) {
    Cmd |= BIT5;
  }

  if (Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeBc) {
    goto FireCommand;
  }

  switch (Packet->SdMmcCmdBlk->ResponseType) {
    case SdMmcResponseTypeR1:
    case SdMmcResponseTypeR5:
    case SdMmcResponseTypeR6:
    case SdMmcResponseTypeR7:
      Cmd |= BIT1 | BIT3 | BIT4;
      break;
    case SdMmcResponseTypeR2:
      Cmd |= BIT0 | BIT3;
      break;
    case SdMmcResponseTypeR3:
    case SdMmcResponseTypeR4:
      Cmd |= BIT1;
      break;
    case SdMmcResponseTypeR1b:
    case SdMmcResponseTypeR5b:
      Cmd |= BIT0 | BIT1 | BIT3 | BIT4;
      break;
    default:
      break;
  }

FireCommand:
  SdccWrite16 (Base, SDCC_HC_COMMAND, Cmd);
  return EFI_SUCCESS;
}

// ─── Result polling ───────────────────────────────────────────────────────────

/**
  Poll interrupt status registers for TRB completion.

  Returns EFI_NOT_READY if the command is still in progress.

  Completion criteria:
  - Data commands:  XFER_COMPLETE required (data transferred to/from buffer)
  - R1b (no data):  both CMD_COMPLETE and XFER_COMPLETE required (XFER signals
    DAT0 de-asserted, per SDHCI spec §1.5)
  - All other commands: CMD_COMPLETE suffices
**/
EFI_STATUS
SdccCheckTrbResult (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN SDCC_HC_TRB           *Trb
  )
{
  UINTN    Base;
  UINT16   NorIntSts;
  UINT16   ErrIntSts;
  UINT8    Index;
  UINT32   Response[4];
  BOOLEAN  HasData;
  BOOLEAN  CrcOrEndBitErr;

  Base    = (UINTN)Private->Config.BaseAddress;
  HasData = (Trb->Packet->InDataBuffer != NULL) || (Trb->Packet->OutDataBuffer != NULL);

  NorIntSts = SdccRead16 (Base, SDCC_HC_NOR_INT_STS);

  if (NorIntSts & SDCC_HC_NOR_INT_ERR_INT) {
    ErrIntSts = SdccRead16 (Base, SDCC_HC_ERR_INT_STS);
    DEBUG ((DEBUG_WARN, "[SdccHci] Error CMD%d NOR=0x%04x ERR=0x%04x\n",
            Trb->Packet->SdMmcCmdBlk->CommandIndex, NorIntSts, ErrIntSts));

    if (HasData && (ErrIntSts & BIT4) && (NorIntSts & SDCC_HC_NOR_INT_XFER_COMPLETE)) {
      goto Success;
    }

    SdccHcSoftwareReset (Base, ErrIntSts);
    CrcOrEndBitErr = ((ErrIntSts & (BIT1 | BIT2 | BIT5 | BIT6)) != 0);
    return CrcOrEndBitErr ? EFI_CRC_ERROR : EFI_DEVICE_ERROR;
  }

  if (HasData) {
    if (!(NorIntSts & SDCC_HC_NOR_INT_XFER_COMPLETE)) {
      return EFI_NOT_READY;
    }
  } else if ((Trb->Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1b) ||
             (Trb->Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR5b))
  {
    if (!(NorIntSts & SDCC_HC_NOR_INT_CMD_COMPLETE)) {
      return EFI_NOT_READY;
    }
    if (!(NorIntSts & SDCC_HC_NOR_INT_XFER_COMPLETE)) {
      return EFI_NOT_READY;
    }
  } else {
    if (!(NorIntSts & SDCC_HC_NOR_INT_CMD_COMPLETE)) {
      return EFI_NOT_READY;
    }
  }

Success:
  if (Trb->Packet->SdMmcCmdBlk->CommandType != SdMmcCommandTypeBc) {
    for (Index = 0; Index < 4; Index++) {
      Response[Index] = SdccRead32 (Base, SDCC_HC_RESPONSE + (UINT32)(Index * 4u));
    }
    CopyMem (Trb->Packet->SdMmcStatusBlk, Response, sizeof Response);
  }

  if (HasData && Trb->Read) {
    SdccDmaUnmap (Trb);
  }

  SdccWrite8 (Base, SDCC_HC_HOST_CTRL1,
              SdccRead8 (Base, SDCC_HC_HOST_CTRL1) & (UINT8)~SDCC_HC_CTRL1_LED);

  return EFI_SUCCESS;
}

// ─── Synchronous command helper ───────────────────────────────────────────────

/**
  Issue a single SD/eMMC command synchronously and wait for completion.

  Builds a minimal pass-through packet, creates a TRB, fires it, then polls
  SdccCheckTrbResult() in a 1 µs spin.  Used by the eMMC mode-negotiation
  sequence (CMD6 SWITCH, CMD8 EXT_CSD) and the identification sequence
  (CMD0, CMD1, CMD2, CMD3, CMD7) in the driver entry point.

  For data transfers set InData to the destination buffer and InLen to its size.
  For command-only operations pass InData=NULL, InLen=0.

  @param[in]  Private     Driver instance private data.
  @param[in]  CmdIdx      SD/eMMC command index (0–63).
  @param[in]  CmdType     Command type (Bc, Bcr, Ac, Adtc).
  @param[in]  RespType    Expected response type.
  @param[in]  Argument    32-bit command argument.
  @param[in]  InData      Optional data-in buffer; NULL for command-only.
  @param[in]  InLen       Length of InData in bytes; ignored when InData is NULL.
  @param[out] StsBlk      Receives the response register contents on return.
  @param[in]  TimeoutUs   Poll budget in microseconds; 0 = 3 000 000 µs (3 s).

  @retval EFI_SUCCESS           Command completed without error.
  @retval EFI_OUT_OF_RESOURCES  TRB allocation failed.
  @retval EFI_TIMEOUT           Device did not respond within the poll budget.
  @retval EFI_DEVICE_ERROR      Device returned an error response.
  @retval EFI_CRC_ERROR         CRC or end-bit error in the response.
**/
EFI_STATUS
SdccEmmcCmd (
  IN  SDCC_HC_PRIVATE_DATA         *Private,
  IN  UINT8                         CmdIdx,
  IN  EFI_SD_MMC_COMMAND_TYPE       CmdType,
  IN  EFI_SD_MMC_RESPONSE_TYPE      RespType,
  IN  UINT32                        Argument,
  IN  VOID                         *InData   OPTIONAL,
  IN  UINT32                        InLen,
  OUT EFI_SD_MMC_STATUS_BLOCK      *StsBlk,
  IN  UINT32                        TimeoutUs
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             CmdBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Pkt;
  SDCC_HC_TRB                         *Trb;
  EFI_STATUS                           Rs;
  UINT32                               MaxWait;

  ZeroMem (&CmdBlk, sizeof CmdBlk);
  ZeroMem (StsBlk,  sizeof *StsBlk);
  ZeroMem (&Pkt,    sizeof Pkt);

  CmdBlk.CommandIndex    = CmdIdx;
  CmdBlk.CommandType     = CmdType;
  CmdBlk.ResponseType    = RespType;
  CmdBlk.CommandArgument = Argument;
  Pkt.SdMmcCmdBlk      = &CmdBlk;
  Pkt.SdMmcStatusBlk   = StsBlk;
  Pkt.Timeout          = 0;  // infinite wait per UEFI spec §17.2 semantics
  Pkt.InDataBuffer     = InData;
  Pkt.InTransferLength = (InData != NULL) ? InLen : 0u;

  Trb = SdccCreateTrb (Private, 0, &Pkt, NULL);
  if (Trb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Rs = SdccExecTrb (Private, Trb);
  if (!EFI_ERROR (Rs)) {
    // TimeoutUs==0 means infinite (UEFI spec §17.2 semantics); use 3 s as cap.
    MaxWait = (TimeoutUs != 0) ? TimeoutUs : 3000000u;
    while (MaxWait-- > 0) {
      Rs = SdccCheckTrbResult (Private, Trb);
      if (Rs != EFI_NOT_READY) {
        break;
      }
      MicroSecondDelay (1);
    }
    if (Rs == EFI_NOT_READY) {
      Rs = EFI_TIMEOUT;
    }
  }

  SdccFreeTrb (Trb);
  return Rs;
}
