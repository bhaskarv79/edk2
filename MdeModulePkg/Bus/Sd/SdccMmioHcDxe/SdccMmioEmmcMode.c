/** @file
  eMMC bus speed mode negotiation for SdccMmioHcDxe.

  Reads EXT_CSD at 400 kHz/1-bit (CMD8 via ADMA2), then selects the highest
  bus speed mode supported by both the device (EXT_CSD DEVICE_TYPE[196] +
  STROBE_SUPPORT[184]) and the platform (SDCC5_PLATFORM_CONFIG.MaxSpeedMode).

  Mode selection priority: HS400-ES > HS400 > HS200 > High Speed > Legacy.

  HS400-ES sequence follows sdcc_enter_hs400_mode_with_es() from the Kodiak
  downstream driver (sdcc_mmc_util.c).  Verification uses EXT_CSD re-read at
  operating frequency instead of CMD13 — downstream never issues CMD13 at
  HS400-ES; the first command is always a data command (EXT_CSD).

  Clock divisor computation:
    SdccSetClockKHz takes (AppsClkHz, TargetKHz): it first calls
    SdccPlatformSetClockFreqHz to move apps_clk to AppsClkHz, then calls
    SdccHcClockSupply with base=AppsClkHz/1000 to find the smallest SDHCI
    divisor N such that apps_clk/(2*N) ≤ TargetKHz.  HS400/HS400-ES use N=1
    (DDR — divide-by-2 is intentional); HS200 and HS use N=0 (bypass).

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

// EXT_CSD DEVICE_TYPE[196] mode bits (JEDEC JESD84-B51 Table 137)
#define EXTCSD_DT_HS52         BIT1   ///< HS 52 MHz
#define EXTCSD_DT_HS200_18V    BIT2   ///< HS200 1.8V SDR
#define EXTCSD_DT_HS400_18V    BIT4   ///< HS400 1.8V DDR
#define EXTCSD_DT_HS400ES_18V  BIT6   ///< HS400 Enhanced Strobe

// Device Status (R1/CMD6 response) bit (JEDEC JESD84-B51 Table "Device Status").
// Set when the device rejects a SWITCH command internally even though the
// host-level CMD6 transaction completed without a CRC/timeout error.
#define EMMC_CARD_STATUS_SWITCH_ERROR  BIT7

// ─── Internal helpers ─────────────────────────────────────────────────────────

/**
  Issue CMD6 SWITCH (R1b) and wait up to 3 seconds for completion.

  @param[in]  Private   Driver instance.
  @param[in]  Argument  CMD6 Access/Index/Value/CmdSet packed argument
                        (use EMMC_CMD6_* macros from SdccMmioHcDxe.h).

  @retval EFI_SUCCESS         Switch accepted and device ready.
  @retval EFI_TIMEOUT         R1b busy did not clear within 3 seconds.
  @retval EFI_DEVICE_ERROR    Error status bit set in R1 response, or the
                              device reported SWITCH_ERROR (card status
                              bit 7) -- CMD6 completed at the transport
                              level but the device rejected the switch
                              and EXT_CSD was not updated.
**/
STATIC
EFI_STATUS
SdccEmmcSwitch (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN UINT32                 Argument
  )
{
  EFI_SD_MMC_STATUS_BLOCK  StsBlk;
  EFI_STATUS               Status;

  Status = SdccEmmcCmd (Private, 6,
                        SdMmcCommandTypeAc, SdMmcResponseTypeR1b,
                        Argument, NULL, 0, &StsBlk, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // A clean host-level CMD6 completion is not evidence the switch actually
  // took effect -- the device can ACK the command while rejecting the
  // switch internally.  Treat SWITCH_ERROR as a hard failure so the caller
  // doesn't go on to reprogram host clock/DLL/HC_SELECT_IN for a mode the
  // device never entered.
  //
  if ((StsBlk.Resp0 & EMMC_CARD_STATUS_SWITCH_ERROR) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "[SdccEmmcMode] CMD6 Arg=0x%08x: SWITCH_ERROR (CardStatus=0x%08x)\n",
      Argument,
      StsBlk.Resp0
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

// ─── EXT_CSD read ─────────────────────────────────────────────────────────────

/**
  Read the full 512-byte EXT_CSD from the eMMC device via CMD8 + ADMA2.

  Can be called at any bus frequency; used both at 400 kHz/1-bit for capability
  detection and at HS400/HS400-ES operating frequency for post-switch verification.
**/
EFI_STATUS
SdccReadExtCsd (
  IN  SDCC_HC_PRIVATE_DATA  *Private,
  OUT UINT8                  ExtCsdBuf[EMMC_EXT_CSD_SIZE]
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             CmdBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Pkt;
  EFI_SD_MMC_STATUS_BLOCK              StsBlk;
  SDCC_HC_TRB                         *Trb;
  EFI_STATUS                           Status;

  ZeroMem (&CmdBlk,   sizeof CmdBlk);
  ZeroMem (&StsBlk,   sizeof StsBlk);
  ZeroMem (&Pkt,      sizeof Pkt);
  ZeroMem (ExtCsdBuf, EMMC_EXT_CSD_SIZE);

  CmdBlk.CommandIndex    = 8;
  CmdBlk.CommandType     = SdMmcCommandTypeAdtc;
  CmdBlk.ResponseType    = SdMmcResponseTypeR1;
  CmdBlk.CommandArgument = 0;

  Pkt.SdMmcCmdBlk      = &CmdBlk;
  Pkt.SdMmcStatusBlk   = &StsBlk;
  Pkt.Timeout          = 0;
  Pkt.InDataBuffer     = ExtCsdBuf;
  Pkt.InTransferLength = EMMC_EXT_CSD_SIZE;

  Trb = SdccCreateTrb (Private, 0, &Pkt, NULL);
  if (Trb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Trb->BlockSize = EMMC_EXT_CSD_SIZE;

  Status = SdccExecTrb (Private, Trb);
  if (!EFI_ERROR (Status)) {
    UINT32  MaxWait;

    MaxWait = 3000000u;
    while (MaxWait-- > 0) {
      Status = SdccCheckTrbResult (Private, Trb);
      if (Status != EFI_NOT_READY) {
        break;
      }
      MicroSecondDelay (1);
    }
    if (Status == EFI_NOT_READY) {
      Status = EFI_TIMEOUT;
    }
  }

  SdccFreeTrb (Trb);

  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO,
            "[SdccEmmc] EXT_CSD: Rev=%d DevType=0x%02x StrobeSupport=0x%02x "
            "HsTiming=0x%02x BusWidth=0x%02x PartCfg=0x%02x\n",
            ExtCsdBuf[EMMC_EXT_CSD_REV],
            ExtCsdBuf[EMMC_EXT_CSD_DEVICE_TYPE],
            ExtCsdBuf[EMMC_EXT_CSD_STROBE_SUPPORT],
            ExtCsdBuf[EMMC_EXT_CSD_HS_TIMING],
            ExtCsdBuf[EMMC_EXT_CSD_BUS_WIDTH],
            ExtCsdBuf[EMMC_EXT_CSD_PARTITION_CFG]));
  }

  return Status;
}

// ─── SDHCI clock supply helper ────────────────────────────────────────────────

/**
  Switch apps_clk to the given frequency and update the SDHCI clock divisor.

  Per SDHCI spec §2.2.14, gates SDCLK first (SdccHcStopClock), then
  reprograms the GCC RCG via SdccPlatformSetClockFreqHz, then re-enables
  SDCLK at the new divisor via SdccHcClockSupply.

  @param[in]  Private      Driver instance.
  @param[in]  AppsClkHz    New apps_clk in Hz (e.g. Config.ClkHz_HS400).
                           Must be one of the frequencies in the clock driver
                           frequency table or SetClockFreqHz will return
                           EFI_NOT_FOUND and the clock rate will not change.
  @param[in]  TargetKHz    Desired SDCLK in kHz.  SdccHcClockSupply picks the
                           smallest SDHCI divisor N ≥ 1 such that
                           (AppsClkHz/1000)/(2*N) ≤ TargetKHz.
                           Pass AppsClkHz/2000 for HS400/HS400-ES (DDR, N=1);
                           pass AppsClkHz/1000 for HS200/HS (bypass, N=0).

  @retval EFI_SUCCESS         Clock switch completed.
  @retval EFI_NOT_FOUND       AppsClkHz not in the platform frequency table.
  @retval EFI_TIMEOUT         SDHCI clock stable bit did not set.
**/
STATIC
EFI_STATUS
SdccSetClockKHz (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN UINT32                 AppsClkHz,
  IN UINT32                 TargetKHz
  )
{
  EFI_STATUS  Status;

  // SDHCI spec §2.2.14: gate SDCLK before switching the base clock source.
  Status = SdccHcStopClock ((UINTN)Private->Config.BaseAddress);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SdccPlatformSetClockFreqHz (&Private->Config, AppsClkHz);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->CurrentAppsClkHz = AppsClkHz;

  return SdccHcClockSupply (
           (UINTN)Private->Config.BaseAddress,
           Private->ControllerVersion,
           AppsClkHz / 1000u,
           TargetKHz
           );
}

/**
  Restore the HC to a safe identification-speed clock state before falling
  back to a lower bus speed mode.

  SdccEmmcEnterHS400ES()/SdccEmmcEnterHS400()/SdccEmmcEnterHS200() each ramp
  apps_clk and SDCLK up (and, for the HS400 tiers, engage the vendor DLL and
  clock-source mux via SdccPlatformSelectClockSource/SdccPlatformDllInit)
  before verifying the switch succeeded. If one of those functions fails
  after the clock/DLL/mux state has already been changed,
  SdccNegotiateEmmcMode()'s fallback immediately calls the next lower mode's
  entry function -- which assumes it is starting from the same low/safe
  clock SdccNegotiateEmmcMode began at, not from the failed attempt's
  leftover high-speed state. Without this restore, the next tier's first
  CMD6 would be issued at the wrong clock/timing.

  Clears the vendor clock-source mux (SdccPlatformSelectClockSource with
  SdccClockSrcNone) and drops SDCLK back to SDCC_HC_IDENTIFY_CLOCK_KHZ via
  divisor only -- apps_clk itself (Private->CurrentAppsClkHz) is left as-is;
  the next tier's own entry function reprograms it as needed. There is no
  platform-lib entry point to explicitly disable the vendor DLL itself;
  clearing the clock-source mux disconnects the DLL's sampling-clock
  reference, which is the dominant factor in whether a wrongly-locked DLL
  from the failed attempt could affect the next tier's commands.
**/
STATIC
VOID
SdccEmmcRestoreSafeClock (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;

  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcNone);

  Status = SdccHcClockSupply (
             (UINTN)Private->Config.BaseAddress,
             Private->ControllerVersion,
             Private->CurrentAppsClkHz / 1000u,
             SDCC_HC_IDENTIFY_CLOCK_KHZ
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "[SdccEmmc] RestoreSafeClock: clock supply failed: %r\n", Status));
  }
}

