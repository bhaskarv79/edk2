/** @file
  Generic SDCC5 MMIO SDHCI Host Controller DXE driver.

  Produces EFI_SD_MMC_PASS_THRU_PROTOCOL via EFI_DRIVER_BINDING_PROTOCOL.
  Platform-specific configuration (clock frequencies, DLL tuning, GPIO) is
  supplied via the SdccPlatformLib library class.

  No PCI IO, NonDiscoverable, or SdMmcPciHcDxe is required.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

// ─── Vendor GUID for SDCC MMIO HC controller handles ─────────────────────────
// Installed on each per-instance handle so DriverBindingSupported can identify
// our own handles from the generic DEVICE_PATH stream.
// {A3C1D2E4-B5F6-47A8-9C0D-1E2F3A4B5C6D}

EFI_GUID  gSdccMmioHcVendorGuid = {
  0xA3C1D2E4, 0xB5F6, 0x47A8,
  { 0x9C, 0x0D, 0x1E, 0x2F, 0x3A, 0x4B, 0x5C, 0x6D }
};

// ─── SDCC vendor device-path type ────────────────────────────────────────────

#pragma pack (1)
typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;
  EFI_GUID                  Guid;
  UINT32                    InstanceIndex;
} SDCC_VENDOR_DP;
#pragma pack ()

// ─── PassThru template ───────────────────────────────────────────────────────

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
    1. SdccPlatformInit — clock enable + GPIO config (via platform shim)
    2. Read HC version; set TIMEOUT_CTRL, CTRL1 to 1-bit, clock to 400 kHz,
       enable interrupt status; clear HC_SELECT_IN_EN
    3. CMD0 → CMD1 → CMD2 → CMD3(RCA) → CMD7(select)
    4. SdccNegotiateEmmcMode — dynamic mode selection from EXT_CSD

  @param[in]  Private        Per-instance private data (Config already populated).
  @param[in]  InstanceIndex  Zero-based controller index.

  @retval EFI_SUCCESS        eMMC initialized and in Transfer state.
  @retval EFI_DEVICE_ERROR   Device not present or CMD sequence failed.
  @retval other              Propagated from SDHCI setup helpers.
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

  // 1. Platform init (clock enable + GPIO config)
  Status = SdccPlatformInit (InstanceIndex, &Private->Config);
  if (EFI_ERROR (Status)) {
    // Non-fatal: firmware may have already enabled clocks.
    DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: SdccPlatformInit returned %r — continuing\n",
            Status));
  }

  //
  // SdccHcReset() issues SW_RST_ALL (HPG §7.3.8.3) — skipped when the platform
  // sets Config.SkipSwRstAll, e.g. because firmware write-protects the reset
  // from UEFI's execution level, or already leaves the HC in a known-good
  // state and a full reset would race the device.  Otherwise run the
  // spec-compliant full reset that a from-cold SDHCI controller expects.
  //
  if (Private->Config.SkipSwRstAll) {
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: SkipSwRstAll set — skipping SdccHcReset\n"));
  } else {
    Status = SdccHcReset (Base);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] eMMC: SdccHcReset failed %r\n", Status));
      return Status;
    }
  }

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

  // 4. Prepare HC for identification — clear any clock/strobe source mux
  //    left set by a prior boot session before CMD0/ACMD41 identification.
  SdccPlatformSelectClockSource (&Private->Config, SdccClockSrcNone);
  DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: clock source mux cleared\n"));

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
    UINT8  Hctl1;
    Hctl1  = MmioRead8 (Base + SDCC_HC_HOST_CTRL1);
    Hctl1 &= (UINT8)~(BIT5 | BIT1);  // clear 8-bit and 4-bit → 1-bit
    MmioWrite8 (Base + SDCC_HC_HOST_CTRL1, Hctl1);
  }

  // 5. eMMC identification: CMD0 → CMD1 → CMD2 → CMD3 → CMD7

  // CMD0 GO_IDLE — broadcast, no response; failure is not device-detectable.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 0, SdMmcCommandTypeBc, SdMmcResponseTypeR1, 0, NULL, 0, &StsBlk, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: CMD0 status %r (broadcast, continuing)\n", Status));
    }
    MicroSecondDelay (1000u);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD0 sent\n"));
  }

  // CMD1 SEND_OP_COND — poll until busy cleared.
  // Up to 500 retries × 2 ms = ~1 s total.
  // Per-call timeout: 50 ms — prevents 3 s spin from multiplying into 1500 s
  // worst-case stall on a dead device.
  //
  // EFI_NOT_READY means CMD_INHIBIT is stuck (SW_RST blocked at EL2 — no
  // recovery between retries).  One NOT_READY means all will be NOT_READY;
  // exit immediately to avoid 500 × 50 ms of log spam when eMMC is absent.
  Ocr   = 0;
  Retry = 500;
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    while (Retry-- > 0) {
      Status = SdccEmmcCmd (Private, 1, SdMmcCommandTypeBcr,
                                  SdMmcResponseTypeR3, 0x40FF8000u, NULL, 0, &StsBlk, 50000u);
      if (Status == EFI_NOT_READY) {
        DEBUG ((DEBUG_WARN, "[SdccHcDxe] eMMC: CMD1 CMD_INHIBIT stuck — no eMMC device\n"));
        break;
      }
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

  // CMD2 ALL_SEND_CID — R2 response.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 2, SdMmcCommandTypeBcr, SdMmcResponseTypeR2, 0, NULL, 0, &StsBlk, 0);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD2 Status=%r\n", Status));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] eMMC: CMD2 (ALL_SEND_CID) failed — no device\n"));
      return EFI_DEVICE_ERROR;
    }
  }

  // CMD3 SET_RELATIVE_ADDR — platform-configured RCA from Config.EmmcRca.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 3, SdMmcCommandTypeAc,
                               SdMmcResponseTypeR1, (UINT32)Private->Config.EmmcRca << 16, NULL, 0, &StsBlk, 0);
    Private->RcaProtocol.Rca = Private->Config.EmmcRca;
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD3 RCA=%u Status=%r\n",
            (UINT32)Private->Config.EmmcRca, Status));
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // CMD7 SELECT — move device to Transfer state.
  {
    EFI_SD_MMC_STATUS_BLOCK  StsBlk;
    Status = SdccEmmcCmd (Private, 7, SdMmcCommandTypeAc,
                               SdMmcResponseTypeR1b, (UINT32)Private->Config.EmmcRca << 16, NULL, 0, &StsBlk, 0);
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] eMMC: CMD7 (select RCA=%u) Status=%r\n",
            (UINT32)Private->Config.EmmcRca, Status));
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

// ─── Helper: free all resources for a Private instance ───────────────────────

/**
  Cancel timer events and free private data for a controller instance.

  Called from DriverBindingStop and from error paths in DriverBindingStart.
  Does NOT uninstall protocols — callers are responsible for uninstalling
  protocols on the controller handle before calling this.

  @param[in]  Private  Per-instance private data to free.
**/
STATIC
VOID
SdccFreeInstance (
  IN SDCC_HC_PRIVATE_DATA  *Private
  )
{
  if (Private->TimerEvent != NULL) {
    gBS->SetTimer (Private->TimerEvent, TimerCancel, 0);
    gBS->CloseEvent (Private->TimerEvent);
    Private->TimerEvent = NULL;
  }
  if (Private->ConnectEvent != NULL) {
    gBS->SetTimer (Private->ConnectEvent, TimerCancel, 0);
    gBS->CloseEvent (Private->ConnectEvent);
    Private->ConnectEvent = NULL;
  }
  if (Private->DeferredConnectEvent != NULL) {
    gBS->CloseEvent (Private->DeferredConnectEvent);
    Private->DeferredConnectEvent = NULL;
  }
  FreePool (Private);
}

