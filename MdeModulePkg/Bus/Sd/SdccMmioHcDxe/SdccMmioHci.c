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

/** Write an 8-bit register at Base + Offset. **/
STATIC
VOID
SdccWrite8 (
  UINTN   B,
  UINT32  O,
  UINT8   V
  )
{
  MmioWrite8 (B + O, V);
}

/** Read an 8-bit register at Base + Offset. **/
STATIC
UINT8
SdccRead8 (
  UINTN   B,
  UINT32  O
  )
{
  return MmioRead8 (B + O);
}

/** Write a 16-bit register at Base + Offset. **/
STATIC
VOID
SdccWrite16 (
  UINTN   B,
  UINT32  O,
  UINT16  V
  )
{
  MmioWrite16 (B + O, V);
}

/** Read a 16-bit register at Base + Offset. **/
STATIC
UINT16
SdccRead16 (
  UINTN   B,
  UINT32  O
  )
{
  return MmioRead16 (B + O);
}

/** Write a 32-bit register at Base + Offset. **/
STATIC
VOID
SdccWrite32 (
  UINTN   B,
  UINT32  O,
  UINT32  V
  )
{
  MmioWrite32 (B + O, V);
}

/** Read a 32-bit register at Base + Offset. **/
STATIC
UINT32
SdccRead32 (
  UINTN   B,
  UINT32  O
  )
{
  return MmioRead32 (B + O);
}

/** Read-modify-write a 16-bit register at Base + Offset: set bits in Mask. **/
STATIC
VOID
SdccOr16 (
  UINTN   B,
  UINT32  O,
  UINT16  M
  )
{
  SdccWrite16 (B, O, SdccRead16 (B, O) | M);
}

/** Read-modify-write a 16-bit register at Base + Offset: clear bits in Mask. **/
STATIC
VOID
SdccAnd16 (
  UINTN   B,
  UINT32  O,
  UINT16  M
  )
{
  SdccWrite16 (B, O, SdccRead16 (B, O) & M);
}

// ─── Wait helpers ─────────────────────────────────────────────────────────────

/**
  Poll a 32-bit MMIO register until (reg & Mask) == Expected or timeout.

  Spins in 1 µs increments.  Intended for short hardware-ready polls
  (SDHCI clock stable, reset complete, DLL lock); do not call for multi-ms
  waits from TPL_NOTIFY context.

  @param[in]  Base       MMIO base address of the SDCC host controller.
  @param[in]  Off        Register byte offset from Base.
  @param[in]  Mask       Bit mask to apply to the register value.
  @param[in]  Expected   Expected value of (reg & Mask) for success.
  @param[in]  TimeoutUs  Maximum wait time in microseconds.

  @retval EFI_SUCCESS   Register reached the expected value within timeout.
  @retval EFI_TIMEOUT   Timeout elapsed before the condition was satisfied.
**/
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
  UINT64  Rem;

  Rem = TimeoutUs;

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