// ─── Mode entry sequences ─────────────────────────────────────────────────────

/**
  Configure HC for HS400/HS400-ES final phase (shared by both entry sequences).

  Performs the HC-side steps that are identical whether Enhanced Strobe is
  active or not:
    - HC_SELECT_IN=6 (DDR200/HS400), HC_SELECT_IN_EN=1
    - apps_clk → ClkHz_HS400, SDHCI divisor N=1 → SDCLK = ClkHz_HS400/2 (DDR)
    - HOST_CTRL1: 8-bit + HighSpeed
    - DLL power save off
    - Full DLL init (14-step HPG §26.1)
    - EXT_CSD re-read at operating frequency as hardware verification

  Called after the device has acknowledged HS_TIMING=HS400 via CMD6.

  @param[in]  Private   Driver instance (ClkHz_HS400 and Dll fields used).
  @param[in]  ModeName  ASCII string for debug messages ("HS400-ES" or "HS400").

  @retval EFI_SUCCESS        All steps completed; DLL locked; EXT_CSD readable.
  @retval EFI_TIMEOUT        DLL lock or EXT_CSD timed out.
  @retval EFI_DEVICE_ERROR   EXT_CSD read returned device error.
**/
STATIC
EFI_STATUS
SdccHcEnterHS400Final (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN CONST CHAR8           *ModeName
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      VerifyBuf32[EMMC_EXT_CSD_SIZE / sizeof (UINT32)];
  UINT8      *VerifyBuf = (UINT8 *)VerifyBuf32;  // UINT32 array → 4-byte aligned (IoAlign=4)

  Base = (UINTN)Private->Config.BaseAddress;

  // DDR200/HS400 clock/strobe source for the vendor DLL.
  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcDdr200Hs400);

  // apps_clk → ClkHz_HS400. SDHCI divisor N=1 → SDCLK = ClkHz_HS400/2 (DDR).
  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS400,
                             Private->Config.ClkHz_HS400 / 2000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] %a: clock supply failed: %r\n", ModeName, Status));
    return Status;
  }
  DEBUG ((DEBUG_INFO, "[SdccEmmc] %a: clock set (%u kHz target): CLK=0x%04x\n",
          ModeName, Private->Config.ClkHz_HS400 / 2000u,
          MmioRead16 (Base + SDCC_HC_CLOCK_CTRL)));

  // HOST_CTRL1 — 8-bit + HighSpeed
  MmioOr8 (Base + SDCC_HC_HOST_CTRL1, BIT5 | BIT2);

  // Full vendor DLL init (HPG §26.1) — platform-specific, see SdccPlatformLib.
  Status = SdccPlatformDllInit (&Private->Config, SdccDllModeHs400);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] %a: DLL init failed: %r\n", ModeName, Status));
    return Status;
  }

  // EXT_CSD re-read at HS400 operating frequency as hardware verification.
  Status = SdccReadExtCsd (Private, VerifyBuf);
  if (EFI_ERROR (Status)) {
    SDCC5_DLL_DIAG  Diag;

    SdccPlatformGetDllDiag (&Private->Config, &Diag);
    DEBUG ((DEBUG_WARN,
            "[SdccEmmc] %a: EXT_CSD verification failed at %u kHz DDR: %r\n"
            "  VFUNC=0x%08x HCTRL1=0x%02x CLK=0x%04x "
            "DLL_STS=0x%08x DLL_CFG=0x%08x\n",
            ModeName,
            Private->Config.ClkHz_HS400 / 2000u,
            Status,
            Diag.VFunc,
            MmioRead8  (Base + SDCC_HC_HOST_CTRL1),
            MmioRead16 (Base + SDCC_HC_CLOCK_CTRL),
            Diag.DllStatus,
            Diag.DllConfig));
    return Status;
  }

  {
    SDCC5_DLL_DIAG  Diag;

    SdccPlatformGetDllDiag (&Private->Config, &Diag);
    DEBUG ((DEBUG_INFO,
            "[SdccEmmc] %a verified: EXT_CSD read OK at %u kHz DDR "
            "VFUNC=0x%08x CLK=0x%04x\n",
            ModeName,
            Private->Config.ClkHz_HS400 / 2000u,
            Diag.VFunc,
            MmioRead16 (Base + SDCC_HC_CLOCK_CTRL)));
  }

  return EFI_SUCCESS;
}

