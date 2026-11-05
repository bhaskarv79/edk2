/** @file

  Provides some data structure definitions used by the SD/MMC host controller driver.

  Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
  Copyright (c) 2015, Intel Corporation. All rights reserved.<BR>
  Copyright (C) 2023, Apple Inc. All rights reserved.<BR>
  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  @par Specification Reference:
    - SD Host Controller Simplified Specification, Version 4.20, July 25, 2018
**/

#pragma once

//
// A memory-mapped SD/MMC host controller has a single slot, whose register
// window is the controller's MMIO resource.
//
#define SD_MMC_HC_MAX_SLOT  1

//
// Size of the SDHCI standard register map (SD Host Controller Simplified
// Specification, Section 2.1), which the register window must cover.
//
#define SD_MMC_HC_MIN_MMIO_SIZE  0x100

//
// SD Host Controller MMIO Register Offset
//
#define SD_MMC_HC_SDMA_ADDR           0x00
#define SD_MMC_HC_ARG2                0x00
#define SD_MMC_HC_BLK_SIZE            0x04
#define SD_MMC_HC_BLK_COUNT           0x06
#define SD_MMC_HC_ARG1                0x08
#define SD_MMC_HC_TRANS_MOD           0x0C
#define SD_MMC_HC_COMMAND             0x0E
#define SD_MMC_HC_RESPONSE            0x10
#define SD_MMC_HC_BUF_DAT_PORT        0x20
#define SD_MMC_HC_PRESENT_STATE       0x24
#define SD_MMC_HC_HOST_CTRL1          0x28
#define SD_MMC_HC_POWER_CTRL          0x29
#define SD_MMC_HC_BLK_GAP_CTRL        0x2A
#define SD_MMC_HC_WAKEUP_CTRL         0x2B
#define SD_MMC_HC_CLOCK_CTRL          0x2C
#define SD_MMC_HC_TIMEOUT_CTRL        0x2E
#define SD_MMC_HC_SW_RST              0x2F
#define SD_MMC_HC_NOR_INT_STS         0x30
#define SD_MMC_HC_ERR_INT_STS         0x32
#define SD_MMC_HC_NOR_INT_STS_EN      0x34
#define SD_MMC_HC_ERR_INT_STS_EN      0x36
#define SD_MMC_HC_NOR_INT_SIG_EN      0x38
#define SD_MMC_HC_ERR_INT_SIG_EN      0x3A
#define SD_MMC_HC_AUTO_CMD_ERR_STS    0x3C
#define SD_MMC_HC_HOST_CTRL2          0x3E
#define SD_MMC_HC_CAP                 0x40
#define SD_MMC_HC_MAX_CURRENT_CAP     0x48
#define SD_MMC_HC_FORCE_EVT_AUTO_CMD  0x50
#define SD_MMC_HC_FORCE_EVT_ERR_INT   0x52
#define SD_MMC_HC_ADMA_ERR_STS        0x54
#define SD_MMC_HC_ADMA_SYS_ADDR       0x58
#define SD_MMC_HC_PRESET_VAL          0x60
#define SD_MMC_HC_SHARED_BUS_CTRL     0xE0
#define SD_MMC_HC_SLOT_INT_STS        0xFC
#define SD_MMC_HC_CTRL_VER            0xFE

//
// SD Host Controller bits to HOST_CTRL2 register
//
#define SD_MMC_HC_CTRL_UHS_MASK    0x0007
#define SD_MMC_HC_CTRL_UHS_SDR12   0x0000
#define SD_MMC_HC_CTRL_UHS_SDR25   0x0001
#define SD_MMC_HC_CTRL_UHS_SDR50   0x0002
#define SD_MMC_HC_CTRL_UHS_SDR104  0x0003
#define SD_MMC_HC_CTRL_UHS_DDR50   0x0004
#define SD_MMC_HC_CTRL_MMC_LEGACY  0x0000
#define SD_MMC_HC_CTRL_MMC_HS_SDR  0x0001
#define SD_MMC_HC_CTRL_MMC_HS_DDR  0x0004
#define SD_MMC_HC_CTRL_MMC_HS200   0x0003
#define SD_MMC_HC_CTRL_MMC_HS400   0x0005

#define SD_MMC_HC_CTRL_DRIVER_STRENGTH_MASK  0x0030