/**
  Poll an 8-bit SDHCI register until (reg & Mask) == Expected or timeout.

  @param[in]  Base       MMIO base address of the HC.
  @param[in]  Off        Register offset from Base.
  @param[in]  Mask       Bitmask to AND before comparing.
  @param[in]  Expected   Value to compare against (after masking).
  @param[in]  TimeoutUs  Timeout in microseconds; MAX_UINT64 = infinite.

  @retval EFI_SUCCESS    Register reached the expected value within the timeout.
  @retval EFI_TIMEOUT    Register did not reach the expected value in time.
**/
EFI_STATUS
SdccWaitSet8 (
  IN UINTN   Base,
  IN UINT32  Off,
  IN UINT8   Mask,
  IN UINT8   Expected,
  IN UINT64  TimeoutUs
  )
{
  UINT64  Rem;

  Rem = TimeoutUs;

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

/**
  Read the SDHCI Host Controller version (CTRL_VER bits[7:0]).

  @param[in]   Base     MMIO base address of the HC.
  @param[out]  Version  Populated with the host controller version byte.

  @retval EFI_SUCCESS            Version read.
  @retval EFI_INVALID_PARAMETER  Version is NULL.
**/
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

/**
  Read the 64-bit SDHCI capabilities register pair (CAP1 + CAP2) into Cap.

  @param[in]   Base  MMIO base address of the HC.
  @param[out]  Cap   Populated with capability bits from CAP1/CAP2.

  @retval EFI_SUCCESS            Capabilities read.
  @retval EFI_INVALID_PARAMETER  Cap is NULL.
**/
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

/** Enable all interrupt status enables (NOR_INT_STS_EN and ERR_INT_STS_EN). **/
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
    UINT32  CtlAck;

    CtlAck = 0;
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

/**
  Gate SDCLK by clearing SDCLK_EN in the clock control register.

  Must be called before modifying the clock divisor to avoid glitches on the
  bus.  Waits for CMD/DAT inhibit to clear before gating.

  @param[in]  Base  MMIO base address of the HC.

  @retval EFI_SUCCESS  Clock gated.
  @retval EFI_TIMEOUT  CMD/DAT inhibit did not clear in time.
**/
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

  On standard SDHCI hardware the SDCD# pin is wired and BIT16 (Card Inserted)
  reflects card presence after debouncing.  On platforms where the CD pin is
  not wired (NoCdPin=TRUE), the primary check cannot be used; falling back to
  DAT[3:0] line signal level (bits[23:20]) detects the card's internal pull-ups.

  @param[in]  Base     SDHCI register window base address.
  @param[in]  NoCdPin  TRUE if the Card Detect pin is not physically wired on
                       this platform.  When TRUE and the primary check fails,
                       a non-zero DAT[3:0] line level is used as presence signal.
  @param[out] Present  Set to TRUE if a card is present and electrically stable;
                       FALSE otherwise.

  @retval EFI_SUCCESS            Present was populated successfully.
  @retval EFI_INVALID_PARAMETER  Present is NULL.
**/
EFI_STATUS
SdccHcCardDetect (
  IN  UINTN    Base,
  IN  BOOLEAN  NoCdPin,
  OUT BOOLEAN *Present
  )
{
  UINT32  State;

  if (Present == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  State = SdccRead32 (Base, SDCC_HC_PRESENT_STATE);
  if ((State & SDCC_HC_STATE_CARD_STABLE) && (State & SDCC_HC_STATE_CARD_PRESENT)) {
    *Present = TRUE;
  } else if (NoCdPin) {
    *Present = (State & SDCC_HC_STATE_DAT_0_3_MASK) != 0;
  } else {
    *Present = FALSE;
  }
  return EFI_SUCCESS;
}

// ─── Cold init (SD card, Sdc2) ───────────────────────────────────────────────

/**
  SD card bus identification: CMD0 → CMD8 → ACMD41 (poll) → CMD2.

  Runs the SD Physical Layer Simplified Spec §4.2 initialization sequence so
  that the card is in Standby state when SdDxe calls SdSetRca (CMD3).

  ACMD41 is implemented as two explicit commands: CMD55 (APP_CMD, Ac, R1, arg=0)
  followed by CMD41 (SD_SEND_OP_COND, Bcr, R3).  The argument 0x41FF8000 sets:
    BIT30 (HCS)  — host supports SDHC/SDXC
    BIT28 (XPC)  — maximum performance (150mA at 3.3V)
    BIT24 (S18R) — host requests 1.8V UHS-I signaling
    bits[23:0]   — 3.2–3.4V window (0xFF8000)

  If the card asserts S18A (BIT24 of OCR), a UHS-I voltage switch sequence is
  performed: CMD11 (VOLTAGE_SWITCH) → host 1.8V signal enable → SDR104 mode
  selection → CMD19 execute-tuning loop (SDHCI spec §3.7).

  @param[in]  Private   Driver instance; Base address and MediaPresent must be valid.

  @retval EFI_SUCCESS        Card is in Standby state, ready for CMD3.
  @retval EFI_DEVICE_ERROR   ACMD41 did not assert OCR[31] within 100 retries.
  @retval Others             Propagated from SdccEmmcCmd or SdccHcUhs1SignalSwitch.
**/
EFI_STATUS
SdccHcSdCardInit (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS             Status;
  EFI_SD_MMC_STATUS_BLOCK  Sts;
  UINT32                 Ocr;
  UINTN                  Retry;
  UINTN                  Base;

  Base = (UINTN)Private->Config.BaseAddress;

  //
  // SD Physical Layer Spec §6.4.1: after power-on and clock supply the host
  // must wait at least 74 SD clock cycles before sending CMD0.  At 400 kHz
  // that is 185 µs; use 1 ms to cover any clock-ramp variance.
  //
  MicroSecondDelay (1000u);

  //
  // CMD0: GO_IDLE_STATE — broadcast reset, no response.
  //
  Status = SdccEmmcCmd (Private, SD_GO_IDLE_STATE,
                        SdMmcCommandTypeBc, SdMmcResponseTypeR1,
                        0x0, NULL, 0, &Sts, 1000u);
  //
  // CMD0 has no response; EFI_TIMEOUT is normal — treat as success.
  //
  if (EFI_ERROR (Status) && (Status != EFI_TIMEOUT)) {
    DEBUG ((DEBUG_WARN, "[SdccHci] SdCardInit: CMD0 returned %r (ignored)\n", Status));
  }
  MicroSecondDelay (1000u);  // let card settle in Idle state after CMD0

  //
  // CMD8: SEND_IF_COND — 3.3V supply (0x1), check pattern 0xAA.
  // Fails for SD 1.x cards (no R7 response); ignore the error and continue.
  //
  Status = SdccEmmcCmd (Private, SD_SEND_IF_COND,
                        SdMmcCommandTypeBcr, SdMmcResponseTypeR7,
                        (0x1u << 8) | 0xAAu, NULL, 0, &Sts, 100000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccHci] SdCardInit: CMD8 failed %r (SD 1.x card?)\n", Status));
  } else {
    if ((Sts.Resp0 & 0xFFu) != 0xAAu) {
      DEBUG ((DEBUG_WARN, "[SdccHci] SdCardInit: CMD8 echo mismatch Resp0=0x%x\n",
              Sts.Resp0));
    }
  }

  //
  // ACMD41 poll loop: CMD55 (APP_CMD) + CMD41 (SD_SEND_OP_COND) until OCR[31].
  // Arg: HCS=BIT30, XPC=BIT28, S18R=BIT24 (request 1.8V switching), VDD window.
  //
  Ocr   = 0;
  Retry = 0;
  do {
    //
    // CMD55: APP_CMD, Ac, R1, argument = (RCA=0) << 16.
    //
    Status = SdccEmmcCmd (Private, SD_APP_CMD,
                          SdMmcCommandTypeAc, SdMmcResponseTypeR1,
                          0x0, NULL, 0, &Sts, 100000u);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: CMD55 failed %r\n", Status));
      return Status;
    }

    //
    // CMD41: SD_SEND_OP_COND, Bcr, R3.
    // BIT24 = S18R: request 1.8V signaling.
    //
    Status = SdccEmmcCmd (Private, SD_SEND_OP_COND,
                          SdMmcCommandTypeBcr, SdMmcResponseTypeR3,
                          0x41FF8000u, NULL, 0, &Sts, 100000u);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: CMD41 failed %r\n", Status));
      return Status;
    }

    Ocr = Sts.Resp0;
    if (++Retry >= 100u) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: ACMD41 timeout OCR=0x%x\n", Ocr));
      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (10000u);  // 10 ms between polls
  } while ((Ocr & BIT31) == 0);

  DEBUG ((DEBUG_WARN,
          "[SdccHci] SdCardInit: ACMD41 complete OCR=0x%x SDHC=%d S18A=%d\n",
          Ocr, (Ocr & BIT30) ? 1 : 0, (Ocr & BIT24) ? 1 : 0));

  //
  // UHS-I voltage switch: if the card set S18A (BIT24) in the OCR response,
  // issue CMD11 (VOLTAGE_SWITCH) and complete the host-side 1.8V transition.
  // SDHCI Simplified Spec v3.0 §3.6.1.
  //
  if (Ocr & BIT24) {
    Status = SdccEmmcCmd (Private, SD_VOLTAGE_SWITCH,
                          SdMmcCommandTypeAc, SdMmcResponseTypeR1,
                          0x0, NULL, 0, &Sts, 100000u);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: CMD11 failed %r\n", Status));
      return Status;
    }

    Status = SdccHcUhs1SignalSwitch (Base);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: 1.8V signal switch failed %r\n", Status));
      return Status;
    }

    //
    // Set UHS_MODE_SEL=SDR104 in HOST_CTRL2.  Must be done before CMD19 tuning
    // (SDHCI spec §3.7 requires UHS mode selection before EXECUTE_TUNING is set).
    //
    {
      UINT16  Ctrl2;

      Ctrl2  = SdccRead16 (Base, SDCC_HC_HOST_CTRL2);
      Ctrl2 &= (UINT16)~SDCC_HC_CTRL2_UHS_MASK;
      Ctrl2 |= SDCC_HC_CTRL2_UHS_SDR104;
      SdccWrite16 (Base, SDCC_HC_HOST_CTRL2, Ctrl2);
      DEBUG ((DEBUG_WARN,
              "[SdccHci] SdCardInit: HOST_CTRL2=0x%04x (SDR104)\n",
              (UINT32)SdccRead16 (Base, SDCC_HC_HOST_CTRL2)));
    }

    //
    // UHS_MCLK clock/strobe source — routes the DLL's MCLK-derived sampling
    // clock into the HC's card-interface logic.  Downstream sets this via
    // HAL_sdhci_HCSelectIn() inside sdcc_hc_set_uhs_mode_select() for
    // SDCC_SDR104_MODE (HAL_SDCC_CLOCK_UHS_MCLK=4), mirroring the eMMC
    // HS400 path's DDR200/HS400 source in SdccHcEnterHS400Final.
    // Without this, the DLL locks against the wrong clock source.
    //
    SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcUhsMclk);

    //
    // Reprogram apps_clk (and SDHCI divisor) to the SDR104 rate *before* DLL
    // init.  The DLL's CM_DLL_SDC4 lock detector needs a live MCLK edge at
    // (or near) the operating frequency to lock — at the 400 kHz identify
    // clock still in effect from ColdInit, DLL_STATUS never asserts DLL_LOCK
    // and SdccPlatformDllInit times out.  Mirrors the downstream reference
    // (sdcc_hc_set_uhs_mode_select() → sdcc_config_clk()) and the eMMC
    // HS200/HS400 paths in SdccMmioEmmcMode.c, which switch apps_clk before
    // calling their DLL init routines.  Bypass divisor (N=0): SDCLK = apps_clk.
    //
    Status = SdccHcStopClock (Base);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: StopClock before SDR104 failed %r\n", Status));
      return Status;
    }
    Status = SdccPlatformSetClockFreqHz (&Private->Config, Private->Config.ClkHz_SDR104);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: SetClockFreqHz(SDR104) failed %r\n", Status));
      return Status;
    }
    Status = SdccHcClockSupply (Base, Private->ControllerVersion,
                                 Private->Config.ClkHz_SDR104 / 1000u,
                                 Private->Config.ClkHz_SDR104 / 1000u);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: ClockSupply(SDR104) failed %r\n", Status));
      return Status;
    }
    DEBUG ((DEBUG_WARN,
            "[SdccHci] SdCardInit: SDR104 apps_clk=%uHz CLOCK_CTRL=0x%04x\n",
            Private->Config.ClkHz_SDR104,
            (UINT32)SdccRead16 (Base, SDCC_HC_CLOCK_CTRL)));

    //
    // Initialise the Qualcomm SDCC5 DLL for SDR104 single-data-rate mode
    // (platform-specific, see SdccPlatformLib).  The DLL must be locked
    // before CMD19 tuning blocks are issued; the tuning loop adjusts the
    // sampling phase within the already-running DLL.  On failure, fall
    // through to the tuning block with TuneOk=FALSE so the UHS mode is
    // cleared and the card enumerates at a lower speed.
    //
    Status = SdccPlatformDllInit (&Private->Config, SdccDllModeSdr104);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "[SdccHci] SdCardInit: SDR104 DLL init failed %r — "
              "clearing SDR104 mode, card will enumerate at lower speed\n",
              Status));
      SdccAnd16 (Base, SDCC_HC_HOST_CTRL2,
                 (UINT16)~SDCC_HC_CTRL2_UHS_SDR104);
      //
      // Restore SDCLK to the High Speed rate — the SDR104 apps_clk switch
      // above left the card without a matching clock now that UHS mode is
      // cleared.  Downstream (sdcc_sd_switch_to_hs) performs the analogous
      // fallback via sdcc_hc_set_uhs_mode_select(..., SDCC_SDR50_MODE).
      //
      SdccHcClockSupply (Base, Private->ControllerVersion,
                          Private->Config.ClkHz_SDR104 / 1000u,
                          Private->Config.ClkHz_HS / 1000u);
      goto SkipTuning;
    }

    //
    // CMD19 execute-tuning loop — SDHCI Simplified Spec v3.0 §3.7.
    // Write EXECUTE_TUNING, then send CMD19 in a loop until SAMPLING_CLK_SELECT
    // is set (tuning complete) or until 40 cycles elapse (timeout/failure).
    // Each CMD19 sends a 64-byte tuning block; the HC checks the CRC and
    // adjusts the sampling phase.
    //
    {
      UINTN   TuneIdx;
      UINT16  Ctrl2;
      BOOLEAN TuneOk = FALSE;

      SdccOr16 (Base, SDCC_HC_HOST_CTRL2, SDCC_HC_CTRL2_EXECUTE_TUNING);

      for (TuneIdx = 0; TuneIdx < 40u; TuneIdx++) {
        Status = SdccEmmcCmd (Private, SD_SEND_TUNING_BLOCK,
                              SdMmcCommandTypeBcr, SdMmcResponseTypeR1,
                              0x0, NULL, 0, &Sts, 150000u);
        MicroSecondDelay (1000u);

        Ctrl2 = SdccRead16 (Base, SDCC_HC_HOST_CTRL2);
        if (!(Ctrl2 & SDCC_HC_CTRL2_EXECUTE_TUNING)) {
          //
          // HC cleared EXECUTE_TUNING — check SAMPLING_CLK_SELECT.
          //
          if (Ctrl2 & SDCC_HC_CTRL2_SAMPLING_CLK) {
            TuneOk = TRUE;
          }
          break;
        }
      }

      DEBUG ((DEBUG_WARN,
              "[SdccHci] SdCardInit: CMD19 tuning %s after %d cycles "
              "HOST_CTRL2=0x%04x\n",
              TuneOk ? "PASS" : "FAIL", (UINT32)TuneIdx,
              (UINT32)SdccRead16 (Base, SDCC_HC_HOST_CTRL2)));

      if (!TuneOk) {
        //
        // Tuning failed — clear EXECUTE_TUNING and fall back.
        // SdDxe will enumerate the card at lower speed.
        //
        SdccAnd16 (Base, SDCC_HC_HOST_CTRL2,
                   (UINT16)~(SDCC_HC_CTRL2_EXECUTE_TUNING | SDCC_HC_CTRL2_UHS_SDR104));
        SdccHcClockSupply (Base, Private->ControllerVersion,
                            Private->Config.ClkHz_SDR104 / 1000u,
                            Private->Config.ClkHz_HS / 1000u);
        DEBUG ((DEBUG_WARN, "[SdccHci] SdCardInit: SDR104 tuning failed — "
                "UHS mode cleared, card will enumerate at lower speed\n"));
      }
    }
  }

