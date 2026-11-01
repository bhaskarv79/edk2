/** @file
  SdccPlatformLib — platform-specific configuration interface for SdccMmioHcDxe.

  SdccMmioHcDxe (generic MMIO SDHCI HC driver) calls SdccGetPlatformConfig() once
  per controller instance at init time.  The platform implementation (e.g.
  QualcommPlatformPkg) returns a SDCC5_PLATFORM_CONFIG struct populated from
  platform PCDs, device tree, or hardware registers.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SDCC_PLATFORM_LIB_H__
#define __SDCC_PLATFORM_LIB_H__

#include <Uefi.h>

///
/// eMMC bus speed modes, in priority order (highest first).
/// 0xFF = auto-select from EXT_CSD DEVICE_TYPE + STROBE_SUPPORT.
///
typedef enum {
  SdccModeAuto     = 0xFF,   ///< Auto-detect from EXT_CSD (default)
  SdccModeHS400ES  = 4,      ///< HS400 Enhanced Strobe (200 MHz DDR 8-bit)
  SdccModeHS400    = 3,      ///< HS400 (200 MHz DDR 8-bit, DLL tuning)
  SdccModeHS200    = 2,      ///< HS200 (200 MHz SDR 8-bit, tuning)
  SdccModeHS       = 1,      ///< High Speed (52 MHz SDR)
  SdccModeLegacy   = 0,      ///< Identification speed (400 kHz, 1-bit)
} SDCC5_EMMC_MODE;

// SdccNegotiateEmmcMode uses >= comparisons across these values, so ordering
// must be strict.  SdccModeAuto (0xFF) is the out-of-band sentinel and must
// be highest; numeric modes must be ordered highest-first.
STATIC_ASSERT (
  (SdccModeAuto > SdccModeHS400ES) &&
  (SdccModeHS400ES > SdccModeHS400) &&
  (SdccModeHS400 > SdccModeHS200) &&
  (SdccModeHS200 > SdccModeHS) &&
  (SdccModeHS > SdccModeLegacy),
  "SDCC5_EMMC_MODE enum values are out of order"
  );

///
/// DLL tuning parameters — platform/SoC-specific values from HSR settings
/// (equivalent of sdcc.dtsi HsrSettings/HS400 node in Qualcomm downstream).
///
typedef struct {
  UINT32    DllConfigLower;   ///< VS_DLL_CONFIG bits[15:0]
  UINT32    DllConfig2;       ///< VS_DLL_CONFIG_2 base value (DDR_CAL_EN excluded)
  UINT32    DllConfig3;       ///< VS_DLL_CONFIG_3
  UINT32    DllUserCtl;       ///< VS_DLL_USER_CTL
  UINT32    MclkFreqCalc;     ///< DLL_CONFIG_2 MCLK_FREQ_CALC field value
  UINT32    DdrRclkDelay;     ///< DDR_CONFIG PRG_RCLK_DLY bits[8:0]
} SDCC5_DLL_CONFIG;

///
/// Selects which DLL lock-detection sequence SdccPlatformDllInit runs.
/// The vendor DLL hardware (HPG §26.1) shares most of its init sequence
/// between modes; these two differ in the DDR-path steps (rclk_delay,
/// CMD_IN_RCLK_EN, DDR_CAL_EN) and in which DLL_STATUS bit gates completion.
///
typedef enum {
  SdccDllModeSdr104 = 0,   ///< SD card UHS-I SDR104: DLL_LOCK (DLL_STATUS[7])
  SdccDllModeHs400  = 1,   ///< eMMC HS400/HS400-ES: DDR_DLL_LOCK_JDR (DLL_STATUS[11])
} SDCC5_DLL_MODE;

///
/// Selects the HC's card-interface clock/strobe source (Qualcomm SDCC5
/// vendor-specific HC_SELECT_IN mux).  Each UHS-I/high-speed mode transition
/// requires routing a different clock edge into the HC's sampling logic
/// before the mode's DLL (if any) is initialised.
///
typedef enum {
  SdccClockSrcFeedback    = 0,  ///< FEEDBACK clock: SDR12/HS200-SDR transitions
  SdccClockSrcDdr200Hs400 = 1,  ///< DDR200/HS400 clock: HS400 and HS400-ES
  SdccClockSrcUhsMclk     = 2,  ///< UHS-mode MCLK: SD card SDR104
  SdccClockSrcNone        = 3,  ///< No dedicated source; clears the mux enable
} SDCC5_CLOCK_SOURCE;

///
/// Platform configuration provided to SdccMmioHcDxe by SdccGetPlatformConfig().
/// All values are read-only after init.
///
typedef struct {
  ///
  /// MMIO base address of the SDHCI register window (e.g. 0x7C4000 for Sdc1).
  ///
  EFI_PHYSICAL_ADDRESS  BaseAddress;

  ///
  /// Slot index within this HC instance (always 0 for storage controllers).
  ///
  UINT8                 SlotIndex;

  ///
  /// Maximum eMMC bus speed mode the platform supports for this controller.
  /// 0xFF = let the driver negotiate from EXT_CSD device capabilities.
  ///
  SDCC5_EMMC_MODE       MaxSpeedMode;

  ///
  /// SDHCI base clock frequency in kHz as reported by CAPS register.
  /// On SDCC5 this reflects the apps_clk after any platform divider
  /// (e.g. 200000 = 200 MHz reported, but actual apps_clk = 384 MHz).
  ///
  UINT32                CapsBaseClkFreqKHz;

  ///
  /// Actual apps_clk frequency in Hz at which the platform runs the HC
  /// for each speed tier.  SdccMmioHcDxe calls SetClockFreqHz with these.
  ///
  UINT32                ClkHz_HS400;    ///< apps_clk for HS400/HS400-ES (e.g. 384000000)
  UINT32                ClkHz_HS200;    ///< apps_clk for HS200         (e.g. 192000000)
  UINT32                ClkHz_HS;       ///< apps_clk for High Speed     (e.g. 52000000)
  UINT32                ClkHz_Init;     ///< apps_clk for identification (e.g. 400000)

  ///
  /// apps_clk frequency in Hz for SD card UHS-I SDR104 mode.  Ignored for eMMC
  /// controllers (IsEmmc == TRUE).  Must be reprogrammed before SdccPlatformDllInit
  /// is called with SdccDllModeSdr104 — the DLL requires a live high-rate MCLK
  /// edge to lock; the identification-clock rate (ClkHz_Init) is not sufficient.
  ///
  UINT32                ClkHz_SDR104;   ///< apps_clk for SD UHS-I SDR104 (e.g. 202000000)

  ///
  /// DLL tuning parameters for HS400/HS400-ES.
  /// Ignored for HS200 and below.
  ///
  SDCC5_DLL_CONFIG      Dll;

  ///
  /// TRUE if this is an eMMC (embedded, non-removable) controller.
  /// FALSE = SD card (removable, hot-plug supported).
  ///
  BOOLEAN               IsEmmc;

  ///
  /// TRUE if the SD slot voltage is hardwired by the platform (e.g. via PMIC
  /// VREG) and the SDHCI CAP voltage-capability bits (CAP1[26:24]) are zero.
  /// When set, SdccHcColdInit writes POWER_CTRL=3.3V+ON directly, bypassing
  /// the standard SdccHcInitPower() CAP-based check.
  ///
  BOOLEAN               ForcePowerOn3P3V;

  ///
  /// TRUE if the Card Detect (SDCD#) pin is not physically wired on this
  /// platform.  When set, SdccHcCardDetect falls back to reading DAT[3:0]
  /// line signal level (PRESENT_STATE bits[23:20]) to infer card presence.
  /// The card's internal pull-ups keep DAT lines high when a card is seated.
  ///
  BOOLEAN               NoCdPin;

  ///
  /// Relative Card Address (RCA) to assign to the eMMC device via CMD3.
  /// Typically read from a platform PCD (e.g. PcdSdcc1EmmcRca).
  /// Ignored for SD card controllers (RCA is returned by the device in CMD3 R6).
  ///
  UINT16                EmmcRca;

  ///
  /// TRUE if SW_RST_ALL (full SDHCI software reset, HPG §7.3.8.3) must not be
  /// issued for this controller instance — e.g. because the platform's
  /// firmware/privilege configuration write-protects it from the exception
  /// level UEFI runs at, or because firmware already leaves the HC in a
  /// known-good state and a full reset would race the device.  When TRUE,
  /// SdccHcColdInit/SdccInitEmmc skip SdccHcReset() and instead unmask PWRCTL
  /// events and issue only SW_RST_DAT_LINE to clear stale DAT-state-machine
  /// residue from a prior session.  When FALSE, the full SdccHcReset()
  /// sequence runs as SDHCI spec-compliant platforms expect.
  ///
  BOOLEAN               SkipSwRstAll;

  ///
  /// TRUE if this controller instance's DMA path (AXI/bus fabric routing from
  /// the SDCC5 controller to system memory) cannot reach physical addresses at
  /// or above 4 GB, regardless of what the SDHCI CAP1[28]/CAP2[27] SysBus64
  /// capability bit advertises.  Some SoC integrations wire the HC's ADMA
  /// engine through a bus segment narrower than the SDHCI-reported capability;
  /// the capability bit alone is not a reliable indicator on such platforms.
  /// When TRUE, SdccDmaMap and SdccBuildAdmaDescTable restrict all DMA buffer
  /// and descriptor-table allocations to below 4 GB (bounce-buffering as
  /// needed).  When FALSE, allocations are unrestricted and rely on the
  /// SysBus64 capability bit already read into SDCC_SLOT_CAP.
  ///
  BOOLEAN               Dma32BitOnly;
} SDCC5_PLATFORM_CONFIG;

/**
  Return platform-specific SDCC5 HC configuration for the requested instance.

  Called once per controller at driver initialization.  The driver stores the
  result in its private data and uses it for all subsequent operations.

  @param[in]  InstanceIndex   Zero-based controller index (0 = eMMC, 1 = SD card).
  @param[out] Config          Caller-allocated struct to populate.

  @retval EFI_SUCCESS         Config was populated successfully.
  @retval EFI_NOT_FOUND       No controller exists at InstanceIndex.
  @retval EFI_INVALID_PARAMETER Config is NULL.
**/
EFI_STATUS
EFIAPI
SdccGetPlatformConfig (
  IN  UINTN                   InstanceIndex,
  OUT SDCC5_PLATFORM_CONFIG  *Config
  );