//
// The transfer modes supported by SD Host Controller
//
typedef enum {
  SdMmcNoData,
  SdMmcPioMode,
  SdMmcSdmaMode,
  SdMmcAdma32bMode,
  SdMmcAdma64bV3Mode,
  SdMmcAdma64bV4Mode
} SD_MMC_HC_TRANSFER_MODE;

//
// The ADMA transfer lengths supported by SD Host Controller
//
typedef enum {
  SdMmcAdmaLen16b,
  SdMmcAdmaLen26b
} SD_MMC_HC_ADMA_LENGTH_MODE;

//
// The maximum data length of each descriptor line
//
#define ADMA_MAX_DATA_PER_LINE_16B  SIZE_64KB
#define ADMA_MAX_DATA_PER_LINE_26B  SIZE_64MB

//
// ADMA descriptor for 32b addressing.
//
typedef struct {
  UINT32    Valid       : 1;
  UINT32    End         : 1;
  UINT32    Int         : 1;
  UINT32    Reserved    : 1;
  UINT32    Act         : 2;
  UINT32    UpperLength : 10;
  UINT32    LowerLength : 16;
  UINT32    Address;
} SD_MMC_HC_ADMA_32_DESC_LINE;

//
// ADMA descriptor for 64b addressing.
//
typedef struct {
  UINT32    Valid       : 1;
  UINT32    End         : 1;
  UINT32    Int         : 1;
  UINT32    Reserved    : 1;
  UINT32    Act         : 2;
  UINT32    UpperLength : 10;
  UINT32    LowerLength : 16;
  UINT32    LowerAddress;
  UINT32    UpperAddress;
} SD_MMC_HC_ADMA_64_V3_DESC_LINE;

typedef struct {
  UINT32    Valid       : 1;
  UINT32    End         : 1;
  UINT32    Int         : 1;
  UINT32    Reserved    : 1;
  UINT32    Act         : 2;
  UINT32    UpperLength : 10;
  UINT32    LowerLength : 16;
  UINT32    LowerAddress;
  UINT32    UpperAddress;
  UINT32    Reserved1;
} SD_MMC_HC_ADMA_64_V4_DESC_LINE;

#define SD_MMC_SDMA_BOUNDARY  512 * 1024
#define SD_MMC_SDMA_ROUND_UP(x, n)  (((x) + n) & ~(n - 1))

typedef struct {
  UINT32    TimeoutFreq   : 6; // bit 0:5
  UINT32    Reserved      : 1; // bit 6
  UINT32    TimeoutUnit   : 1; // bit 7
  UINT32    BaseClkFreq   : 8; // bit 8:15
  UINT32    MaxBlkLen     : 2; // bit 16:17
  UINT32    BusWidth8     : 1; // bit 18
  UINT32    Adma2         : 1; // bit 19
  UINT32    Reserved2     : 1; // bit 20
  UINT32    HighSpeed     : 1; // bit 21
  UINT32    Sdma          : 1; // bit 22
  UINT32    SuspRes       : 1; // bit 23
  UINT32    Voltage33     : 1; // bit 24
  UINT32    Voltage30     : 1; // bit 25
  UINT32    Voltage18     : 1; // bit 26
  UINT32    SysBus64V4    : 1; // bit 27
  UINT32    SysBus64V3    : 1; // bit 28
  UINT32    AsyncInt      : 1; // bit 29
  UINT32    SlotType      : 2; // bit 30:31
  UINT32    Sdr50         : 1; // bit 32
  UINT32    Sdr104        : 1; // bit 33
  UINT32    Ddr50         : 1; // bit 34
  UINT32    UhsII         : 1; // bit 35
  UINT32    DriverTypeA   : 1; // bit 36
  UINT32    DriverTypeC   : 1; // bit 37
  UINT32    DriverTypeD   : 1; // bit 38
  UINT32    DriverType4   : 1; // bit 39
  UINT32    TimerCount    : 4; // bit 40:43
  UINT32    Reserved4     : 1; // bit 44
  UINT32    TuningSDR50   : 1; // bit 45
  UINT32    RetuningMod   : 2; // bit 46:47
  UINT32    ClkMultiplier : 8; // bit 48:55
  UINT32    Reserved5     : 3; // bit 56:58
  UINT32    Adma3         : 1; // bit 59
  UINT32    Vdd2Voltage18 : 1; // bit 60
  UINT32    Reserved6     : 2; // bit 61:62
  UINT32    Hs400         : 1; // bit 63
} SD_MMC_HC_SLOT_CAP;