SkipTuning:
  //
  // CMD2: ALL_SEND_CID — card moves from Ready to Identification state.
  //
  Status = SdccEmmcCmd (Private, SD_ALL_SEND_CID,
                        SdMmcCommandTypeBcr, SdMmcResponseTypeR2,
                        0x0, NULL, 0, &Sts, 100000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] SdCardInit: CMD2 failed %r\n", Status));
    return Status;
  }

  //
  // Card is now in Identification state.  SdDxe will issue CMD3 (SdSetRca)
  // which moves the card to Standby state and captures the assigned RCA.
  //
  DEBUG ((DEBUG_WARN, "[SdccHci] SdCardInit: SD card in Identification state, "
         "ready for SdDxe CMD3\n"));
  return EFI_SUCCESS;
}

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
  @retval Others              Propagated from SdccHcReadVersion(),
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

  DEBUG ((DEBUG_WARN, "[SdccHci] ColdInit: Base=0x%lx CapsBaseClkFreqKHz=%u\n",
          (UINT64)Base, Private->Config.CapsBaseClkFreqKHz));

  //
  // SW_RST_ALL (SDCC_HC_SW_RST bit[7:0] = 0xFF) resets the CMD/DAT state
  // machines, PWRCTL masks, and interrupt enables in one step (HPG §7.3.8.3 /
  // SdccHcReset).  Skipped when Config.SkipSwRstAll is set — some platforms
  // write-protect this register from the exception level UEFI runs at,
  // leaving SW_RST stuck at 0x1 and the reset poll timing out.  When skipped,
  // unmask PWRCTL events and enable interrupt status directly (the two
  // operations SdccHcReset() performs after its reset completes), then issue
  // only SW_RST_DAT_LINE — which operates independently of SW_RST_ALL and is
  // not subject to the same write-protection — to flush stale DAT-state-
  // machine residue left by a prior UEFI session.  Without this, a prior
  // CMD18 that completed normally can leave DAT_INHIBIT asserted, blocking
  // the first data command on the next boot.
  //
  if (Private->Config.SkipSwRstAll) {
    SdccWrite32 (Base, SDCC_HC_PWRCTL_MASK, 0xFu);
    SdccHcEnableInterrupts (Base);

    {
      EFI_STATUS  RstStatus;
      SdccWrite8 (Base, SDCC_HC_SW_RST, SDCC_HC_RST_DAT_LINE);
      RstStatus = SdccWaitSet8 (Base, SDCC_HC_SW_RST,
                                 SDCC_HC_RST_DAT_LINE, 0,
                                 SDCC_HC_GENERIC_TIMEOUT_US);
      DEBUG ((DEBUG_WARN,
              "[SdccHci] ColdInit: SW_RST_DAT_LINE %s SW_RST=0x%x PRESENT=0x%x\n",
              EFI_ERROR (RstStatus) ? L"STUCK" : L"OK",
              (UINT32)SdccRead8 (Base, SDCC_HC_SW_RST),
              SdccRead32 (Base, SDCC_HC_PRESENT_STATE)));
    }
  } else {
    Status = SdccHcReset (Base);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: SdccHcReset failed: %r\n", Status));
      return Status;
    }
  }

  DEBUG ((DEBUG_WARN, "[SdccHci] ColdInit: PWRCTL_STATUS=0x%x PWRCTL_MASK=0x%x\n",
          SdccRead32 (Base, SDCC_HC_PWRCTL_STATUS),
          SdccRead32 (Base, SDCC_HC_PWRCTL_MASK)));

  Status = SdccHcReadVersion (Base, &Private->ControllerVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: ReadVersion failed: %r\n", Status));
    return Status;
  }

  Status = SdccHcReadCapability (Base, &Private->Capability);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: ReadCapability failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_WARN,
          "[SdccHci] ColdInit: CtrlVer=0x%x CAP=0x%llx "
          "V33=%d V30=%d V18=%d SysBus64V3=%d\n",
          Private->ControllerVersion, Private->Capability.Raw,
          Private->Capability.Voltage33, Private->Capability.Voltage30,
          Private->Capability.Voltage18, Private->Capability.SysBus64V3));

  //
  // Use the platform-configured base clock.  The CAPS value is not always
  // reliable on SDCC5 (SDCC5 reports a software-overridden value).
  //
  if (Private->Config.CapsBaseClkFreqKHz == 0) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: CapsBaseClkFreqKHz=0 — check PCD\n"));
    return EFI_DEVICE_ERROR;
  }

  Private->CurrentAppsClkHz = Private->Config.CapsBaseClkFreqKHz * 1000u;

  Status = SdccHcInitPower (Base, Private->Capability);
  if (EFI_ERROR (Status)) {
    if (Private->Config.ForcePowerOn3P3V) {
      //
      // The platform has hardwired 3.3V via the PMIC and does not populate
      // SDHCI CAP1[26:24] voltage bits.  Write POWER_CTRL directly.
      //
      SdccWrite8 (Base, SDCC_HC_POWER_CTRL, SDCC_HC_POWER_3P3V | SDCC_HC_POWER_ON);
      DEBUG ((DEBUG_WARN,
              "[SdccHci] ColdInit: ForcePowerOn3P3V — "
              "CAP has no voltage bits; wrote POWER_CTRL=0x%x\n",
              (UINT32)(SDCC_HC_POWER_3P3V | SDCC_HC_POWER_ON)));
    } else {
      DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: InitPower failed: %r "
              "(no voltage bits in CAP — AHB clock may not be running)\n", Status));
      return Status;
    }
  }

  DEBUG ((DEBUG_WARN, "[SdccHci] ColdInit: Power on OK. POWER_CTRL=0x%x\n",
          SdccRead8 (Base, SDCC_HC_POWER_CTRL)));

  Status = SdccHcClockSupply (Base, Private->ControllerVersion,
                               Private->Config.CapsBaseClkFreqKHz,
                               SDCC_HC_IDENTIFY_CLOCK_KHZ);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: ClockSupply(400kHz) failed: %r\n", Status));
    return Status;
  }

  SdccHcInitTimeout (Base);

  Status = SdccHcEnableV4 (Base, Private->ControllerVersion, Private->Capability);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHci] ColdInit: EnableV4 failed: %r\n", Status));
    return Status;
  }

  Status = SdccHcCardDetect (Base, Private->Config.NoCdPin, &Private->MediaPresent);
  DEBUG ((DEBUG_WARN,
          "[SdccHci] ColdInit: CardDetect PRESENT_STATE=0x%x MediaPresent=%d\n",
          SdccRead32 (Base, SDCC_HC_PRESENT_STATE),
          (UINT32)Private->MediaPresent));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Private->MediaPresent) {
    Status = SdccHcSdCardInit (Private);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN,
              "[SdccHci] ColdInit: SD identification failed %r — "
              "MediaPresent cleared; hot-plug will retry\n", Status));
      Private->MediaPresent = FALSE;
    }
  }

  return EFI_SUCCESS;
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