// ─── EFI_DRIVER_BINDING_PROTOCOL ─────────────────────────────────────────────

// Canonical controller handles established by the entry point, indexed by
// instance.  Set once in the entry point; Supported() uses these to reject
// duplicate handles carrying the same VendorGUID but at a different address.
#define SDCC_MAX_INSTANCES  4
STATIC EFI_HANDLE  mSdccCtrlHandle[SDCC_MAX_INSTANCES];

/**
  Tests whether this driver supports a given controller handle.

  Checks for the SDCC MMIO vendor GUID in the handle's device path to
  identify controller handles created by SdccMmioHcDxeEntryPoint.

  @param[in]  This                  Driver binding protocol instance.
  @param[in]  Controller            Handle of the controller to test.
  @param[in]  RemainingDevicePath   Ignored (bus driver pattern not used).

  @retval EFI_SUCCESS               This driver can manage the controller.
  @retval EFI_ALREADY_STARTED       Driver already managing this controller.
  @retval EFI_UNSUPPORTED           Not a recognised SDCC MMIO controller handle.
**/
STATIC
EFI_STATUS
EFIAPI
SdccMmioHcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL *Dp;
  SDCC_VENDOR_DP           *Node;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&Dp,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  // Walk the device path looking for our vendor node.
  while (!IsDevicePathEnd (Dp)) {
    if ((DevicePathType (Dp) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Dp) == HW_VENDOR_DP) &&
        (DevicePathNodeLength (Dp) == sizeof (SDCC_VENDOR_DP)))
    {
      Node = (SDCC_VENDOR_DP *)Dp;
      if (CompareGuid (&Node->Guid, &gSdccMmioHcVendorGuid)) {
        //
        // Our vendor node must be the terminal node (immediately followed by
        // EndNode).  Child handles created by SdDxe/EmmcDxe append SD/EMMC
        // device path nodes AFTER our vendor node, so their device paths contain
        // our GUID as a prefix but are NOT our controller handle.  Returning
        // EFI_SUCCESS for child handles would cause DriverBindingStart to fire
        // again on each child, resetting the HC and creating an infinite loop.
        //
        if (!IsDevicePathEnd (NextDevicePathNode (Dp))) {
          DEBUG ((DEBUG_VERBOSE,
                  "[SdccHcDxe] Supported: Controller=%p Instance=%u — child path, REJECTING\n",
                  Controller, Node->InstanceIndex));
          return EFI_UNSUPPORTED;
        }

        //
        // Reject handles that were not created by our entry point.  The entry
        // point records every controller handle it creates in mSdccCtrlHandle[].
        // Any handle carrying our VendorGUID but NOT in that table is a spurious
        // duplicate (e.g. created by ConPlatform path-copy or a second driver)
        // and must be rejected to prevent reinitialising the HC.
        //
        UINTN  Idx;
        BOOLEAN  IsOurs = FALSE;
        for (Idx = 0; Idx < SDCC_MAX_INSTANCES; Idx++) {
          if (mSdccCtrlHandle[Idx] == Controller) {
            IsOurs = TRUE;
            break;
          }
        }
        if (!IsOurs) {
          DEBUG ((DEBUG_VERBOSE,
                  "[SdccHcDxe] Supported: Controller=%p Instance=%u — NOT in entry-point table, REJECTING\n",
                  Controller, Node->InstanceIndex));
          return EFI_UNSUPPORTED;
        }

        // Reject re-entry: if PassThru is already installed this driver already
        // owns the handle.
        EFI_SD_MMC_PASS_THRU_PROTOCOL  *Dummy;
        Status = gBS->OpenProtocol (
                        Controller,
                        &gEfiSdMmcPassThruProtocolGuid,
                        (VOID **)&Dummy,
                        This->DriverBindingHandle,
                        Controller,
                        EFI_OPEN_PROTOCOL_GET_PROTOCOL
                        );
        DEBUG ((DEBUG_VERBOSE,
                "[SdccHcDxe] Supported: Controller=%p Instance=%u PassThruCheck=%r\n",
                Controller, Node->InstanceIndex, Status));
        if (!EFI_ERROR (Status)) {
          return EFI_ALREADY_STARTED;
        }
        return EFI_SUCCESS;
      }
    }
    Dp = NextDevicePathNode (Dp);
  }
  return EFI_UNSUPPORTED;
}