/**
  Switch device and HC to HS400 Enhanced Strobe.

  Sequence (from downstream sdcc_enter_hs400_mode_with_es):
    1. CMD6: HS_TIMING=HS200
    2. HC clock to 26 MHz (SDR12 transition), HC_SELECT_IN=2 (FEEDBACK)
    3. CMD6: HS_TIMING=HS
    4. CMD6: BUS_WIDTH=DDR 8-bit + Enhanced Strobe (EXT_CSD[183]=0x86)
    5. CMD6: HS_TIMING=HS400
    6. HC_SELECT_IN=6 (DDR200/HS400)
    7. HC clock to ClkHz_HS400/2 (192 MHz for Kodiak)
    8. HOST_CTRL1: 8-bit + HighSpeed bits
    9. DLL power save off
    10-14. DLL init (HPG §26.1 steps)
    15. EXT_CSD re-read at operating frequency as verification

  Returns error if any CMD6 fails or EXT_CSD verification fails.
  Caller falls back to HS400 or lower mode on failure.
**/
STATIC
EFI_STATUS
SdccEmmcEnterHS400ES (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;

  // Step 1: HS_TIMING=HS200 (prerequisite for DDR mode switch)
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS200);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: HS_TIMING=HS200 failed: %r\n", Status));
    return Status;
  }

  // Step 2: Lower HC clock to ~24 MHz for SDR12 transition; FEEDBACK clock source.
  // Keep apps_clk at ClkHz_HS400 — only SDHCI divisor changes for this intermediate step.
  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcFeedback);
  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS400, 26000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: 26 MHz clock step failed: %r\n", Status));
    return Status;
  }

  // Step 3: HS_TIMING=HS
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: HS_TIMING=HS failed: %r\n", Status));
    return Status;
  }

  // Step 4: BUS_WIDTH=DDR 8-bit + Enhanced Strobe (EXT_CSD[183]=0x86)
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_BUS_WIDTH_DDR8ES);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: BUS_WIDTH=DDR8+ES failed: %r\n", Status));
    return Status;
  }

  // Step 5: HS_TIMING=HS400
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS400);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: HS_TIMING=HS400 failed: %r\n", Status));
    return Status;
  }

  // Steps 6-15: shared HS400 final-phase sequence (HC_SELECT_IN, clock, DLL, verify)
  return SdccHcEnterHS400Final (Private, "HS400-ES");
}