/**
  Map a data buffer for ADMA2.

  Allocates a bounce buffer and remaps it as uncached (EFI_MEMORY_UC) so the
  DMA engine and CPU share a coherent view without explicit cache maintenance.
  Matches the UfsMemHcDxe AllocateBuffer pattern used on this platform.

  If Private->Config.Dma32BitOnly is TRUE, the allocation is restricted to
  below 4 GB — some SDCC5 integrations route the ADMA engine through a bus
  segment narrower than the SDHCI CAP1[28]/CAP2[27] SysBus64 capability bit
  advertises, so the platform must opt in explicitly rather than the driver
  assuming every instance needs the restriction.  When FALSE, the allocation
  is unrestricted.

  @param[in]  Trb  TRB describing the pending transfer; Trb->Private->Config
                   supplies the Dma32BitOnly policy for this controller.
**/
EFI_STATUS
SdccDmaMap (
  IN SDCC_HC_TRB  *Trb
  )
{
  EFI_PHYSICAL_ADDRESS             MaxAddr;
  EFI_STATUS                       Status;
  UINTN                            Pages;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;
  EFI_ALLOCATE_TYPE                AllocType;

  Trb->BounceBuffer = NULL;
  Trb->BouncePages  = 0;

  Pages = EFI_SIZE_TO_PAGES (Trb->DataLen);

  if (Trb->Private->Config.Dma32BitOnly) {
    //
    // Cap the ceiling so the bounce buffer ends strictly below 0xFFFFFFFF.
    // AllocateMaxAddress returns the highest page at or below the ceiling.
    // With ceiling = MAX_UINT32 - N*4096, a N-page allocation starts exactly at
    // that ceiling and ends at 0xFFFFFFFF — still on the boundary.  Subtracting
    // one extra page pushes the highest possible start to
    //   MAX_UINT32 - (N+1)*4096
    // so the last byte is at most 0xFFFEFFFF (N=16: buffer at 0xFFFEF000–0xFFFFEFFF).
    //
    MaxAddr   = MAX_UINT32 - EFI_PAGES_TO_SIZE (Pages + 1);
    AllocType = AllocateMaxAddress;
  } else {
    MaxAddr   = 0;
    AllocType = AllocateAnyPages;
  }

  Status  = gBS->AllocatePages (
                   AllocType,
                   EfiBootServicesData,
                   Pages,
                   &MaxAddr
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[SdccHci] Bounce buffer alloc failed (%u bytes): %r\n",
            Trb->DataLen, Status));
    return Status;
  }

  Status = gDS->GetMemorySpaceDescriptor (MaxAddr, &GcdDescriptor);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[SdccHci] GetMemorySpaceDescriptor failed addr=0x%lx: %r\n",
            MaxAddr, Status));
    gBS->FreePages (MaxAddr, Pages);
    return EFI_DEVICE_ERROR;
  }

  if ((GcdDescriptor.Capabilities & EFI_MEMORY_UC) == 0) {
    DEBUG ((DEBUG_ERROR,
            "[SdccHci] EFI_MEMORY_UC not in GCD caps; cannot use uncached DMA buffer\n"));
    gBS->FreePages (MaxAddr, Pages);
    return EFI_DEVICE_ERROR;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  MaxAddr,
                  EFI_PAGES_TO_SIZE (Pages),
                  (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "[SdccHci] UC remap failed addr=0x%lx: %r\n",
            MaxAddr, Status));
    gBS->FreePages (MaxAddr, Pages);
    return EFI_DEVICE_ERROR;
  }

  Trb->BounceBuffer = (VOID *)(UINTN)MaxAddr;
  Trb->BouncePages  = Pages;
  Trb->DataPhy      = MaxAddr;

  if (!Trb->Read) {
    CopyMem (Trb->BounceBuffer, Trb->Data, Trb->DataLen);
  } else {
    ZeroMem (Trb->BounceBuffer, Trb->DataLen);
  }

  Trb->DataCacheFlushed = TRUE;
  return EFI_SUCCESS;
}

