/** @file
  MMIO-based UFS Host Controller DXE driver entry point and Driver Binding
  implementation.

  UfsMemHcDxe binds to non-discoverable devices registered with
  EDKII_NON_DISCOVERABLE_UFS_MMIO_DEVICE_GUID and produces
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL on the same handle, so that
  UfsPassThruDxe can consume it.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsMemHcDxe.h"

//
// HC protocol template, copied into each controller's Private->UfsHc in
// Start().
//
STATIC CONST EDKII_UFS_HOST_CONTROLLER_PROTOCOL  mUfsMemHcTemplate = {
  UfsMemHcGetMmioBar,
  UfsMemHcAllocateBuffer,
  UfsMemHcFreeBuffer,
  UfsMemHcMap,
  UfsMemHcUnmap,
  UfsMemHcFlush,
  UfsMemHcMmioRead,
  UfsMemHcMmioWrite
};

/**
  Find the UFSHCI register window in the resources of a non-discoverable
  device.

  The first memory descriptor is taken as the UFSHCI register window. Any
  non-memory descriptor makes the device unsupported.

  @param[in]   Device    The non-discoverable device.
  @param[out]  MmioBase  Base address of the register window.
  @param[out]  MmioSize  Size of the register window, in bytes.

  @retval EFI_SUCCESS      A usable register window was found.
  @retval EFI_UNSUPPORTED  The resources do not describe a usable register
                           window.

**/
STATIC
EFI_STATUS
UfsMemHcGetMmioResource (
  IN  NON_DISCOVERABLE_DEVICE  *Device,
  OUT EFI_PHYSICAL_ADDRESS     *MmioBase,
  OUT UINT64                   *MmioSize
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Desc;
  BOOLEAN                            Found;

  if (Device->Resources == NULL) {
    return EFI_UNSUPPORTED;
  }

  Found = FALSE;
  for (Desc = Device->Resources; Desc->Desc != ACPI_END_TAG_DESCRIPTOR;
       Desc = (VOID *)((UINT8 *)Desc + Desc->Len + 3))
  {
    if ((Desc->Desc != ACPI_ADDRESS_SPACE_DESCRIPTOR) ||
        (Desc->ResType != ACPI_ADDRESS_SPACE_TYPE_MEM))
    {
      return EFI_UNSUPPORTED;
    }

    if (!Found) {
      *MmioBase = Desc->AddrRangeMin;
      *MmioSize = Desc->AddrLen;
      Found     = TRUE;
    }
  }

  if (!Found || (*MmioSize < UFS_MEM_HC_MIN_MMIO_SIZE)) {
    return EFI_UNSUPPORTED;
  }

  //
  // The whole window must be addressable through a UINTN, since
  // GetUfsHcMmioBar() returns the base as one.
  //
  if ((*MmioBase > MAX_UINTN) || (*MmioSize - 1 > MAX_UINTN - *MmioBase)) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Tests to see if this driver supports a given controller.

  The controller is supported if it carries EDKII_NON_DISCOVERABLE_DEVICE with
  a Type of EDKII_NON_DISCOVERABLE_UFS_MMIO_DEVICE_GUID, coherent DMA, and
  resources that describe a usable register window.

  @param[in]  This                 A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  Controller           The handle of the controller to test.
  @param[in]  RemainingDevicePath  Ignored; this is a device driver.

  @retval EFI_SUCCESS              The device is supported by this driver.
  @retval EFI_ALREADY_STARTED      The device is already being managed by this driver.
  @retval EFI_ACCESS_DENIED        The device is already being managed by a different
                                   driver.
  @retval EFI_UNSUPPORTED          The device is not supported by this driver.

**/
STATIC
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  NON_DISCOVERABLE_DEVICE  *Device;
  EFI_PHYSICAL_ADDRESS     MmioBase;
  UINT64                   MmioSize;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Device,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Device->Type == NULL) ||
      !CompareGuid (Device->Type, &gEdkiiNonDiscoverableUfsMmioDeviceGuid) ||
      (Device->DmaType != NonDiscoverableDeviceDmaTypeCoherent))
  {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = UfsMemHcGetMmioResource (Device, &MmioBase, &MmioSize);
  }

  gBS->CloseProtocol (
         Controller,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

/**
  Starts managing a UFS memory-mapped host controller.

  Opens EDKII_NON_DISCOVERABLE_DEVICE BY_DRIVER, runs the device's
  Initialize() hook and installs EDKII_UFS_HOST_CONTROLLER_PROTOCOL.

  @param[in]  This                 A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  Controller           The handle of the controller to start.
  @param[in]  RemainingDevicePath  Ignored; this is a device driver.

  @retval EFI_SUCCESS              The driver is started on the controller.
  @retval EFI_OUT_OF_RESOURCES     The private data could not be allocated.
  @retval other                    Opening the device, its Initialize() hook,
                                   or installing the protocol failed.

**/
STATIC
EFI_STATUS
EFIAPI
UfsMemHcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS               Status;
  NON_DISCOVERABLE_DEVICE  *Device;
  UFS_MEM_HC_PRIVATE_DATA  *Private;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Device,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private = AllocateZeroPool (sizeof (UFS_MEM_HC_PRIVATE_DATA));
  if (Private == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CloseDevice;
  }

  Private->Signature = UFS_MEM_HC_PRIVATE_DATA_SIGNATURE;
  Private->Device    = Device;
  CopyMem (&Private->UfsHc, &mUfsMemHcTemplate, sizeof (Private->UfsHc));

  Status = UfsMemHcGetMmioResource (Device, &Private->MmioBase, &Private->MmioSize);
  if (EFI_ERROR (Status)) {
    goto FreePrivate;
  }

  if (Device->Initialize != NULL) {
    Status = Device->Initialize (Device);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a: device Initialize() failed - %r\n", __func__, Status));
      goto FreePrivate;
    }
  }

  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Private->UfsHc
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: install UFS HC protocol failed - %r\n", __func__, Status));
    goto FreePrivate;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: base 0x%Lx size 0x%Lx\n",
    __func__,
    (UINT64)Private->MmioBase,
    Private->MmioSize
    ));

  return EFI_SUCCESS;

