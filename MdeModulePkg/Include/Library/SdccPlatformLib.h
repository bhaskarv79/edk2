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
  /// EFI_CLOCK_PROTOCOL clock names for this controller.
  /// NULL = skip clock enable (already enabled by platform firmware).
  ///
  CONST CHAR8          *ClkAhbName;    ///< e.g. "gcc_sdcc1_ahb_clk"
  CONST CHAR8          *ClkAppsName;   ///< e.g. "gcc_sdcc1_apps_clk"

  ///
  /// TLMM GPIO drive-strength and pull configuration.
  /// TlmmGpioBase = 0 → skip GPIO configuration.
  ///
  EFI_PHYSICAL_ADDRESS  TlmmGpioBase;  ///< TLMM SDC pad-ctrl block base
  UINT32                TlmmHdrvCfg0;  ///< SDC1_HDRV_PULL_CTL_0 value
  UINT32                TlmmHdrvCfg1;  ///< SDC1_HDRV_PULL_CTL_1 value

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

#endif /* __SDCC_PLATFORM_LIB_H__ */