/**
  Unmap a data buffer after DMA completion.

  For read transfers, copies DMA output from the uncached bounce buffer back
  to the caller's original buffer.  Restores EFI_MEMORY_WB before freeing so
  pages are returned to the pool in a cache-coherent state.

  @param[in]  Trb  TRB whose data buffer should be unmapped.
**/
VOID
SdccDmaUnmap (
  IN SDCC_HC_TRB  *Trb
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  GcdDescriptor;
  EFI_PHYSICAL_ADDRESS             BounceAddr;

  if (!Trb->DataCacheFlushed) {
    return;
  }

  if (Trb->BounceBuffer != NULL) {
    if (Trb->Read) {
      CopyMem (Trb->Data, Trb->BounceBuffer, Trb->DataLen);
    }

    BounceAddr = (EFI_PHYSICAL_ADDRESS)(UINTN)Trb->BounceBuffer;
    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (BounceAddr, &GcdDescriptor)) &&
        ((GcdDescriptor.Attributes & EFI_CACHE_ATTRIBUTE_MASK) == EFI_MEMORY_UC))
    {
      gDS->SetMemorySpaceAttributes (
             BounceAddr,
             EFI_PAGES_TO_SIZE (Trb->BouncePages),
             (GcdDescriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WB
             );
    }

    gBS->FreePages (BounceAddr, Trb->BouncePages);
    Trb->BounceBuffer = NULL;
    Trb->BouncePages  = 0;
  }

  Trb->DataCacheFlushed = FALSE;
}

