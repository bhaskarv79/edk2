/** @file
  Generic MMIO-based UFS Host Controller DXE driver entry point and
  DriverBinding implementation.

  UfsMemHcDxe binds to handles carrying EDKII_UFS_MEM_HC_DEVICE_PROTOCOL and
  produces EDKII_UFS_HOST_CONTROLLER_PROTOCOL on the same handle so that
  UfsPassThruDxe can consume it.

  Copyright (c) 2024, EDK2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsMemHcDxe.h"

///
/// Fixed vtable of HC protocol ops, copied via CopyMem into each controller's
/// Private->UfsHc in Start().  CR() later recovers Private from a UfsHc
/// pointer using that embedding.
///
EDKII_UFS_HOST_CONTROLLER_PROTOCOL  gUfsMemHcTemplate = {
  UfsMemHcGetMmioBar,
  UfsMemHcAllocateBuffer,
  UfsMemHcFreeBuffer,
  UfsMemHcMap,
  UfsMemHcUnmap,
  UfsMemHcFlush,
  UfsMemHcMmioRead,
  UfsMemHcMmioWrite,
};

EFI_DRIVER_BINDING_PROTOCOL  gUfsMemHcDriverBinding = {
  UfsMemHcDriverBindingSupported,
  UfsMemHcDriverBindingStart,
  UfsMemHcDriverBindingStop,
  0x10,
  NULL,
  NULL,
};

/**
  Tests whether this driver supports the given controller handle.

  Supported() opens EDKII_UFS_MEM_HC_DEVICE_PROTOCOL BY_TEST and immediately
  closes it.  Any other result means the handle is not for this driver.

  @param[in]  This                 DriverBinding instance.
  @param[in]  Controller           Handle to test.
  @param[in]  RemainingDevicePath  Ignored (device driver).

  @retval EFI_SUCCESS              Protocol present; driver can manage handle.
  @retval EFI_UNSUPPORTED          Protocol absent; not our device.
  @retval other                    Open failed for an unexpected reason.

**/
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                        Status;
  EDKII_UFS_MEM_HC_DEVICE_PROTOCOL  *DevProtocol;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEdkiiUfsMemHcDeviceProtocolGuid,
                  (VOID **)&DevProtocol,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CloseProtocol (
         Controller,
         &gEdkiiUfsMemHcDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return EFI_SUCCESS;
}

/**
  Starts managing a UFS memory-mapped host controller.

  Allocates a private data structure, copies the HC protocol template, opens
  EDKII_UFS_MEM_HC_DEVICE_PROTOCOL BY_DRIVER to prevent other drivers from
  claiming the handle, then installs EDKII_UFS_HOST_CONTROLLER_PROTOCOL.

  @param[in]  This                 DriverBinding instance.
  @param[in]  Controller           Handle of the controller to start.
  @param[in]  RemainingDevicePath  Ignored.

  @retval EFI_SUCCESS              Driver started successfully.
  @retval EFI_OUT_OF_RESOURCES     Cannot allocate private data.
  @retval other                    Protocol open or install failed.

**/
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                        Status;
  EDKII_UFS_MEM_HC_DEVICE_PROTOCOL  *DevProtocol;
  UFS_MEM_HC_PRIVATE_DATA           *Private;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEdkiiUfsMemHcDeviceProtocolGuid,
                  (VOID **)&DevProtocol,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsMemHc] Start: OpenProtocol DevProtocol - %r\n", Status));
    return Status;
  }

  Private = AllocateZeroPool (sizeof (UFS_MEM_HC_PRIVATE_DATA));
  if (Private == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorCloseDevProto;
  }

  Private->Signature   = UFS_MEM_HC_PRIVATE_DATA_SIGNATURE;
  Private->DevProtocol = DevProtocol;
  CopyMem (&Private->UfsHc, &gUfsMemHcTemplate, sizeof (EDKII_UFS_HOST_CONTROLLER_PROTOCOL));

  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Private->UfsHc
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[UfsMemHc] Start: Install UfsHc - %r\n", Status));
    goto ErrorFreePrivate;
  }

  DEBUG ((
    DEBUG_INFO,
    "[UfsMemHc] Start: HciBase=0x%lx Len=0x%x\n",
    DevProtocol->HciMmioBase,
    DevProtocol->HciMmioLength
    ));

  return EFI_SUCCESS;

ErrorFreePrivate:
  FreePool (Private);

ErrorCloseDevProto:
  gBS->CloseProtocol (
         Controller,
         &gEdkiiUfsMemHcDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );
  return Status;
}

/**
  Stops managing a UFS memory-mapped host controller.

  Uninstalls EDKII_UFS_HOST_CONTROLLER_PROTOCOL, frees the private data
  structure, and closes EDKII_UFS_MEM_HC_DEVICE_PROTOCOL.

  @param[in]  This              DriverBinding instance.
  @param[in]  Controller        Handle of the controller to stop.
  @param[in]  NumberOfChildren  Must be 0 (no child handles created).
  @param[in]  ChildHandleBuffer Ignored.

  @retval EFI_SUCCESS           Driver stopped successfully.
  @retval other                 Uninstall or close failed.

**/
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS                          Status;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  UFS_MEM_HC_PRIVATE_DATA             *Private;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  (VOID **)&UfsHc,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private = UFS_MEM_HC_PRIVATE_DATA_FROM_UFSHC (UfsHc);

  Status = gBS->UninstallProtocolInterface (
                  Controller,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &Private->UfsHc
                  );
  if (EFI_ERROR (Status)) {
    //
    // Most commonly EFI_ACCESS_DENIED: a consumer (e.g. UfsPassThruDxe)
    // still has gEdkiiUfsHostControllerProtocolGuid open BY_DRIVER on this
    // handle.  Private and the EDKII_UFS_MEM_HC_DEVICE_PROTOCOL binding are
    // deliberately left intact on this path -- Stop() is expected to be
    // retried by the caller once that consumer disconnects.
    //
    DEBUG ((
      DEBUG_ERROR,
      "[UfsMemHc] Stop: Uninstall gEdkiiUfsHostControllerProtocolGuid failed on "
      "Controller=%p - %r; leaving Private and device-protocol binding intact "
      "for retry\n",
      Controller,
      Status
      ));
    return Status;
  }

  FreePool (Private);

  gBS->CloseProtocol (
         Controller,
         &gEdkiiUfsMemHcDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return EFI_SUCCESS;
}

/**
  Entry point for UfsMemHcDxe.

  @param[in]  ImageHandle  Handle of this image.
  @param[in]  SystemTable  Pointer to EFI System Table.

  @retval EFI_SUCCESS  Driver registered successfully.
  @retval other        EfiLibInstallDriverBindingComponentName2 failed.

**/
EFI_STATUS
EFIAPI
UfsMemHcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &gUfsMemHcDriverBinding,
           ImageHandle,
           &gUfsMemHcComponentName,
           &gUfsMemHcComponentName2
           );
}