//
// SD Host controller version
//
#define SD_MMC_HC_CTRL_VER_100  0x00
#define SD_MMC_HC_CTRL_VER_200  0x01
#define SD_MMC_HC_CTRL_VER_300  0x02
#define SD_MMC_HC_CTRL_VER_400  0x03
#define SD_MMC_HC_CTRL_VER_410  0x04
#define SD_MMC_HC_CTRL_VER_420  0x05

//
// SD Host controller V4 enhancements
//
#define SD_MMC_HC_V4_EN                BIT12
#define SD_MMC_HC_64_ADDR_EN           BIT13
#define SD_MMC_HC_26_DATA_LEN_ADMA_EN  BIT10

///
/// Direction of a DMA mapping, as seen from the host controller.
///
typedef enum {
  ///
  /// The controller reads system memory (host to device).
  ///
  SdMmcHcIoOperationBusMasterRead,
  ///
  /// The controller writes system memory (device to host).
  ///
  SdMmcHcIoOperationBusMasterWrite,
  ///
  /// The controller and the CPU share the buffer, which must come from
  /// SdMmcHcIoAllocateBuffer().
  ///
  SdMmcHcIoOperationBusMasterCommonBuffer,
  SdMmcHcIoOperationMaximum
} SD_MMC_HC_IO_OPERATION;

///
/// Register window and DMA properties of a memory-mapped SD/MMC host
/// controller. It takes the place of EFI_PCI_IO_PROTOCOL in the code shared
/// with SdMmcPciHcDxe.
///
typedef struct {
  ///
  /// Base address and size of the SDHCI register window.
  ///
  UINTN      MmioBase;
  UINT64     MmioSize;
  ///
  /// TRUE if the controller's DMA is cache coherent with the CPU.
  ///
  BOOLEAN    DmaCoherent;
  ///
  /// TRUE if the controller can reach system memory above 4 GB.
  ///
  BOOLEAN    Dma64;
  ///
  /// CPU cache line size used for non-coherent DMA, in bytes.
  ///
  UINT32     DmaAlignment;
} SD_MMC_HC_IO;

///
/// Map token passed back through Mapping in SdMmcHcIoMap() and consumed by
/// SdMmcHcIoUnmap().
///
/// When a transfer cannot be handed to the controller in place, it is
/// bounced through BounceBuffer instead. That happens if the buffer is above
/// 4 GB on a controller without 64-bit addressing, or, on a non-coherent
/// platform, if a device-to-host buffer does not fill whole CPU cache lines.
/// Invalidating a partial line could otherwise discard dirty data belonging
/// to unrelated memory sharing that line.
///
typedef struct {
  VOID                      *HostAddress;
  UINTN                     NumberOfBytes;
  SD_MMC_HC_IO_OPERATION    Operation;
  VOID                      *BounceBuffer;
  UINTN                     BouncePages;
} SD_MMC_HC_MAP_INFO;

/**
  Provides the controller-specific address needed to access system memory.

  Common buffers must come from SdMmcHcIoAllocateBuffer() and are mapped in
  place. Bus master read and write buffers are mapped in place when the
  controller can reach them and, on a non-coherent platform, when cache
  maintenance on them cannot touch unrelated data. Otherwise they are bounced
  through a page-aligned buffer the controller can reach.

  @param[in]      HcIo           The host controller I/O context.
  @param[in]      Operation      The direction of the transfer.
  @param[in]      HostAddress    The system memory address to map.
  @param[in, out] NumberOfBytes  On input the number of bytes to map. On
                                 output the number of bytes that were mapped.
  @param[out]     DeviceAddress  The address the controller must use.
  @param[out]     Mapping        A value to pass to SdMmcHcIoUnmap().

  @retval EFI_SUCCESS            The range was mapped for NumberOfBytes.
  @retval EFI_UNSUPPORTED        HostAddress cannot be mapped as a common
                                 buffer.
  @retval EFI_INVALID_PARAMETER  One or more parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES   A bounce buffer could not be allocated.

**/
EFI_STATUS
SdMmcHcIoMap (
  IN     SD_MMC_HC_IO            *HcIo,
  IN     SD_MMC_HC_IO_OPERATION  Operation,
  IN     VOID                    *HostAddress,
  IN OUT UINTN                   *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS    *DeviceAddress,
  OUT    VOID                    **Mapping
  );