/**
  Switch device and HC to HS400 (no Enhanced Strobe).

  Same sequence as HS400-ES but BUS_WIDTH=DDR 8-bit (no strobe bit).
  EXT_CSD re-read at operating frequency is used for verification.
**/
STATIC
EFI_STATUS
SdccEmmcEnterHS400 (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS200);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccEmmc] HS400: CMD6 HS_TIMING=HS200 failed: %r\n", Status));
    return Status;
  }

  // FEEDBACK clock source for the SDR12 clock-lowering intermediate step.
  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcFeedback);
  // Keep apps_clk at ClkHz_HS400 — only SDHCI divisor changes for this intermediate step.
  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS400, 26000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400: 26 MHz clock step failed: %r\n", Status));
    return Status;
  }

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccEmmc] HS400: CMD6 HS_TIMING=HS failed: %r\n", Status));
    return Status;
  }

  // BUS_WIDTH=DDR 8-bit (no Enhanced Strobe — EXT_CSD[183]=0x06)
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_WRITE_BYTE (183, 0x06u));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccEmmc] HS400: CMD6 BUS_WIDTH=DDR8 failed: %r\n", Status));
    return Status;
  }

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS400);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccEmmc] HS400: CMD6 HS_TIMING=HS400 failed: %r\n", Status));
    return Status;
  }

  // Steps 6-15: shared HS400 final-phase sequence (HC_SELECT_IN, clock, DLL, verify)
  return SdccHcEnterHS400Final (Private, "HS400");
}

