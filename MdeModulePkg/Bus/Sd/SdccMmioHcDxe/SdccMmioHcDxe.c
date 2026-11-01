/** @file
  Generic Qualcomm SDCC5 MMIO SDHCI Host Controller DXE driver entry point.

  Iterates over all controller instances reported by SdccGetPlatformConfig(),
  initializes each, then installs EFI_SD_MMC_PASS_THRU_PROTOCOL directly.

  No PCI IO, NonDiscoverable, or SdMmcPciHcDxe is required.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

STATIC EFI_SD_MMC_PASS_THRU_PROTOCOL  mPassThruTemplate = {
  sizeof (UINT32),           // IoAlign — DWORD-aligned buffers
  SdccPassThruPassThru,
  SdccPassThruGetNextSlot,
  SdccPassThruBuildDevicePath,
  SdccPassThruGetSlotNumber,
  SdccPassThruResetDevice,
};

// ─── eMMC initialization ──────────────────────────────────────────────────────

/**
  Initialize the eMMC controller and device.

  Sequence:
    1. SdccPlatformInit — clock enable + TLMM config (via platform shim)
    2. Read HC version; set TIMEOUT_CTRL, CTRL1 to 1-bit, clock to 400 kHz,
       enable interrupt status; clear HC_SELECT_IN_EN
    3. CMD0 → CMD1 → CMD2 → CMD3(RCA=2) → CMD7(select)
    4. SdccNegotiateEmmcMode — dynamic mode selection from EXT_CSD
**/
STATIC
EFI_STATUS
SdccInitEmmc (
  IN SDCC_HC_PRIVATE_DATA  *Private,
  IN UINTN                  InstanceIndex
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      Ocr;
  UINT32      Retry;

  Base = (UINTN)Private->Config.BaseAddress;

  // 1. Platform init (clock enable + TLMM GPIO config)
  Status = SdccPlatformInit (InstanceIndex, &Private->Config);
  if (EFI_ERROR (Status)) {
    // Non-fatal: XBL may have already enabled clocks.  Continue with SDHCI init.
    DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: SdccPlatformInit returned %r — continuing\n",
            Status));
  }

  //
  // SdccHcReset (SW_RST_ALL) is intentionally skipped for eMMC (Sdc1).
  // SW_RST_ALL is write-protected from EL2 on Qualcomm Kodiak; XBL/PBL leaves
  // the eMMC HC in a known-good powered state.  Calling SdccHcReset would
  // time out waiting for SW_RST_ALL to self-clear and return EFI_TIMEOUT.
  //

  // 2. Read SDHCI version and capabilities
  Private->ControllerVersion = (UINT16)(MmioRead16 (Base + SDCC_HC_CTRL_VER) & 0xFFu);

  SdccHcReadCapability (Base, &Private->Capability);

  //
  // SDCC5 CAPS reports a software-overridden BaseClkFreq that does not
  // reflect the actual apps_clk.  Use the platform-configured value instead.
  //
  if (Private->Config.CapsBaseClkFreqKHz == 0) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] eMMC: CapsBaseClkFreqKHz=0 — check PCD\n"));
    return EFI_DEVICE_ERROR;
  }

  // 3. Enable SDHCI v4 / 64-bit ADMA mode (no-op for CTRL_VER < v4.00)
  SdccHcEnableV4 (Base, Private->ControllerVersion, Private->Capability);

  // 4. Prepare HC for identification
  {
    UINT32  VFunc = MmioRead32 (Base + SDCC_HC_VS_FUNC);
    if (VFunc & BIT18) {
      MmioAnd32 (Base + SDCC_HC_VS_FUNC, (UINT32)~(BIT18 | (0x7u << 19)));
      DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: HC_SELECT_IN_EN cleared\n"));
    }
  }

  SdccHcInitTimeout (Base);

  Status = SdccHcClockSupply (Base, Private->ControllerVersion,
                               Private->Config.CapsBaseClkFreqKHz,
                               SDCC_HC_IDENTIFY_CLOCK_KHZ);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite16 (Base + SDCC_HC_NOR_INT_STS_EN, 0xFFFFu);
  MmioWrite16 (Base + SDCC_HC_ERR_INT_STS_EN, 0xFFFFu);

  {
    UINT8  Hctl1 = MmioRead8 (Base + SDCC_HC_HOST_CTRL1);
    Hctl1 &= (UINT8)~(BIT5 | BIT1);  // clear 8-bit and 4-bit → 1-bit
    MmioWrite8 (Base + SDCC_HC_HOST_CTRL1, Hctl1);
  }

  // 5. eMMC identification: CMD0 → CMD1 → CMD2 → CMD3 → CMD7

  // CMD0 GO_IDLE — broadcast, no response; failure is not device-detectable.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    // CMD0 GO_IDLE — broadcast, no response; SdMmcResponseTypeR1 (==0) is the
    // closest available enum value since this PassThru protocol has no NoResponse type.
    Status = SdccEmmcCmd (Private, 0, SdMmcCommandTypeBc, SdMmcResponseTypeR1, 0, NULL, 0, &StsBlk, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: CMD0 status %r (broadcast, continuing)\n", Status));
    }
    MicroSecondDelay (1000u);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD0 sent\n"));
  }

  // CMD1 SEND_OP_COND — poll until busy cleared
  // Outer loop: up to 500 retries × 2 ms = ~1 s total.
  // Per-call timeout: 50 ms — prevents SdccEmmcCmd's 3 s spin from multiplying
  // the outer retry count into a 1500 s worst-case stall on a dead device.
  Ocr   = 0;
  Retry = 500;
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    while (Retry-- > 0) {
      Status = SdccEmmcCmd (Private, 1, SdMmcCommandTypeBcr,
                                  SdMmcResponseTypeR3, 0x40FF8000u, NULL, 0, &StsBlk, 50000u);
      Ocr = StsBlk.Resp0;
      if (!EFI_ERROR (Status) && (Ocr & BIT31)) {
        break;
      }
      MicroSecondDelay (2000u);
    }
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD1 OCR=0x%08x Status=%r\n", Ocr, Status));
  }
  if (EFI_ERROR (Status) || !(Ocr & BIT31)) {
    return EFI_DEVICE_ERROR;
  }

  // CMD2 ALL_SEND_CID — R2 response. Failure means no device on the bus; a missing
  // CID makes RCA assignment and CMD7 select meaningless, so treat it as fatal.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 2, SdMmcCommandTypeBcr, SdMmcResponseTypeR2, 0, NULL, 0, &StsBlk, 0);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD2 Status=%r\n", Status));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] eMMC: CMD2 (ALL_SEND_CID) failed — no device\n"));
      return EFI_DEVICE_ERROR;
    }
  }

  // CMD3 SET_RELATIVE_ADDR, RCA=2 (PBL assigns 2; downstream SdccDxe hardcodes this)
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 3, SdMmcCommandTypeAc,
                               SdMmcResponseTypeR1, 2u << 16, NULL, 0, &StsBlk, 0);
    Private->RcaProtocol.Rca = 2u;  // eMMC RCA assigned by host (PBL uses 2 on Kodiak)
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD3 RCA=2 Status=%r\n", Status));
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // CMD7 SELECT — move device to Transfer state
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 7, SdMmcCommandTypeAc,
                               SdMmcResponseTypeR1b, 2u << 16, NULL, 0, &StsBlk, 0);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD7 (select RCA=2) Status=%r\n", Status));
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // 6. Dynamic mode negotiation (reads EXT_CSD, selects HS400-ES / HS400 / HS200 / HS)
  Status = SdccNegotiateEmmcMode (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: mode negotiation failed (%r) — 400 kHz\n",
            Status));
  }

  Private->MediaPresent = TRUE;
  DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: init complete\n"));
  return EFI_SUCCESS;
}