/**
  Perform any platform-specific initialization for the controller.

  Called after SdccGetPlatformConfig() returns Config, and before any
  SDHCI register access.  Typical actions: enable AHB + Apps clocks via
  EFI_CLOCK_PROTOCOL, configure TLMM GPIO drive-strength and pull settings.

  This function allows the platform shim to use Qualcomm-specific protocols
  and libraries without pulling those dependencies into the generic driver.

  @param[in]  InstanceIndex   Zero-based controller index (matches GetPlatformConfig).
  @param[in]  Config          Platform configuration returned by SdccGetPlatformConfig.

  @retval EFI_SUCCESS         Platform init succeeded (or was not needed).
  @retval Other               Non-fatal; driver will attempt to continue.
**/
EFI_STATUS
EFIAPI
SdccPlatformInit (
  IN UINTN                         InstanceIndex,
  IN CONST SDCC5_PLATFORM_CONFIG  *Config
  );

/**
  Request a change to the apps_clk frequency for the SDCC controller.

  Called by SdccMmioHcDxe before programming the SDHCI clock divisor when
  entering a new speed mode (HS400/HS400-ES, HS200, HS, or identification).
  The platform implementation calls EFI_CLOCK_PROTOCOL::SetClockFreqHz on
  the apps_clk branch for the corresponding controller instance.

  If the platform has no clock driver or the clock protocol is unavailable,
  this function returns EFI_SUCCESS silently — the SDHCI divisor calculation
  will use Config.ClkHz_HS400 as the base clock (the XBL-set rate), which
  is always correct for HS400/HS400-ES and functional for other modes.

  @param[in]  Config     Platform configuration for this controller instance.
                         The platform shim identifies the controller by the
                         Config.BaseAddress or by the InstanceIndex cached
                         during SdccPlatformInit.
  @param[in]  FreqHz     Target apps_clk frequency in Hz (e.g. ClkHz_HS400).

  @retval EFI_SUCCESS    Frequency was set, or clock protocol not available
                         (non-fatal — divisor uses Config.ClkHz_HS400 base).
  @retval EFI_TIMEOUT    RCG UPDATE bit did not clear (hardware failure).
**/
EFI_STATUS
EFIAPI
SdccPlatformSetClockFreqHz (
  IN CONST SDCC5_PLATFORM_CONFIG  *Config,
  IN UINT32                        FreqHz
  );