/**
  Switch device and HC to HS200 (SDR 8-bit).

  Target SDCLK = ClkHz_HS200/2 using the same apps_clk base as HS400.
**/
STATIC
EFI_STATUS
SdccEmmcEnterHS200 (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINTN       Base;

  Base = (UINTN)Private->Config.BaseAddress;

  // 8-bit SDR bus width
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_BUS_WIDTH_8BIT);
  if (EFI_ERROR (Status)) { return Status; }
  MmioOr8 (Base + SDCC_HC_HOST_CTRL1, BIT5);

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS200);
  if (EFI_ERROR (Status)) { return Status; }

  // UHS_MODE_SEL=SDR104 (value 3) — SDHCI spec Table 2-32 for HS200 SDR mode.
  // Clear bits[2:0] first to avoid a reserved state if bit[2] was set previously.
  MmioAnd16 (Base + SDCC_HC_HOST_CTRL2, (UINT16)~SDCC_HC_CTRL2_UHS_MASK);
  MmioOr16  (Base + SDCC_HC_HOST_CTRL2, 0x0003u);

  // FEEDBACK clock source for HS200 SDR mode.
  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcFeedback);

  // apps_clk → ClkHz_HS200 (bypass mode, N=0) → SDCLK = ClkHz_HS200.
  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS200,
                             Private->Config.ClkHz_HS200 / 1000u);
  if (EFI_ERROR (Status)) { return Status; }

  DEBUG ((DEBUG_INFO, "[SdccEmmc] HS200: CLK=0x%04x\n",
          MmioRead16 (Base + SDCC_HC_CLOCK_CTRL)));

  return EFI_SUCCESS;
}

/**
  Switch device and HC to High Speed (52 MHz SDR 8-bit).

  apps_clk → ClkHz_HS, bypass (N=0) → SDCLK = ClkHz_HS exactly.
**/
STATIC
EFI_STATUS
SdccEmmcEnterHS (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINTN       Base;

  Base = (UINTN)Private->Config.BaseAddress;

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_BUS_WIDTH_8BIT);
  if (EFI_ERROR (Status)) { return Status; }
  MmioOr8 (Base + SDCC_HC_HOST_CTRL1, BIT5 | BIT2);   // 8-bit + HighSpeed

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS);
  if (EFI_ERROR (Status)) { return Status; }

  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS,
                             Private->Config.ClkHz_HS / 1000u);
  if (EFI_ERROR (Status)) { return Status; }

  DEBUG ((DEBUG_INFO, "[SdccEmmc] HS: CLK=0x%04x\n",
          MmioRead16 (Base + SDCC_HC_CLOCK_CTRL)));

  return EFI_SUCCESS;
}

// ─── Mode negotiation entry point ────────────────────────────────────────────

