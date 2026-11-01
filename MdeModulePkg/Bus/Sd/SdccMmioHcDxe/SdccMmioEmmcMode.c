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
    UINT32  MaxWait = 3000000u;
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

  Calls SdccPlatformSetClockFreqHz to reprogram the GCC RCG to AppsClkHz,
  then programs the SDHCI CLK_CTRL divisor to produce TargetKHz from that
  base.  SdccHcClockSupply takes both values in kHz.

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

  Status = SdccPlatformSetClockFreqHz (&Private->Config, AppsClkHz);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return SdccHcClockSupply (
           (UINTN)Private->Config.BaseAddress,
           Private->ControllerVersion,
           AppsClkHz / 1000u,
           TargetKHz
           );
}

// ─── DLL init (HS400 / HS400-ES) ──────────────────────────────────────────────

/**
  Initialise the Qualcomm SDCC5 DLL.

  14-step sequence from HPG §26.1.  All tuning parameters come from
  Private->Config.Dll, populated by SdccGetPlatformConfig from FixedPCDs.

  Steps 1-9: power-cycle and enable the DLL.
  Steps 10-14: set rclk_delay, CMD_IN_RCLK, DDR_CAL_EN, then poll lock.
**/
STATIC
EFI_STATUS
SdccDllInit (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  UINTN              Base;
  SDCC5_DLL_CONFIG  *D;
  UINT32             DllCfg;
  UINT32             DllCfg2;

  Base = (UINTN)Private->Config.BaseAddress;
  D    = &Private->Config.Dll;

  // Step 1: disable DLL output clock (CK_OUT_EN = BIT18)
  MmioAnd32 (Base + SDCC_HC_VS_DLL_CONFIG, (UINT32)~BIT18);

  // Step 2: disable DLL input clock (DLL_CLOCK_DISABLE = BIT21 of DLL_CONFIG_2)
  MmioOr32  (Base + SDCC_HC_VS_DLL_CONFIG2, BIT21);

  // Step 3: assert DLL_RST (BIT30) + PDN (BIT29)
  MmioOr32  (Base + SDCC_HC_VS_DLL_CONFIG, BIT30 | BIT29);

  // Step 4: write DLL_CONFIG_3 (platform DLL setting)
  MmioWrite32 (Base + SDCC_HC_VS_DLL_CONFIG3, D->DllConfig3);

  // Step 5: write DLL_USER_CTL (platform DLL setting)
  MmioWrite32 (Base + SDCC_HC_VS_DLL_USER_CTL, D->DllUserCtl);

  // Step 5b: DLL_CONFIG lower 16 bits (DllConfigLower), MCLK_FREQ[26:24] from DllConfig2
  DllCfg = MmioRead32 (Base + SDCC_HC_VS_DLL_CONFIG);
  DllCfg = (DllCfg & 0xF8FF0000u)
          | (D->DllConfig2    & 0x07000000u)   // MCLK_FREQ field
          | (D->DllConfigLower & 0x0000FFFFu);
  MmioWrite32 (Base + SDCC_HC_VS_DLL_CONFIG, DllCfg);

  // Step 5c: DLL_CONFIG_2 MclkFreqCalc[17:10]; keep DLL_CLOCK_DISABLE; DDR_CAL_EN=0
  DllCfg2  = MmioRead32 (Base + SDCC_HC_VS_DLL_CONFIG2);
  DllCfg2 &= ~(0xFFu << 10);
  DllCfg2 |=  (D->MclkFreqCalc & 0xFFu) << 10;
  DllCfg2 &= (UINT32)~BIT0;   // DDR_CAL_EN — enabled in step 12
  DllCfg2 |= BIT21;            // DLL_CLOCK_DISABLE still asserted
  MmioWrite32 (Base + SDCC_HC_VS_DLL_CONFIG2, DllCfg2);

  // Step 6: re-enable DLL input clock
  MmioAnd32 (Base + SDCC_HC_VS_DLL_CONFIG2, (UINT32)~BIT21);

  // Step 7: wait 5 µs for DLL input clock to settle
  MicroSecondDelay (5);

  // Step 8: enable DLL (BIT16) and output clock (BIT18), RST + PDN still asserted
  DllCfg  = MmioRead32 (Base + SDCC_HC_VS_DLL_CONFIG);
  DllCfg |= BIT18 | BIT16;
  MmioWrite32 (Base + SDCC_HC_VS_DLL_CONFIG, DllCfg);

  // Step 9: wait 52 µs
  MicroSecondDelay (52);

  // Step 10: deassert RST + PDN
  MmioAnd32 (Base + SDCC_HC_VS_DLL_CONFIG, (UINT32)~(BIT30 | BIT29));

  // Step 11: toggle RST (assert → 1 µs → deassert)
  MmioOr32  (Base + SDCC_HC_VS_DLL_CONFIG, BIT30);
  MicroSecondDelay (1);
  MmioAnd32 (Base + SDCC_HC_VS_DLL_CONFIG, (UINT32)~BIT30);

  // Step 12: DDR_CONFIG rclk_delay bits[8:0] (platform value from PCD)
  {
    UINT32  DdrCfg = MmioRead32 (Base + SDCC_HC_VS_DDR_CONFIG);
    DdrCfg = (DdrCfg & ~0x1FFu) | (D->DdrRclkDelay & 0x1FFu);
    MmioWrite32 (Base + SDCC_HC_VS_DDR_CONFIG, DdrCfg);
  }

  // Step 13: CMD_IN_RCLK_EN (VENDOR_DDR200_CFG bit1)
  MmioOr32 (Base + SDCC_HC_VS_DDR200_CFG, BIT1);

  // Step 14: enable DDR calibration (DLL_CONFIG_2 bit0) then poll DDR_DLL_LOCK_JDR
  MmioOr32 (Base + SDCC_HC_VS_DLL_CONFIG2, BIT0);

  {
    UINT32  Waited  = 0;
    UINT32  DllSts;
    while (Waited < 50000u) {
      if (MmioRead32 (Base + SDCC_HC_VS_DLL_STATUS) & BIT11) {
        break;
      }
      MicroSecondDelay (1);
      Waited++;
    }
    DllSts = MmioRead32 (Base + SDCC_HC_VS_DLL_STATUS);
    DEBUG ((DEBUG_INFO, "[SdccEmmc] DLL_STATUS=0x%08x DDR_DLL_LOCK_JDR=%d\n",
            DllSts, !!(DllSts & BIT11)));
    if (!(DllSts & BIT11)) {
      DEBUG ((DEBUG_ERROR, "[SdccEmmc] DLL lock timeout after 50 ms\n"));
      return EFI_TIMEOUT;
    }
  }
  return EFI_SUCCESS;
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

  // HC_SELECT_IN=6 (DDR200/HS400), HC_SELECT_IN_EN=1
  MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~(BIT18 | (0x7u << 19)));
  MmioOr32  (Base + SDCC_HC_VS_FUNC, ((UINT32)6 << 19) | BIT18);

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

  // DLL power save off (PWRSAVE bit1 = 0)
  MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~BIT1);

  // Full DLL init (HPG §26.1, 14 steps)
  Status = SdccDllInit (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] %a: DLL init failed: %r\n", ModeName, Status));
    return Status;
  }

  // EXT_CSD re-read at HS400 operating frequency as hardware verification.
  Status = SdccReadExtCsd (Private, VerifyBuf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "[SdccEmmc] %a: EXT_CSD verification failed at %u kHz DDR: %r\n"
            "  VFUNC=0x%08x HCTRL1=0x%02x CLK=0x%04x "
            "DLL_STS=0x%08x DLL_CFG=0x%08x\n",
            ModeName,
            Private->Config.ClkHz_HS400 / 2000u,
            Status,
            MmioRead32 (Base + SDCC_HC_VS_FUNC),
            MmioRead8  (Base + SDCC_HC_HOST_CTRL1),
            MmioRead16 (Base + SDCC_HC_CLOCK_CTRL),
            MmioRead32 (Base + SDCC_HC_VS_DLL_STATUS),
            MmioRead32 (Base + SDCC_HC_VS_DLL_CONFIG)));
    return Status;
  }

  DEBUG ((DEBUG_INFO,
          "[SdccEmmc] %a verified: EXT_CSD read OK at %u kHz DDR "
          "VFUNC=0x%08x CLK=0x%04x\n",
          ModeName,
          Private->Config.ClkHz_HS400 / 2000u,
          MmioRead32 (Base + SDCC_HC_VS_FUNC),
          MmioRead16 (Base + SDCC_HC_CLOCK_CTRL)));

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
  UINTN       Base;

  Base = (UINTN)Private->Config.BaseAddress;

  // Step 1: HS_TIMING=HS200 (prerequisite for DDR mode switch)
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS200);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES: HS_TIMING=HS200 failed: %r\n", Status));
    return Status;
  }

  // Step 2: Lower HC clock to ~24 MHz for SDR12 transition; HC_SELECT_IN=2 (FEEDBACK)
  // Keep apps_clk at ClkHz_HS400 — only SDHCI divisor changes for this intermediate step.
  MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~(BIT18 | (0x7u << 19)));
  MmioOr32  (Base + SDCC_HC_VS_FUNC, ((UINT32)2 << 19) | BIT18);
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
  UINTN       Base;

  Base = (UINTN)Private->Config.BaseAddress;

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS200);
  if (EFI_ERROR (Status)) { return Status; }

  MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~(BIT18 | (0x7u << 19)));
  MmioOr32  (Base + SDCC_HC_VS_FUNC, ((UINT32)2 << 19) | BIT18);
  // Keep apps_clk at ClkHz_HS400 — only SDHCI divisor changes for this intermediate step.
  Status = SdccSetClockKHz (Private, Private->Config.ClkHz_HS400, 26000u);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400: 26 MHz clock step failed: %r\n", Status));
    return Status;
  }

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS);
  if (EFI_ERROR (Status)) { return Status; }

  // BUS_WIDTH=DDR 8-bit (no Enhanced Strobe — EXT_CSD[183]=0x06)
  Status = SdccEmmcSwitch (Private, EMMC_CMD6_WRITE_BYTE (183, 0x06u));
  if (EFI_ERROR (Status)) { return Status; }

  Status = SdccEmmcSwitch (Private, EMMC_CMD6_HS_TIMING_HS400);
  if (EFI_ERROR (Status)) { return Status; }

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

  // HC_SELECT_IN=2 (FEEDBACK) for HS200 SDR
  MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~(BIT18 | (0x7u << 19)));
  MmioOr32  (Base + SDCC_HC_VS_FUNC, ((UINT32)2 << 19) | BIT18);

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
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400-ES failed (%r) — falling back to HS400\n",
              Status));
      /* fall through */

    case SdccModeHS400:
      Status = SdccEmmcEnterHS400 (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS400 failed (%r) — falling back to HS200\n",
              Status));
      /* fall through */

    case SdccModeHS200:
      Status = SdccEmmcEnterHS200 (Private);
      if (!EFI_ERROR (Status)) {
        break;
      }
      DEBUG ((DEBUG_WARN, "[SdccEmmc] HS200 failed (%r) — falling back to HS\n",
              Status));
      /* fall through */

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
