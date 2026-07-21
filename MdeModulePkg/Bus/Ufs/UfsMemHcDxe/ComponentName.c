/** @file
  Component Name protocol implementation for UfsMemHcDxe.

  Copyright (c) 2024, EDK2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsMemHcDxe.h"

GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME_PROTOCOL  gUfsMemHcComponentName = {
  UfsMemHcComponentNameGetDriverName,
  UfsMemHcComponentNameGetControllerName,
  "eng"
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME2_PROTOCOL  gUfsMemHcComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)UfsMemHcComponentNameGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)UfsMemHcComponentNameGetControllerName,
  "en"
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mUfsMemHcDriverNameTable[] = {
  {
    "eng;en",
    L"Universal Flash Storage (UFS) Memory-Mapped Host Controller Driver"
  },
  {
    NULL,
    NULL
  }
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mUfsMemHcControllerNameTable[] = {
  {
    "eng;en",
    L"Universal Flash Storage (UFS) Memory-Mapped Host Controller"
  },
  {
    NULL,
    NULL
  }
};

/**
  Retrieves the user-readable driver name string.

  @param[in]  This        ComponentName(2) protocol instance.
  @param[in]  Language    RFC 4646 language code.
  @param[out] DriverName  Receives the Unicode name string.

  @retval EFI_SUCCESS            Name returned.
  @retval EFI_INVALID_PARAMETER  Language or DriverName is NULL.
  @retval EFI_UNSUPPORTED        Language not supported.

**/
EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mUfsMemHcDriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gUfsMemHcComponentName)
           );
}

/**
  Retrieves the user-readable controller name string.

  @param[in]  This              ComponentName(2) protocol instance.
  @param[in]  ControllerHandle  Handle of the managed controller.
  @param[in]  ChildHandle       Must be NULL (device driver).
  @param[in]  Language          RFC 4646 language code.
  @param[out] ControllerName    Receives the Unicode name string.

  @retval EFI_SUCCESS            Name returned.
  @retval EFI_INVALID_PARAMETER  Language or ControllerName is NULL.
  @retval EFI_UNSUPPORTED        ChildHandle not NULL, or driver not managing controller.

**/
EFI_STATUS
EFIAPI
UfsMemHcComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  EFI_STATUS  Status;

  if ((Language == NULL) || (ControllerName == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (ChildHandle != NULL) {
    return EFI_UNSUPPORTED;
  }

  Status = EfiTestManagedDevice (
             ControllerHandle,
             gUfsMemHcDriverBinding.DriverBindingHandle,
             &gEdkiiUfsMemHcDeviceProtocolGuid
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mUfsMemHcControllerNameTable,
           ControllerName,
           (BOOLEAN)(This == &gUfsMemHcComponentName)
           );
}