/**
  Start managing a SDCC MMIO controller handle.

  Retrieves the instance index from the vendor device path node, calls
  SdccGetPlatformConfig to get MMIO base and platform config, initializes
  the HC, then installs EFI_SD_MMC_PASS_THRU_PROTOCOL on the handle.

  @param[in]  This                  Driver binding protocol instance.
  @param[in]  Controller            Handle of the controller to start.
  @param[in]  RemainingDevicePath   Ignored.

  @retval EFI_SUCCESS               PassThru protocol installed.
  @retval EFI_OUT_OF_RESOURCES      Memory allocation failed.
  @retval EFI_DEVICE_ERROR          HW initialization failed.
  @retval other                     Propagated from helpers.
**/
STATIC
EFI_STATUS
EFIAPI
SdccMmioHcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  EFI_DEVICE_PATH_PROTOCOL *Dp;
  SDCC_VENDOR_DP           *Node;
  UINTN                    InstanceIdx;
  SDCC5_PLATFORM_CONFIG    Config;
  SDCC_HC_PRIVATE_DATA    *Private;

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] DriverBindingStart entered Controller=%p\n", Controller));

  // Retrieve the instance index from the vendor device path node.
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&Dp,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InstanceIdx = MAX_UINTN;
  while (!IsDevicePathEnd (Dp)) {
    if ((DevicePathType (Dp) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Dp) == HW_VENDOR_DP) &&
        (DevicePathNodeLength (Dp) == sizeof (SDCC_VENDOR_DP)))
    {
      Node = (SDCC_VENDOR_DP *)Dp;
      if (CompareGuid (&Node->Guid, &gSdccMmioHcVendorGuid)) {
        InstanceIdx = (UINTN)Node->InstanceIndex;
        break;
      }
    }
    Dp = NextDevicePathNode (Dp);
  }
  if (InstanceIdx == MAX_UINTN) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Config, sizeof Config);
  Status = SdccGetPlatformConfig (InstanceIdx, &Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: GetPlatformConfig: %r\n",
            (UINT32)InstanceIdx, Status));
    return Status;
  }

  if (Config.BaseAddress == 0) {
    return EFI_DEVICE_ERROR;
  }

  Private = AllocateZeroPool (sizeof (SDCC_HC_PRIVATE_DATA));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Private->Signature = SDCC_HC_PRIVATE_SIGNATURE;
  Private->Handle    = Controller;
  CopyMem (&Private->Config, &Config, sizeof Config);
  InitializeListHead (&Private->Queue);
  CopyMem (&Private->PassThru, &mPassThruTemplate, sizeof mPassThruTemplate);

  Status = SdccHcMapMmio (Config.BaseAddress, SIZE_4KB);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: MapMmio 0x%lx failed: %r\n",
            (UINT32)InstanceIdx, Config.BaseAddress, Status));
    FreePool (Private);
    return Status;
  }

  if (Config.IsEmmc) {
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] Start[%u]: eMMC init begin\n", (UINT32)InstanceIdx));
    Status = SdccInitEmmc (Private, InstanceIdx);
  } else {
    DEBUG ((DEBUG_INFO, "[SdccHcDxe] Start[%u]: SD card init begin\n", (UINT32)InstanceIdx));
    Status = SdccPlatformInit (InstanceIdx, &Config);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] Start[%u]: SdccPlatformInit: %r (non-fatal)\n",
              (UINT32)InstanceIdx, Status));
    }
    Status = SdccHcColdInit (Private);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "[SdccHcDxe] Start[%u]: HW init failed (%r)\n",
            (UINT32)InstanceIdx, Status));
    FreePool (Private);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] Start[%u]: HW init OK\n", (UINT32)InstanceIdx));
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  SdccProcessAsyncTaskList,
                  Private,
                  &Private->TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: CreateEvent(TimerEvent) failed: %r\n",
            (UINT32)InstanceIdx, Status));
    FreePool (Private);
    return Status;
  }
  Status = gBS->SetTimer (Private->TimerEvent, TimerPeriodic, SDCC_ASYNC_TIMER_PERIOD);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: SetTimer(TimerEvent) failed: %r\n",
            (UINT32)InstanceIdx, Status));
    SdccFreeInstance (Private);
    return Status;
  }

  // 100 ms hot-plug timer for SD card
  if (!Config.IsEmmc) {
    //
    // EVT_NOTIFY_SIGNAL requires NotifyTpl >= TPL_CALLBACK (EDK2 DxeCore
    // rejects TPL_APPLICATION with EFI_INVALID_PARAMETER).  Use TPL_CALLBACK
    // and let SdccDeferredConnect lower TPL briefly around ConnectController.
    //
    Status = gBS->CreateEvent (
                    EVT_NOTIFY_SIGNAL,
                    TPL_CALLBACK,
                    SdccDeferredConnect,
                    Private,
                    &Private->DeferredConnectEvent
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: CreateEvent(DeferredConnect) failed: %r\n",
              (UINT32)InstanceIdx, Status));
      SdccFreeInstance (Private);
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
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: CreateEvent(ConnectEvent) failed: %r\n",
              (UINT32)InstanceIdx, Status));
      SdccFreeInstance (Private);
      return Status;
    }
    Status = gBS->SetTimer (Private->ConnectEvent, TimerPeriodic,
                             EFI_TIMER_PERIOD_MILLISECONDS (100));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: SetTimer(ConnectEvent) failed: %r\n",
              (UINT32)InstanceIdx, Status));
      SdccFreeInstance (Private);
      return Status;
    }
  }

  if (Config.IsEmmc) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->Handle,
                    &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                    &gEfiSdccHcRcaProtocolGuid,     &Private->RcaProtocol,
                    NULL
                    );
  } else {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->Handle,
                    &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                    NULL
                    );
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Start[%u]: InstallMultipleProtocol failed: %r\n",
            (UINT32)InstanceIdx, Status));
    SdccFreeInstance (Private);
    return Status;
  }

  DEBUG ((DEBUG_INFO,
          "[SdccHcDxe] Start[%u]: PassThru installed Controller=%p→Handle=%p Base=0x%lx\n",
          (UINT32)InstanceIdx, Controller, Private->Handle, Config.BaseAddress));

  // SdDxe/EmmcDxe will not find this handle unless ConnectController is
  // called now.  BDS ran ConnectAll before our driver loaded, so those
  // bus drivers have already scanned all pre-existing handles.  The
  // hot-plug path (SdccDeferredConnect) only fires on a *state change*,
  // so a card that is already present when we install PassThru would
  // never trigger it.  Call ConnectController here — DriverBindingStart
  // runs at TPL_APPLICATION so this is safe per UEFI spec §6.3.
  DEBUG ((DEBUG_INFO, "[SdccHcDxe] Start[%u]: calling ConnectController (recursive)\n",
          (UINT32)InstanceIdx));
  gBS->ConnectController (Private->Handle, NULL, NULL, TRUE);
  DEBUG ((DEBUG_INFO, "[SdccHcDxe] Start[%u]: ConnectController returned\n",
          (UINT32)InstanceIdx));

  return EFI_SUCCESS;
}