FreePrivate:
  FreePool (Private);

CloseDevice:
  gBS->CloseProtocol (
         Controller,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );
  return Status;
}

/**
  Stops managing a UFS memory-mapped host controller.

  Uninstalls EDKII_UFS_HOST_CONTROLLER_PROTOCOL, frees the private data and
  closes EDKII_NON_DISCOVERABLE_DEVICE.

  @param[in]  This              A pointer to the EFI_DRIVER_BINDING_PROTOCOL instance.
  @param[in]  Controller        The handle of the controller to stop.
  @param[in]  NumberOfChildren  Not used; this driver creates no child handles.
  @param[in]  ChildHandleBuffer Not used.

  @retval EFI_SUCCESS           The driver is stopped on the controller.
  @retval other                 The protocol could not be found or uninstalled.

**/
STATIC
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

  //
  // This fails, typically with EFI_ACCESS_DENIED, while a consumer such as
  // UfsPassThruDxe still has the protocol open BY_DRIVER. Leave everything
  // in place so Stop() can be retried after that consumer disconnects.
  //
  Status = gBS->UninstallProtocolInterface (
                  Controller,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &Private->UfsHc
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: uninstall UFS HC protocol failed - %r\n", __func__, Status));
    return Status;
  }

  FreePool (Private);

  gBS->CloseProtocol (
         Controller,
         &gEdkiiNonDiscoverableDeviceProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  gUfsMemHcDriverBinding = {
  UfsMemHcDriverBindingSupported,
  UfsMemHcDriverBindingStart,
  UfsMemHcDriverBindingStop,
  0x10,
  NULL,
  NULL
};

/**
  Entry point for UfsMemHcDxe.

  @param[in]  ImageHandle  The firmware allocated handle for the EFI image.
  @param[in]  SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS  The driver binding and component name protocols were
                       installed.
  @retval other        EfiLibInstallDriverBindingComponentName2() failed.

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