// ─── ADMA2 descriptor table ───────────────────────────────────────────────────

/**
  Fill the ADMA2 64-bit descriptor table.

  Qualcomm SDCC5 descriptor bit layout (differs from SDHCI standard spec):
    Valid = bit[0], End = bit[1], Int = bit[2], Act = bits[5:4] (0b10 = TRAN)
  This matches the Agatti reference sSDHCADMADescriptor struct.

  The per-instance embedded table (Private->AdmaDescTable) is used whenever its
  address already satisfies the platform's DMA addressing policy.  If
  Private->Config.Dma32BitOnly is TRUE and the embedded table landed above
  4 GB (AllocateZeroPool placement is not guaranteed below 4 GB), a dedicated
  below-4GB page is allocated for this TRB instead — see SdccDmaMap for the
  Dma32BitOnly rationale.  When Dma32BitOnly is FALSE, the embedded table's
  address is used unconditionally.
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
  SDCC_ADMA2_64_DESC   *Table;
  UINTN                 TablePages;
  EFI_PHYSICAL_ADDRESS  TableMaxAddr;
  EFI_STATUS            Status;

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
  // Use the per-instance embedded table unless the platform requires sub-4GB
  // DMA addressing (Dma32BitOnly) and the embedded table's address doesn't
  // satisfy it.  AdmaDescPages > 0 tells SdccFreeAdmaDescTable to call
  // FreePages on completion.
  //
  if (!Private->Config.Dma32BitOnly ||
      ((UINTN)Private->AdmaDescTable <= MAX_UINT32))
  {
    Table      = Private->AdmaDescTable;
    TablePages = 0;
  } else {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR  TableGcdDesc;

    TablePages   = EFI_SIZE_TO_PAGES (sizeof (Private->AdmaDescTable));
    //
    // Same off-by-one fix as the data bounce buffer: subtract one extra page so
    // the descriptor table ends strictly below 0xFFFFFFFF.
    //
    TableMaxAddr = MAX_UINT32 - EFI_PAGES_TO_SIZE (TablePages + 1);
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    TablePages,
                    &TableMaxAddr
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "[SdccHci] Failed to allocate below-4GB descriptor table: %r\n",
              Status));
      return Status;
    }
    Status = gDS->GetMemorySpaceDescriptor (TableMaxAddr, &TableGcdDesc);
    if (!EFI_ERROR (Status) && (TableGcdDesc.Capabilities & EFI_MEMORY_UC)) {
      gDS->SetMemorySpaceAttributes (
             TableMaxAddr,
             EFI_PAGES_TO_SIZE (TablePages),
             (TableGcdDesc.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC
             );
    }
    Table = (SDCC_ADMA2_64_DESC *)(UINTN)TableMaxAddr;
  }

  //
  // Zero the ENTIRE table, not just NumEntries.  A prior transfer may have used
  // more descriptors, leaving a stale End=1 marker mid-table.  The ADMA engine
  // walks until it sees End=1, so a stale marker would silently truncate this
  // transfer.  Zeroing all 512 entries (~6 KB) is cheap and eliminates the risk.
  //
  ZeroMem (Table, sizeof (Private->AdmaDescTable));

  Remaining = Trb->DataLen;
  DataPhy   = Trb->DataPhy;

  for (i = 0; i < NumEntries; i++) {
    ChunkSize             = MIN (Remaining, SDCC_ADMA2_MAX_DATA_16B);
    Table[i].Valid        = 1;
    Table[i].Act          = SDCC_ADMA2_ACT_TRAN;
    Table[i].Length       = (UINT16)(ChunkSize == SIZE_64KB ? 0 : (UINT16)ChunkSize);
    Table[i].LowerAddress = (UINT32)(DataPhy & 0xFFFFFFFFu);
    Table[i].UpperAddress = (UINT32)(DataPhy >> 32);
    DataPhy   += ChunkSize;
    Remaining -= ChunkSize;
  }
  Table[NumEntries - 1].End = 1;

  // UC memory: no cache flush needed; CPU writes go directly to DRAM.

  Trb->AdmaDesc      = Table;
  Trb->AdmaDescPhy   = (EFI_PHYSICAL_ADDRESS)(UINTN)Table;
  Trb->AdmaDescPages = TablePages;

  return EFI_SUCCESS;
}

/**
  Release ADMA2 descriptor table resources.

  For per-TRB below-4GB overflow allocations (AdmaDescPages > 0), frees the
  pages via gBS->FreePages.  For tables embedded in Private (AdmaDescPages == 0),
  clears the TRB pointer only.

  @param[in]  Trb  TRB whose ADMA descriptor memory should be released.
**/
VOID
SdccFreeAdmaDescTable (
  IN SDCC_HC_TRB  *Trb
  )
{
  if (Trb->AdmaDesc == NULL) {
    return;
  }
  if (Trb->AdmaDescPages > 0) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR  FreeGcdDesc;
    EFI_PHYSICAL_ADDRESS             DescAddr;

    DescAddr = (EFI_PHYSICAL_ADDRESS)(UINTN)Trb->AdmaDesc;
    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (DescAddr, &FreeGcdDesc)) &&
        ((FreeGcdDesc.Attributes & EFI_CACHE_ATTRIBUTE_MASK) == EFI_MEMORY_UC))
    {
      gDS->SetMemorySpaceAttributes (
             DescAddr,
             EFI_PAGES_TO_SIZE (Trb->AdmaDescPages),
             (FreeGcdDesc.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WB
             );
    }
    gBS->FreePages (DescAddr, Trb->AdmaDescPages);
  }
  Trb->AdmaDesc      = NULL;
  Trb->AdmaDescPhy   = 0;
  Trb->AdmaDescPages = 0;
}