/**
  Completes a SdMmcHcIoMap() operation and releases its resources.

  @param[in]  HcIo     The host controller I/O context.
  @param[in]  Mapping  The value returned by SdMmcHcIoMap(). NULL for a
                       common buffer.

  @retval EFI_SUCCESS            The range was unmapped.
  @retval EFI_INVALID_PARAMETER  HcIo is NULL.

**/
EFI_STATUS
SdMmcHcIoUnmap (
  IN SD_MMC_HC_IO  *HcIo,
  IN VOID          *Mapping
  );

/**
  Allocates pages suitable for a SdMmcHcIoOperationBusMasterCommonBuffer
  mapping.

  The pages are placed where the controller can reach them and, if DMA is
  non-coherent, remapped uncached.

  @param[in]   HcIo         The host controller I/O context.
  @param[in]   Pages        The number of pages to allocate.
  @param[out]  HostAddress  The base address of the allocated range.

  @retval EFI_SUCCESS            The pages were allocated.
  @retval EFI_INVALID_PARAMETER  HcIo or HostAddress is NULL.
  @retval EFI_OUT_OF_RESOURCES   The pages could not be allocated, or could
                                 not be remapped uncached.

**/
EFI_STATUS
SdMmcHcIoAllocateBuffer (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINTN         Pages,
  OUT VOID          **HostAddress
  );

/**
  Frees pages allocated with SdMmcHcIoAllocateBuffer().

  @param[in]  HcIo         The host controller I/O context.
  @param[in]  Pages        The number of pages to free.
  @param[in]  HostAddress  The base address of the allocated range.

  @retval EFI_SUCCESS            The pages were freed.
  @retval EFI_INVALID_PARAMETER  HcIo or HostAddress is NULL.

**/
EFI_STATUS
SdMmcHcIoFreeBuffer (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINTN         Pages,
  IN VOID          *HostAddress
  );

/**
  Dump the content of SD/MMC host controller's Capability Register.

  @param[in]  Slot            The slot number of the SD card to send the command to.
  @param[in]  Capability      The buffer to store the capability data.

**/
VOID
DumpCapabilityReg (
  IN UINT8               Slot,
  IN SD_MMC_HC_SLOT_CAP  *Capability
  );

/**
  Read/Write specified SD/MMC host controller mmio register.

  @param[in]      HcIo        The host controller I/O context.
  @param[in]      BarIndex     The slot index, 0 for a memory-mapped
                               controller.
  @param[in]      Offset       The offset within the register window to
                               start the memory operation.
  @param[in]      Read         A boolean to indicate it's read or write operation.
  @param[in]      Count        The width of the mmio register in bytes.
                               Must be 1, 2 , 4 or 8 bytes.
  @param[in, out] Data         For read operations, the destination buffer to store
                               the results. For write operations, the source buffer
                               to write data from. The caller is responsible for
                               having ownership of the data buffer and ensuring its
                               size not less than Count bytes.

  @retval EFI_INVALID_PARAMETER The HcIo or Data is NULL or the Count is not valid.
  @retval EFI_SUCCESS           The read/write operation succeeds.
  @retval Others                The read/write operation fails.

**/
EFI_STATUS
EFIAPI
SdMmcHcRwMmio (
  IN     SD_MMC_HC_IO  *HcIo,
  IN     UINT8         BarIndex,
  IN     UINT32        Offset,
  IN     BOOLEAN       Read,
  IN     UINT8         Count,
  IN OUT VOID          *Data
  );

/**
  Do OR operation with the value of the specified SD/MMC host controller mmio register.

  @param[in] HcIo             The host controller I/O context.
  @param[in] BarIndex          The slot index, 0 for a memory-mapped
                               controller.
  @param[in] Offset            The offset within the register window to
                               start the memory operation.
  @param[in] Count             The width of the mmio register in bytes.
                               Must be 1, 2 , 4 or 8 bytes.
  @param[in] OrData            The pointer to the data used to do OR operation.
                               The caller is responsible for having ownership of
                               the data buffer and ensuring its size not less than
                               Count bytes.

  @retval EFI_INVALID_PARAMETER The HcIo or OrData is NULL or the Count is not valid.
  @retval EFI_SUCCESS           The OR operation succeeds.
  @retval Others                The OR operation fails.

**/
EFI_STATUS
EFIAPI
SdMmcHcOrMmio (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINT8         BarIndex,
  IN  UINT32        Offset,
  IN  UINT8         Count,
  IN  VOID          *OrData
  );