// ─── Driver entry point ───────────────────────────────────────────────────────

EFI_STATUS
EFIAPI
SdccMmioHcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  SDCC_HC_PRIVATE_DATA    *Private;
  UINTN                    InstanceIdx;
  SDCC5_PLATFORM_CONFIG    Config;
  EFI_DEVICE_PATH_PROTOCOL *DevPath;

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] SdccMmioHcDxe entry point\n"));

  for (InstanceIdx = 0; ; InstanceIdx++) {
    ZeroMem (&Config, sizeof Config);
    Status = SdccGetPlatformConfig (InstanceIdx, &Config);
    if (Status == EFI_NOT_FOUND) {
      break;  // No more controller instances
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] GetPlatformConfig[%u]: %r\n",
              (UINT32)InstanceIdx, Status));
      continue;
    }

    DEBUG ((DEBUG_INFO, "[SdccHcDxe] Instance %u: Base=0x%lx IsEmmc=%d\n",
            (UINT32)InstanceIdx, Config.BaseAddress, (INT32)Config.IsEmmc));

    if (Config.BaseAddress == 0) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] Instance %u: BaseAddress=0 — skipping\n",
              (UINT32)InstanceIdx));
      continue;
    }

    Private = AllocateZeroPool (sizeof (SDCC_HC_PRIVATE_DATA));
    if (Private == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Private->Signature = SDCC_HC_PRIVATE_SIGNATURE;
    Private->Handle    = NULL;
    CopyMem (&Private->Config, &Config, sizeof Config);
    InitializeListHead (&Private->Queue);
    CopyMem (&Private->PassThru, &mPassThruTemplate, sizeof mPassThruTemplate);

    // Map SDHCI register window before any register access
    Status = SdccHcMapMmio (Config.BaseAddress, SIZE_4KB);
    if (EFI_ERROR (Status)) {
      FreePool (Private);
      continue;
    }

    // Hardware initialization
    if (Config.IsEmmc) {
      Status = SdccInitEmmc (Private, InstanceIdx);
    } else {
      Status = SdccPlatformInit (InstanceIdx, &Config);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "[SdccHcDxe] Instance %u: SdccPlatformInit: %r (non-fatal)\n",
                (UINT32)InstanceIdx, Status));
      }
      Status = SdccHcColdInit (Private);
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] Instance %u: HW init failed (%r) — skipping\n",
              (UINT32)InstanceIdx, Status));
      FreePool (Private);
      continue;
    }

    // 1 ms async TRB drain timer
    Status = gBS->CreateEvent (
                    EVT_TIMER | EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    SdccProcessAsyncTaskList,
                    Private,
                    &Private->TimerEvent
                    );
    if (EFI_ERROR (Status)) {
      FreePool (Private);
      return Status;
    }
    gBS->SetTimer (Private->TimerEvent, TimerPeriodic, SDCC_ASYNC_TIMER_PERIOD);

    // 100 ms hot-plug timer for SD card
    if (!Config.IsEmmc) {
      // DeferredConnectEvent fires at TPL_APPLICATION — used by SdccEnumerateDevice
      // to call ConnectController safely (UEFI spec §6.3 forbids ConnectController
      // above TPL_APPLICATION).
      Status = gBS->CreateEvent (
                      EVT_NOTIFY_SIGNAL,
                      TPL_APPLICATION,
                      SdccDeferredConnect,
                      Private,
                      &Private->DeferredConnectEvent
                      );
      if (EFI_ERROR (Status)) {
        gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
        gBS->CloseEvent (Private->TimerEvent);
        FreePool (Private);
        return Status;
      }

      Status = gBS->CreateEvent (
                      EVT_TIMER | EVT_NOTIFY_SIGNAL,
                      TPL_CALLBACK,
                      SdccEnumerateDevice,
                      Private,
                      &Private->ConnectEvent
                      );
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (Private->DeferredConnectEvent);
        gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
        gBS->CloseEvent (Private->TimerEvent);
        FreePool (Private);
        return Status;
      }
      gBS->SetTimer (Private->ConnectEvent, TimerPeriodic,
                     EFI_TIMER_PERIOD_MILLISECONDS (100));
    }

    // Vendor device path to avoid collisions with UFS or other SDHCI handles
    {
      #pragma pack (1)
      typedef struct {
        EFI_DEVICE_PATH_PROTOCOL  Header;
        EFI_GUID                  Guid;
        UINT32                    InstanceIndex;
      } SDCC_VENDOR_DP;
      #pragma pack ()

      // {A3C1D2E4-B5F6-47A8-9C0D-1E2F3A4B5C6D} — Qualcomm SDCC MMIO HC vendor path
      EFI_GUID  SdccVendorGuid = {
        0xA3C1D2E4, 0xB5F6, 0x47A8,
        { 0x9C, 0x0D, 0x1E, 0x2F, 0x3A, 0x4B, 0x5C, 0x6D }
      };
      SDCC_VENDOR_DP  VendorNode;

      ZeroMem (&VendorNode, sizeof VendorNode);
      VendorNode.Header.Type    = HARDWARE_DEVICE_PATH;
      VendorNode.Header.SubType = HW_VENDOR_DP;
      SetDevicePathNodeLength (&VendorNode.Header, sizeof (SDCC_VENDOR_DP));
      CopyGuid (&VendorNode.Guid, &SdccVendorGuid);
      VendorNode.InstanceIndex  = (UINT32)InstanceIdx;

      DevPath = AppendDevicePathNode (NULL, (EFI_DEVICE_PATH_PROTOCOL *)&VendorNode);
    }
    if (DevPath == NULL) {
      gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
      gBS->CloseEvent (Private->TimerEvent);
      if (Private->ConnectEvent != NULL) {
        gBS->SetTimer (Private->ConnectEvent, TimerCancel, 0);
        gBS->CloseEvent (Private->ConnectEvent);
      }
      if (Private->DeferredConnectEvent != NULL) {
        gBS->CloseEvent (Private->DeferredConnectEvent);
      }
      FreePool (Private);
      return EFI_OUT_OF_RESOURCES;
    }

    if (Config.IsEmmc) {
      Status = gBS->InstallMultipleProtocolInterfaces (
                      &Private->Handle,
                      &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                      &gEfiSdccHcRcaProtocolGuid,     &Private->RcaProtocol,
                      &gEfiDevicePathProtocolGuid,    DevPath,
                      NULL
                      );
    } else {
      Status = gBS->InstallMultipleProtocolInterfaces (
                      &Private->Handle,
                      &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                      &gEfiDevicePathProtocolGuid,    DevPath,
                      NULL
                      );
    }
    if (EFI_ERROR (Status)) {
      FreePool (DevPath);
      gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
      gBS->CloseEvent (Private->TimerEvent);
      if (Private->ConnectEvent != NULL) {
        gBS->SetTimer (Private->ConnectEvent, TimerCancel, 0);
        gBS->CloseEvent (Private->ConnectEvent);
      }
      if (Private->DeferredConnectEvent != NULL) {
        gBS->CloseEvent (Private->DeferredConnectEvent);
      }
      FreePool (Private);
      continue;
    }

    DEBUG ((DEBUG_INFO,
            "[SdccHcDxe] Instance %u: PassThru installed handle=%p Base=0x%lx\n",
            (UINT32)InstanceIdx, Private->Handle, Config.BaseAddress));
  }

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] Entry point complete\n"));
  return EFI_SUCCESS;
}