/**
  Negotiate the highest bus speed mode supported by device and platform.

  1. Reads EXT_CSD at 400 kHz/1-bit to discover device capabilities.
  2. Selects highest mode from: HS400-ES > HS400 > HS200 > HS > Legacy.
  3. Executes mode-entry sequence; on failure falls back to next lower mode.

  Always returns EFI_SUCCESS — the driver operates at whatever mode was
  achieved, and EmmcDxe will work at any speed.
**/
EFI_STATUS
SdccNegotiateEmmcMode (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS       Status;
  SDCC5_EMMC_MODE  PlatformMax;
  SDCC5_EMMC_MODE  SelectedMode;
  UINT8            DevType;
  UINT8            StrobeSupport;
  UINT32           ExtCsdBuf32[EMMC_EXT_CSD_SIZE / sizeof (UINT32)];
  UINT8           *ExtCsdBuf = (UINT8 *)ExtCsdBuf32;  // guaranteed UINT32-aligned (IoAlign=4)

  Status = SdccReadExtCsd (Private, ExtCsdBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] EXT_CSD read failed (%r) — staying at 400 kHz\n",
            Status));
    return EFI_SUCCESS;
  }

  DevType       = ExtCsdBuf[EMMC_EXT_CSD_DEVICE_TYPE];
  StrobeSupport = ExtCsdBuf[EMMC_EXT_CSD_STROBE_SUPPORT];
  PlatformMax   = Private->Config.MaxSpeedMode;

  DEBUG ((DEBUG_INFO,
          "[SdccEmmc] EXT_CSD DevType=0x%02x StrobeSupport=0x%02x PlatformMax=%d\n",
          DevType, StrobeSupport, (INT32)PlatformMax));

  // Select highest mode supported by both device and platform.
  if ((StrobeSupport & BIT0) && (DevType & EXTCSD_DT_HS400ES_18V) &&
      ((PlatformMax == SdccModeAuto) || (PlatformMax >= SdccModeHS400ES)))
  {
    SelectedMode = SdccModeHS400ES;
  } else if ((DevType & EXTCSD_DT_HS400_18V) &&
             ((PlatformMax == SdccModeAuto) || (PlatformMax >= SdccModeHS400)))
  {
    SelectedMode = SdccModeHS400;
  } else if ((DevType & EXTCSD_DT_HS200_18V) &&
             ((PlatformMax == SdccModeAuto) || (PlatformMax >= SdccModeHS200)))
  {
    SelectedMode = SdccModeHS200;
  } else if ((DevType & EXTCSD_DT_HS52) &&
             ((PlatformMax == SdccModeAuto) || (PlatformMax >= SdccModeHS)))
  {
    SelectedMode = SdccModeHS;
  } else {
    SelectedMode = SdccModeLegacy;
  }

  DEBUG ((DEBUG_INFO, "[SdccEmmc] Selected mode: %d\n", (INT32)SelectedMode));

  switch (SelectedMode) {
    case SdccModeHS400ES:
      Status = SdccEmmcEnterHS400ES (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES failed (%r) — falling back\n", Status));
      SdccEmmcRestoreSafeClock (Private);
      if (DevType & EXTCSD_DT_HS400_18V) {
        // Device supports HS400 — attempt it.
        /* fall through */
      } else {
        // Device only supports HS400ES (not HS400) — skip straight to HS200.
        DEBUG ((DEBUG_WARN, "[SdccEmmc] Device lacks HS400 support — falling back to HS200\n"));
        Status = SdccEmmcEnterHS200 (Private);
        if (!EFI_ERROR (Status)) {
          break;
        }
        DEBUG ((DEBUG_WARN, "[SdccEmmc] HS200 failed (%r) — falling back to HS\n", Status));
        SdccEmmcRestoreSafeClock (Private);
        goto TryHS;
      }
      /* fall through */

    case SdccModeHS400:
      Status = SdccEmmcEnterHS400 (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400 failed (%r) — falling back to HS200\n",
              Status));
      SdccEmmcRestoreSafeClock (Private);
      /* fall through */

    case SdccModeHS200:
      Status = SdccEmmcEnterHS200 (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS200 failed (%r) — falling back to HS\n",
              Status));
      SdccEmmcRestoreSafeClock (Private);
      /* fall through */

    TryHS:
    case SdccModeHS:
      Status = SdccEmmcEnterHS (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS failed (%r) — staying at 400 kHz\n",
              Status));
      /* fall through */

    default:
      Status = EFI_SUCCESS;   // Legacy/400 kHz — already configured
      break;
  }

  return EFI_SUCCESS;   // non-fatal: any achieved mode is usable
}