/**
  Initialise the Qualcomm SDCC5 DLL and poll for lock.

  Performs the full HPG §26.1 vendor DLL bring-up sequence — power-cycle,
  program DLL_CONFIG/DLL_CONFIG_2/DLL_CONFIG_3/DLL_USER_CTL from Config.Dll,
  re-enable, and (for SdccDllModeHs400 only) the additional DDR-path steps
  (rclk_delay, CMD_IN_RCLK_EN, DDR_CAL_EN) — then polls the mode-appropriate
  DLL_STATUS lock bit.  This sequence is entirely Qualcomm vendor-specific;
  none of the registers it touches exist in the SDHCI Simplified
  Specification.

  Must be called after the HC's card-interface clock source has been
  selected for the target mode (SdccPlatformSelectClockSource) and after
  apps_clk has been switched to the target frequency
  (SdccPlatformSetClockFreqHz) — the DLL cannot lock without a live clock
  edge at (or near) the operating rate.

  @param[in]  Config   Platform configuration for this controller instance;
                       Config.Dll supplies all tuning register values.
  @param[in]  Mode     Which DLL lock-detection sequence to run.

  @retval EFI_SUCCESS   DLL locked within 50 ms.
  @retval EFI_TIMEOUT   The mode-appropriate DLL_STATUS lock bit did not
                        assert within 50 ms.
**/
EFI_STATUS
EFIAPI
SdccPlatformDllInit (
  IN CONST SDCC5_PLATFORM_CONFIG  *Config,
  IN SDCC5_DLL_MODE                 Mode
  );