/**
  Do AND operation with the value of the specified SD/MMC host controller mmio register.

  @param[in] HcIo             The host controller I/O context.
  @param[in] BarIndex          The slot index, 0 for a memory-mapped
                               controller.
  @param[in] Offset            The offset within the register window to
                               start the memory operation.
  @param[in] Count             The width of the mmio register in bytes.
                               Must be 1, 2 , 4 or 8 bytes.
  @param[in] AndData           The pointer to the data used to do AND operation.
                               The caller is responsible for having ownership of
                               the data buffer and ensuring its size not less than
                               Count bytes.

  @retval EFI_INVALID_PARAMETER The HcIo or AndData is NULL or the Count is not valid.
  @retval EFI_SUCCESS           The AND operation succeeds.
  @retval Others                The AND operation fails.

**/
EFI_STATUS
EFIAPI
SdMmcHcAndMmio (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINT8         BarIndex,
  IN  UINT32        Offset,
  IN  UINT8         Count,
  IN  VOID          *AndData
  );

/**
  Wait for the value of the specified MMIO register set to the test value.

  @param[in]  HcIo         The host controller I/O context.
  @param[in]  BarIndex      The slot index, 0 for a memory-mapped
                            controller.
  @param[in]  Offset        The offset within the register window to
                            start the memory operation.
  @param[in]  Count         The width of the mmio register in bytes.
                            Must be 1, 2, 4 or 8 bytes.
  @param[in]  MaskValue     The mask value of memory.
  @param[in]  TestValue     The test value of memory.
  @param[in]  Timeout       The time out value for wait memory set, uses 1
                            microsecond as a unit.

  @retval EFI_TIMEOUT       The MMIO register hasn't expected value in timeout
                            range.
  @retval EFI_SUCCESS       The MMIO register has expected value.
  @retval Others            The MMIO operation fails.

**/
EFI_STATUS
EFIAPI
SdMmcHcWaitMmioSet (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINT8         BarIndex,
  IN  UINT32        Offset,
  IN  UINT8         Count,
  IN  UINT64        MaskValue,
  IN  UINT64        TestValue,
  IN  UINT64        Timeout
  );

/**
  Get the controller version information from the specified slot.

  @param[in]  HcIo           The host controller I/O context.
  @param[in]  Slot            The slot number of the SD card to send the command to.
  @param[out] Version         The buffer to store the version information.

  @retval EFI_SUCCESS         The operation executes successfully.
  @retval Others              The operation fails.

**/
EFI_STATUS
SdMmcHcGetControllerVersion (
  IN  SD_MMC_HC_IO  *HcIo,
  IN  UINT8         Slot,
  OUT UINT16        *Version
  );

/**
  Set all interrupt status bits in Normal and Error Interrupt Status Enable
  register.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.

  @retval EFI_SUCCESS       The operation executes successfully.
  @retval Others            The operation fails.

**/
EFI_STATUS
SdMmcHcEnableInterrupt (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot
  );

/**
  Get the capability data from the specified slot.

  @param[in]  HcIo           The host controller I/O context.
  @param[in]  Slot            The slot number of the SD card to send the command to.
  @param[out] Capability      The buffer to store the capability data.

  @retval EFI_SUCCESS         The operation executes successfully.
  @retval Others              The operation fails.

**/
EFI_STATUS
SdMmcHcGetCapability (
  IN     SD_MMC_HC_IO     *HcIo,
  IN     UINT8            Slot,
  OUT SD_MMC_HC_SLOT_CAP  *Capability
  );

/**
  Get the maximum current capability data from the specified slot.

  @param[in]  HcIo           The host controller I/O context.
  @param[in]  Slot            The slot number of the SD card to send the command to.
  @param[out] MaxCurrent      The buffer to store the maximum current capability data.

  @retval EFI_SUCCESS         The operation executes successfully.
  @retval Others              The operation fails.

**/
EFI_STATUS
SdMmcHcGetMaxCurrent (
  IN     SD_MMC_HC_IO  *HcIo,
  IN     UINT8         Slot,
  OUT UINT64           *MaxCurrent
  );

