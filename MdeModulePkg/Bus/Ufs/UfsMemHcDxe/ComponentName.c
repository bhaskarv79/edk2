/** @file
  Component Name protocol implementation for UfsMemHcDxe.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>

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
  { "eng;en", L"UFS Memory-Mapped Host Controller Driver" },
  { NULL,     NULL                                        }
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mUfsMemHcControllerNameTable[] = {
  { "eng;en", L"UFS Memory-Mapped Host Controller" },
  { NULL,     NULL                                 }
};

/**
  Retrieves a Unicode string that is the user readable name of the driver.

  @param[in]   This        A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                           EFI_COMPONENT_NAME_PROTOCOL instance.
  @param[in]   Language    A pointer to a Null-terminated ASCII string array
                           indicating the language.
  @param[out]  DriverName  A pointer to the Unicode string to return.

  @retval EFI_SUCCESS            The Unicode string for the Driver specified by
                                 This and the language specified by Language
                                 was returned in DriverName.
  @retval EFI_INVALID_PARAMETER  Language or DriverName is NULL.
  @retval EFI_UNSUPPORTED        The driver specified by This does not support
                                 the language specified by Language.

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
  Retrieves a Unicode string that is the user readable name of the controller
  that is being managed by a driver.

  @param[in]   This              A pointer to the EFI_COMPONENT_NAME2_PROTOCOL or
                                 EFI_COMPONENT_NAME_PROTOCOL instance.
  @param[in]   ControllerHandle  The handle of a controller that the driver
                                 specified by This is managing.
  @param[in]   ChildHandle       The handle of the child controller. Must be NULL
                                 for this device driver.
  @param[in]   Language          A pointer to a Null-terminated ASCII string
                                 array indicating the language.
  @param[out]  ControllerName    A pointer to the Unicode string to return.

  @retval EFI_SUCCESS            The Unicode string for the controller was
                                 returned in ControllerName.
  @retval EFI_INVALID_PARAMETER  Language or ControllerName is NULL.
  @retval EFI_UNSUPPORTED        ChildHandle is not NULL, the driver is not
                                 managing ControllerHandle, or the language is
                                 not supported.

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
             &gEdkiiNonDiscoverableDeviceProtocolGuid
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
