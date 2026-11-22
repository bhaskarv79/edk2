/** @file
  Lab UFS host controller platform driver.

  Installs EDKII_UFS_HC_PLATFORM_PROTOCOL so that the platform callback
  path of UfsMemHcDxe and UfsPassThruDxe can be tested on QEMU, the way a
  silicon platform driver such as QcomUfsHcDxe uses it:
  - OverrideHcInfo logs the capabilities and version it is given;
  - Callback logs every phase, checks the driver interface it receives and,
    after link start-up, reads PA_CONNECTEDTXDATALANES through it;
  - the phase given by QEMU's fw_cfg file opt/org.tianocore/UfsLabFailPhase
    (a decimal phase number), else by PcdUfsLabFailPhase (0xFF: none), fails
    with EFI_DEVICE_ERROR, to test how the UFS stack handles a failing
    platform.

  Lab code, not intended for upstream.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/QemuFwCfgLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>

#define UIC_DME_GET                 0x01
#define UIC_ATTR_CONNECTED_TX_LANE  0x1561
#define UFS_LAB_FW_CFG_FAIL_PHASE   "opt/org.tianocore/UfsLabFailPhase"

STATIC UINT8  mFailPhase;

STATIC CONST CHAR8  *CONST  mPhaseName[] = {
  "PreHce",
  "PostHce",
  "PreLinkStartup",
  "PostLinkStartup"
};

/**
  Log the controller information and leave it unchanged.

  @param[in]      ControllerHandle  Handle of the UFS controller.
  @param[in, out] HcInfo            Host controller information.

  @retval EFI_SUCCESS            Information logged.
  @retval EFI_INVALID_PARAMETER  HcInfo is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
UfsLabOverrideHcInfo (
  IN     EFI_HANDLE         ControllerHandle,
  IN OUT EDKII_UFS_HC_INFO  *HcInfo
  )
{
  if (HcInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_INFO, "UfsLab: OverrideHcInfo cap=0x%08x ver=0x%08x\n", HcInfo->Capabilities, HcInfo->Version));
  return EFI_SUCCESS;
}

/**
  Log a platform callback phase; fail it when PcdUfsLabFailPhase asks to.

  @param[in]      ControllerHandle  Handle of the UFS controller.
  @param[in]      CallbackPhase     The phase.
  @param[in, out] CallbackData      EDKII_UFS_HC_DRIVER_INTERFACE.

  @retval EFI_SUCCESS            Phase handled.
  @retval EFI_INVALID_PARAMETER  Unknown phase or missing driver interface.
  @retval EFI_DEVICE_ERROR       Injected failure.
**/
STATIC
EFI_STATUS
EFIAPI
UfsLabCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE  *Interface;
  EDKII_UIC_COMMAND              UicCommand;
  EFI_STATUS                     Status;

  if ((UINTN)CallbackPhase >= ARRAY_SIZE (mPhaseName)) {
    DEBUG ((DEBUG_ERROR, "UfsLab: unknown phase %u\n", CallbackPhase));
    return EFI_INVALID_PARAMETER;
  }

  Interface = CallbackData;
  if ((Interface == NULL) || (Interface->UfsHcProtocol == NULL) || (Interface->UfsExecUicCommand == NULL)) {
    DEBUG ((DEBUG_ERROR, "UfsLab: phase %u (%a) without a driver interface\n", CallbackPhase, mPhaseName[CallbackPhase]));
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_INFO, "UfsLab: phase %u (%a)\n", CallbackPhase, mPhaseName[CallbackPhase]));

  if (CallbackPhase == (EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE)mFailPhase) {
    DEBUG ((DEBUG_ERROR, "UfsLab: injected failure in phase %u (%a)\n", CallbackPhase, mPhaseName[CallbackPhase]));
    return EFI_DEVICE_ERROR;
  }

  if (CallbackPhase == EdkiiUfsHcPostLinkStartup) {
    UicCommand.Opcode = UIC_DME_GET;
    UicCommand.Arg1   = UIC_ATTR_CONNECTED_TX_LANE << 16;
    UicCommand.Arg2   = 0;
    UicCommand.Arg3   = 0;
    Status            = Interface->UfsExecUicCommand (Interface, &UicCommand);
    DEBUG ((DEBUG_INFO, "UfsLab: DME_GET PA_CONNECTEDTXDATALANES: %r, value %u\n", Status, UicCommand.Arg3));
  }

  return EFI_SUCCESS;
}

STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  mUfsLabPlatform = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
  UfsLabOverrideHcInfo,
  UfsLabCallback,
  EdkiiUfsCardRefClkFreq19p2Mhz,
  FALSE,
  FALSE
};

/**
  Read the phase to fail: QEMU's fw_cfg file opt/org.tianocore/UfsLabFailPhase
  when the machine has one, so that each phase can be tested without
  rebuilding, else PcdUfsLabFailPhase.

  @return The phase number, 0xFF for none.
**/
STATIC
UINT8
UfsLabGetFailPhase (
  VOID
  )
{
  FIRMWARE_CONFIG_ITEM  Item;
  UINTN                 Size;
  CHAR8                 Value[4];

  if (!QemuFwCfgIsAvailable () ||
      EFI_ERROR (QemuFwCfgFindFile (UFS_LAB_FW_CFG_FAIL_PHASE, &Item, &Size)) ||
      (Size == 0) || (Size >= sizeof (Value)))
  {
    return PcdGet8 (PcdUfsLabFailPhase);
  }

  QemuFwCfgSelectItem (Item);
  QemuFwCfgReadBytes (Size, Value);
  Value[Size] = '\0';
  return (UINT8)AsciiStrDecimalToUintn (Value);
}

/**
  Entry point: install the lab platform protocol.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to the EFI system table.

  @retval EFI_SUCCESS  Protocol installed.
  @retval Others       Installation failed.
**/
EFI_STATUS
EFIAPI
UfsHcPlatformLabDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  Handle     = NULL;
  mFailPhase = UfsLabGetFailPhase ();
  DEBUG ((DEBUG_INFO, "UfsLab: platform protocol v%u, fail phase 0x%02x\n", mUfsLabPlatform.Version, mFailPhase));
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEdkiiUfsHcPlatformProtocolGuid,
                &mUfsLabPlatform,
                NULL
                );
}