/**
  Detect whether there is a SD/MMC card attached at the specified SD/MMC host controller
  slot.

  Refer to SD Host Controller Simplified spec 3.0 Section 3.1 for details.

  @param[in]  HcIo         The host controller I/O context.
  @param[in]  Slot          The slot number of the SD card to send the command to.
  @param[out] MediaPresent  The pointer to the media present boolean value.

  @retval EFI_SUCCESS       There is no media change happened.
  @retval EFI_MEDIA_CHANGED There is media change happened.
  @retval Others            The detection fails.

**/
EFI_STATUS
SdMmcHcCardDetect (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot,
  OUT BOOLEAN      *MediaPresent
  );

/**
  Stop SD/MMC card clock.

  Refer to SD Host Controller Simplified spec 3.0 Section 3.2.2 for details.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.

  @retval EFI_SUCCESS       Succeed to stop SD/MMC clock.
  @retval Others            Fail to stop SD/MMC clock.

**/
EFI_STATUS
SdMmcHcStopClock (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot
  );

/**
  Start the SD clock.

  @param[in] HcIo  The host controller I/O context.
  @param[in] Slot   The slot number.

  @retval EFI_SUCCESS  Succeeded to start the SD clock.
  @retval Others       Failed to start the SD clock.
**/
EFI_STATUS
SdMmcHcStartSdClock (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot
  );

/**
  SD/MMC bus power control.

  Refer to SD Host Controller Simplified spec 3.0 Section 3.3 for details.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.
  @param[in] PowerCtrl      The value setting to the power control register.

  @retval TRUE              There is a SD/MMC card attached.
  @retval FALSE             There is no a SD/MMC card attached.

**/
EFI_STATUS
SdMmcHcPowerControl (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot,
  IN UINT8         PowerCtrl
  );

/**
  Set the SD/MMC bus width.

  Refer to SD Host Controller Simplified spec 3.0 Section 3.4 for details.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.
  @param[in] BusWidth       The bus width used by the SD/MMC device, it must be 1, 4 or 8.

  @retval EFI_SUCCESS       The bus width is set successfully.
  @retval Others            The bus width isn't set successfully.

**/
EFI_STATUS
SdMmcHcSetBusWidth (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot,
  IN UINT16        BusWidth
  );

/**
  Supply SD/MMC card with maximum voltage at initialization.

  Refer to SD Host Controller Simplified spec 3.0 Section 3.3 for details.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.
  @param[in] Capability     The capability of the slot.

  @retval EFI_SUCCESS       The voltage is supplied successfully.
  @retval Others            The voltage isn't supplied successfully.

**/
EFI_STATUS
SdMmcHcInitPowerVoltage (
  IN SD_MMC_HC_IO        *HcIo,
  IN UINT8               Slot,
  IN SD_MMC_HC_SLOT_CAP  Capability
  );

/**
  Initialize the Timeout Control register with most conservative value at initialization.

  Refer to SD Host Controller Simplified spec 3.0 Section 2.2.15 for details.

  @param[in] HcIo          The host controller I/O context.
  @param[in] Slot           The slot number of the SD card to send the command to.

  @retval EFI_SUCCESS       The timeout control register is configured successfully.
  @retval Others            The timeout control register isn't configured successfully.

**/
EFI_STATUS
SdMmcHcInitTimeoutCtrl (
  IN SD_MMC_HC_IO  *HcIo,
  IN UINT8         Slot
  );

/**
  Set SD Host Controller control 2 registry according to selected speed.

  @param[in] ControllerHandle The handle of the controller.
  @param[in] HcIo            The host controller I/O context.
  @param[in] Slot             The slot number of the SD card to send the command to.
  @param[in] Timing           The timing to select.

  @retval EFI_SUCCESS         The timing is set successfully.
  @retval Others              The timing isn't set successfully.
**/
EFI_STATUS
SdMmcHcUhsSignaling (
  IN EFI_HANDLE       ControllerHandle,
  IN SD_MMC_HC_IO     *HcIo,
  IN UINT8            Slot,
  IN SD_MMC_BUS_MODE  Timing
  );

/**
  Set driver strength in host controller.

  @param[in] HcIo           The host controller I/O context.
  @param[in] SlotIndex       The slot index of the card.
  @param[in] DriverStrength  DriverStrength to set in the controller.

  @retval EFI_SUCCESS  Driver strength programmed successfully.
  @retval Others       Failed to set driver strength.
**/
EFI_STATUS
SdMmcSetDriverStrength (
  IN SD_MMC_HC_IO             *HcIo,
  IN UINT8                    SlotIndex,
  IN SD_DRIVER_STRENGTH_TYPE  DriverStrength
  );
