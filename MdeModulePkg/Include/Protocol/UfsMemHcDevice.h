/** @file
  EDKII UFS Memory-Mapped Host Controller Device Protocol.

  This protocol is produced by a thin platform (vendor) driver to describe a
  memory-mapped UFS Host Controller Instance to the generic UfsMemHcDxe driver.
  UfsMemHcDxe binds to this protocol and produces
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL on the same handle.

  This is the MMIO-based counterpart of EFI_PCI_IO_PROTOCOL used by
  UfsPciHcDxe.  The vendor driver sets HciMmioBase and HciMmioLength and
  installs this protocol; no function pointers are required.

  Copyright (c) 2024, EDK2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef EDKII_UFS_MEM_HC_DEVICE_H_
#define EDKII_UFS_MEM_HC_DEVICE_H_

///
/// EDKII_UFS_MEM_HC_DEVICE_PROTOCOL_GUID
///
#define EDKII_UFS_MEM_HC_DEVICE_PROTOCOL_GUID \
  { \
    0x2b335d62, 0x2e5d, 0x4a9f, { 0xb5, 0x3c, 0x1e, 0x44, 0x7a, 0xce, 0x81, 0xdd } \
  }

///
/// Protocol structure.  The vendor driver fills these fields and installs the
/// protocol on a new handle before UfsMemHcDxe binds.
///
typedef struct {
  ///
  /// Physical base address of the UFSHCI-compliant register block.
  ///
  EFI_PHYSICAL_ADDRESS    HciMmioBase;

  ///
  /// Byte length of the MMIO window.  UfsMemHcDxe will claim this range via
  /// the GCD so that no other driver can overlap it.
  ///
  UINTN                   HciMmioLength;
} EDKII_UFS_MEM_HC_DEVICE_PROTOCOL;

extern EFI_GUID  gEdkiiUfsMemHcDeviceProtocolGuid;

#endif // EDKII_UFS_MEM_HC_DEVICE_H_