// ─── CMD/DAT soft reset after error ──────────────────────────────────────────

/**
  Issue CMD_LINE and/or DAT_LINE software reset after a transfer error.

  Per SDHCI §3.3.7, the host must reset CMD_LINE when CMD error bits are set
  (bits[3:0]) or AutoCMD12/23 error (bit8), and DAT_LINE when data error bits
  are set (bits[6:4]).  Resets are no-op if the corresponding error bits are
  clear.

  @param[in]  Base          MMIO base address of the HC.
  @param[in]  ErrIntStatus  ERR_INT_STS value captured after the failed TRB.

  @retval EFI_SUCCESS  Reset(s) completed.
  @retval EFI_TIMEOUT  SW_RST bit did not clear within SDCC_HC_GENERIC_TIMEOUT_US.
**/
STATIC
EFI_STATUS
SdccHcSoftwareReset (
  IN UINTN   Base,
  IN UINT16  ErrIntStatus
  )
{
  UINT8  SwRst;

  SwRst = 0;

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

/**
  Check whether the HC is ready to accept a new command (CMD and DAT inhibit).

  Called at the start of SdccExecTrb to avoid issuing a command while the bus
  is still busy from a previous transfer.  If inhibit does not clear within
  SDCC_HC_GENERIC_TIMEOUT_US, the TRB is failed with EFI_NOT_READY.

  @param[in]  Private  Per-instance private data.
  @param[in]  Trb      TRB to be submitted (used for command index in debug output).

  @retval EFI_SUCCESS    HC is ready; command may be issued.
  @retval EFI_NOT_READY  CMD or DAT inhibit did not clear in time.
**/
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

  // CMD7 with Argument=0 is a broadcast deselect per SD spec §4.7.4: no card
  // responds.  SdDxe sends it as SdMmcCommandTypeAc with ResponseType=R1
  // (ZeroMem default), which would set response-expected bits and wait for a
  // response that never comes — causing a timeout and leaving CMD_INHIBIT_CMD
  // set.  Fire it with no response bits (same path as Bc / CMD0) so SDHCI
  // asserts CMD_COMPLETE immediately and clears CMD_INHIBIT cleanly.
  if ((Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeBc) ||
      ((Packet->SdMmcCmdBlk->CommandIndex == 7) &&
       (Packet->SdMmcCmdBlk->CommandArgument == 0)))
  {
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

    // Bc (no-response) commands always time out — that is expected.  Do NOT
    // call SdccHcSoftwareReset: SW_RST CMD_LINE (BIT1) is EL2-blocked on
    // Sdc2 (and Sdc1), so the write is silently ignored and CMD_INHIBIT stays
    // latched, blocking every subsequent command.  Instead, clear the interrupt
    // status bits (W1C) and treat the timeout as success.
    //
    // CMD7 with CommandArgument=0 (deselect-all, RCA=0) is a special case:
    // SD spec §4.7.4 says no card responds to CMD7(RCA=0) — it is a broadcast
    // deselect even though SdDxe classifies it as SdMmcCommandTypeAc.
    // Treat it identically to Bc: clear W1C bits, skip SW_RST, return success.
    if ((Trb->Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeBc) ||
        ((Trb->Packet->SdMmcCmdBlk->CommandIndex == 7) &&
         (Trb->Packet->SdMmcCmdBlk->CommandArgument == 0)))
    {
      SdccWrite16 (Base, SDCC_HC_ERR_INT_STS, ErrIntSts);
      SdccWrite16 (Base, SDCC_HC_NOR_INT_STS, NorIntSts);
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

  if (HasData) {
    //
    // Follow downstream sdcc_hc.c pattern: issue SW_RST_DAT_LINE after every
    // successful data transfer.  This drains any residual DAT state machine
    // state so the next data command (including a CMD18 on the next boot) is
    // not blocked by a stale DAT_INHIBIT.
    //
    SdccWrite8 (Base, SDCC_HC_SW_RST, SDCC_HC_RST_DAT_LINE);
    SdccWaitSet8 (Base, SDCC_HC_SW_RST,
                  SDCC_HC_RST_DAT_LINE, 0,
                  SDCC_HC_GENERIC_TIMEOUT_US);
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

  // SDHCI spec §2.2.9: wait for CMD_INHIBIT (and DAT_INHIBIT for R1b/data)
  // to clear before submitting.  Poll up to 50 ms in 1 µs steps — after
  // CMD0 the host controller may hold CMD_INHIBIT briefly while the card
  // settles; failing immediately would spam NOT_READY on every CMD1 retry.
  //
  // Exception: skip for SdMmcCommandTypeBc (broadcast, no response).  CMD0
  // is Bc and leaves CMD_INHIBIT set when SW_RST is privilege-blocked; the
  // host controller accepts the COMMAND register write regardless (SdccExecTrb
  // uses a goto-FireCommand path for Bc).  Checking CMD_INHIBIT here would
  // block every subsequent Bcr command (CMD1 poll) indefinitely.
  if (CmdBlk.CommandType != SdMmcCommandTypeBc) {
    UINTN  InhibitWait;
    for (InhibitWait = 50000; InhibitWait > 0; InhibitWait--) {
      Rs = SdccCheckTrbEnv (Private, Trb);
      if (!EFI_ERROR (Rs)) {
        break;
      }
      MicroSecondDelay (1);
    }
    if (EFI_ERROR (Rs)) {
      DEBUG ((DEBUG_WARN, "[SdccHci] CMD%d: CMD_INHIBIT still set after 50 ms — skipping\n",
              CmdBlk.CommandIndex));
      SdccFreeTrb (Trb);
      return EFI_NOT_READY;
    }
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