/**
  Stop managing a SDCC MMIO controller handle.

  Uninstalls PassThru (and optionally RCA) protocol, cancels timers, and
  frees Private.

  @param[in]  This               Driver binding protocol instance.
  @param[in]  Controller         Handle of the controller to stop.
  @param[in]  NumberOfChildren   Number of child handles (always 0 here).
  @param[in]  ChildHandleBuffer  Child handles (unused).

  @retval EFI_SUCCESS            Protocols uninstalled and resources freed.
  @retval EFI_DEVICE_ERROR       PassThru not found on this handle.
**/
STATIC
EFI_STATUS
EFIAPI
SdccMmioHcDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    Controller,
  IN UINTN                         NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                     Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru;
  SDCC_HC_PRIVATE_DATA          *Private;
  EFI_SDCC_HC_RCA_PROTOCOL      *RcaProtocol;

  DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Stop Controller=%p NChildren=%u\n",
          Controller, (UINT32)NumberOfChildren));

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiSdMmcPassThruProtocolGuid,
                  (VOID **)&PassThru,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Private = SDCC_HC_PRIVATE_FROM_PASSTHRU (PassThru);

  gBS->UninstallProtocolInterface (
         Controller,
         &gEfiSdMmcPassThruProtocolGuid,
         &Private->PassThru
         );

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiSdccHcRcaProtocolGuid,
                  (VOID **)&RcaProtocol,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (!EFI_ERROR (Status)) {
    gBS->UninstallProtocolInterface (
           Controller,
           &gEfiSdccHcRcaProtocolGuid,
           &Private->RcaProtocol
           );
  }

  SdccFreeInstance (Private);
  return EFI_SUCCESS;
}

