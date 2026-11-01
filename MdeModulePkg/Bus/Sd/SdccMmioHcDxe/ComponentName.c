/** @file
  EFI_COMPONENT_NAME_PROTOCOL and EFI_COMPONENT_NAME2_PROTOCOL for
  SdccMmioHcDxe.

  Provides human-readable driver and controller names, shown by the UEFI
  shell `drivers` and `devices` commands.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "SdccMmioHcDxe.h"

// ─── String tables ────────────────────────────────────────────────────────────

GLOBAL_REMOVE_IF_UNREFERENCED
EFI_UNICODE_STRING_TABLE  mSdccDriverNameTable[] = {
  { "eng;en", L"Qualcomm SDCC5 MMIO SDHCI Host Controller Driver" },
  { NULL,     NULL                                                  }
};

GLOBAL_REMOVE_IF_UNREFERENCED
EFI_UNICODE_STRING_TABLE  mSdccControllerNameTable[] = {
  { "eng;en", L"Qualcomm SDCC5 MMIO HC" },
  { NULL,     NULL                       }
};

// ─── EFI_COMPONENT_NAME_PROTOCOL callbacks ────────────────────────────────────

/**
  Retrieves a human-readable name of the driver.

  @param[in]   This         Component name protocol instance.
  @param[in]   Language     ISO 639-2 language code (e.g. "eng").
  @param[out]  DriverName   Pointer to the driver name string.

  @retval EFI_SUCCESS            Name was returned.
  @retval EFI_UNSUPPORTED        Language is not supported.
**/
STATIC
EFI_STATUS
EFIAPI
SdccComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                      **DriverName
  )
{
  return LookupUnicodeString (
           Language,
           gSdccMmioHcComponentName.SupportedLanguages,
           mSdccDriverNameTable,
           DriverName
           );
}

/**
  Retrieves a human-readable name of a controller managed by this driver.

  @param[in]   This               Component name protocol instance.
  @param[in]   ControllerHandle   Handle of the controller.
  @param[in]   ChildHandle        Child handle (NULL for the controller itself).
  @param[in]   Language           ISO 639-2 language code.
  @param[out]  ControllerName     Pointer to the controller name string.

  @retval EFI_SUCCESS             Name was returned.
  @retval EFI_UNSUPPORTED         Language is not supported or handle not managed.
**/
STATIC
EFI_STATUS
EFIAPI
SdccComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  EFI_HANDLE                    ControllerHandle,
  IN  EFI_HANDLE                    ChildHandle        OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                      **ControllerName
  )
{
  return LookupUnicodeString (
           Language,
           gSdccMmioHcComponentName.SupportedLanguages,
           mSdccControllerNameTable,
           ControllerName
           );
}

// ─── EFI_COMPONENT_NAME2_PROTOCOL callbacks ───────────────────────────────────

/**
  Retrieves a human-readable name of the driver (RFC 4646 language).

  @param[in]   This         Component name2 protocol instance.
  @param[in]   Language     RFC 4646 language code (e.g. "en").
  @param[out]  DriverName   Pointer to the driver name string.

  @retval EFI_SUCCESS            Name was returned.
  @retval EFI_UNSUPPORTED        Language is not supported.
**/
STATIC
EFI_STATUS
EFIAPI
SdccComponentName2GetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  CHAR8                         *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           gSdccMmioHcComponentName2.SupportedLanguages,
           mSdccDriverNameTable,
           DriverName,
           (BOOLEAN)(This != &gSdccMmioHcComponentName2)
           );
}

/**
  Retrieves a human-readable name of a controller (RFC 4646 language).

  @param[in]   This               Component name2 protocol instance.
  @param[in]   ControllerHandle   Handle of the controller.
  @param[in]   ChildHandle        Child handle (NULL for the controller itself).
  @param[in]   Language           RFC 4646 language code.
  @param[out]  ControllerName     Pointer to the controller name string.

  @retval EFI_SUCCESS             Name was returned.
  @retval EFI_UNSUPPORTED         Language is not supported or handle not managed.
**/
STATIC
EFI_STATUS
EFIAPI
SdccComponentName2GetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL  *This,
  IN  EFI_HANDLE                     ControllerHandle,
  IN  EFI_HANDLE                     ChildHandle        OPTIONAL,
  IN  CHAR8                         *Language,
  OUT CHAR16                        **ControllerName
  )
{
  return LookupUnicodeString2 (
           Language,
           gSdccMmioHcComponentName2.SupportedLanguages,
           mSdccControllerNameTable,
           ControllerName,
           (BOOLEAN)(This != &gSdccMmioHcComponentName2)
           );
}

// ─── Protocol instances ───────────────────────────────────────────────────────

EFI_COMPONENT_NAME_PROTOCOL  gSdccMmioHcComponentName = {
  SdccComponentNameGetDriverName,
  SdccComponentNameGetControllerName,
  "eng"
};

EFI_COMPONENT_NAME2_PROTOCOL  gSdccMmioHcComponentName2 = {
  SdccComponentName2GetDriverName,
  SdccComponentName2GetControllerName,
  "en"
};