///
/// Vendor DLL/mux register snapshot for diagnostic logging on a failed DLL
/// lock or mode-verification read.  Field names match the vendor register
/// names (VFunc = VS_FUNC, DllStatus = VS_DLL_STATUS, DllConfig = VS_DLL_CONFIG)
/// so callers can log them without needing to know the underlying offsets.
///
typedef struct {
  UINT32    VFunc;
  UINT32    DllStatus;
  UINT32    DllConfig;
} SDCC5_DLL_DIAG;

/**
  Snapshot the vendor DLL/mux registers for diagnostic logging.

  Used by the generic driver when a DLL-dependent mode's post-switch
  verification (e.g. EXT_CSD re-read) fails or succeeds, to log the DLL's
  register state without the generic layer needing to know the VS_FUNC/
  VS_DLL_STATUS/VS_DLL_CONFIG offsets.

  @param[in]  Config   Platform configuration for this controller instance.
  @param[out] Diag     Caller-allocated struct to populate.
**/
VOID
EFIAPI
SdccPlatformGetDllDiag (
  IN  CONST SDCC5_PLATFORM_CONFIG  *Config,
  OUT SDCC5_DLL_DIAG                *Diag
  );

/**
  Select the HC's card-interface clock/strobe source (vendor HC_SELECT_IN mux).

  Each UHS-I/high-speed mode transition requires routing a different clock
  edge into the HC's sampling logic before (or, for SdccClockSrcNone, instead
  of) that mode's DLL is initialised.  This is a Qualcomm vendor-specific
  register (VS_FUNC) with no SDHCI-spec equivalent.

  @param[in]  Config   Platform configuration for this controller instance.
  @param[in]  Source   Clock/strobe source to select, or SdccClockSrcNone to
                       clear the mux enable (used during cold init before any
                       mode has been negotiated).

  @retval EFI_SUCCESS  Always — this is a direct MMIO write with no failure mode.
**/
EFI_STATUS
EFIAPI
SdccPlatformSelectClockSource (
  IN CONST SDCC5_PLATFORM_CONFIG  *Config,
  IN SDCC5_CLOCK_SOURCE             Source
  );

/**
  Read the vendor last-fetched-ADMA-address diagnostic register.

  Diagnostic-only: used by the generic driver's CMD18-timeout handler to log
  the ADMA engine's last-fetched descriptor address when a data transfer
  times out, to help distinguish a DMA stall from a descriptor address
  issue.  This register (VS_ADMA_ERR_ADDR0) is Qualcomm vendor-specific and
  has no SDHCI-spec equivalent.

  @param[in]  Config   Platform configuration for this controller instance.

  @return  Raw register value (last AXI address the ADMA engine fetched).
**/
UINT32
EFIAPI
SdccPlatformReadAdmaErrAddr (
  IN CONST SDCC5_PLATFORM_CONFIG  *Config
  );

#endif /* __SDCC_PLATFORM_LIB_H__ */