// ─── Driver binding instance ──────────────────────────────────────────────────

EFI_DRIVER_BINDING_PROTOCOL  gSdccMmioHcDriverBinding = {
  SdccMmioHcDriverBindingSupported,
  SdccMmioHcDriverBindingStart,
  SdccMmioHcDriverBindingStop,
  0x10,
  NULL,
  NULL
};

// ─── Driver entry point ───────────────────────────────────────────────────────

/**
  Driver entry point.

  Creates one controller handle per SDCC instance (via SdccGetPlatformConfig),
  then installs the driver binding and component name protocols.

  Controller handles are vendor device path nodes carrying the instance index.
  DriverBindingStart is invoked by ConnectController when these handles are
  processed.

  @param[in]  ImageHandle  Firmware-allocated handle for the EFI image.
  @param[in]  SystemTable  Pointer to the EFI System Table.

  @retval EFI_SUCCESS      Driver binding protocol installed.
  @retval other            Propagated from EfiLibInstallDriverBindingComponentName2.
**/
EFI_STATUS
EFIAPI
SdccMmioHcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS               Status;
  UINTN                    InstanceIdx;
  SDCC5_PLATFORM_CONFIG    Config;
  SDCC_VENDOR_DP           VendorNode;
  EFI_DEVICE_PATH_PROTOCOL *DevPath;
  EFI_HANDLE               CtrlHandle;

  DEBUG ((DEBUG_INFO, "[SdccHcDxe] SdccMmioHcDxe entry point\n"));

  // Create one controller handle per platform-reported SDCC instance.
  // Each handle carries a vendor device path node so DriverBindingSupported
  // can identify it later.
  for (InstanceIdx = 0; ; InstanceIdx++) {
    ZeroMem (&Config, sizeof Config);
    Status = SdccGetPlatformConfig (InstanceIdx, &Config);
    if (Status == EFI_NOT_FOUND) {
      break;
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "[SdccHcDxe] GetPlatformConfig[%u]: %r — skipping\n",
              (UINT32)InstanceIdx, Status));
      continue;
    }
    if (Config.BaseAddress == 0) {
      continue;
    }

    ZeroMem (&VendorNode, sizeof VendorNode);
    VendorNode.Header.Type    = HARDWARE_DEVICE_PATH;
    VendorNode.Header.SubType = HW_VENDOR_DP;
    SetDevicePathNodeLength (&VendorNode.Header, sizeof (SDCC_VENDOR_DP));
    CopyGuid (&VendorNode.Guid, &gSdccMmioHcVendorGuid);
    VendorNode.InstanceIndex  = (UINT32)InstanceIdx;

    DevPath = AppendDevicePathNode (NULL, (EFI_DEVICE_PATH_PROTOCOL *)&VendorNode);
    if (DevPath == NULL) {
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Instance %u: DevPath alloc failed\n",
              (UINT32)InstanceIdx));
      continue;
    }

    //
    // Guard against multiple entry-point invocations (e.g. when the DXE
    // dispatcher re-dispatches this image after a new protocol is installed).
    // LocateDevicePath consumes the path pointer until it reaches the deepest
    // handle whose device path is a prefix of DevPath.  If the result handle
    // is non-NULL and RemainingDevicePath points at the end-of-path node, a
    // handle with exactly this vendor path already exists — skip creation.
    //
    {
      EFI_DEVICE_PATH_PROTOCOL  *CheckPath = DevPath;
      EFI_HANDLE                 ExistingHandle = NULL;
      EFI_STATUS                 LocateStatus;
      LocateStatus = gBS->LocateDevicePath (
                            &gEfiDevicePathProtocolGuid,
                            &CheckPath,
                            &ExistingHandle
                            );
      if (!EFI_ERROR (LocateStatus) && IsDevicePathEnd (CheckPath)) {
        DEBUG ((DEBUG_WARN,
                "[SdccHcDxe] EntryPoint: Instance %u: handle=%p already exists — skipping\n",
                (UINT32)InstanceIdx, ExistingHandle));
        FreePool (DevPath);
        if (InstanceIdx < SDCC_MAX_INSTANCES) {
          mSdccCtrlHandle[InstanceIdx] = ExistingHandle;
        }
        continue;
      }
    }

    CtrlHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &CtrlHandle,
                    &gEfiDevicePathProtocolGuid, DevPath,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      FreePool (DevPath);
      DEBUG ((DEBUG_ERROR, "[SdccHcDxe] Instance %u: InstallDevicePath: %r\n",
              (UINT32)InstanceIdx, Status));
      continue;
    }

    DEBUG ((DEBUG_INFO, "[SdccHcDxe] EntryPoint: Instance %u: controller handle=%p created\n",
            (UINT32)InstanceIdx, CtrlHandle));

    // Record the canonical handle so Supported() can reject duplicates.
    if (InstanceIdx < SDCC_MAX_INSTANCES) {
      mSdccCtrlHandle[InstanceIdx] = CtrlHandle;
    }
  }

  // Install driver binding + component name protocols.
  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gSdccMmioHcDriverBinding,
             ImageHandle,
             &gSdccMmioHcComponentName,
             &gSdccMmioHcComponentName2
             );
  ASSERT_EFI_ERROR (Status);
  return Status;
}
