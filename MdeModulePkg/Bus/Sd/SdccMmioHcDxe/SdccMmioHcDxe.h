/** @file
  Private types and definitions for the SdccMmioHcDxe generic SDHCI MMIO driver.

  This driver produces EFI_SD_MMC_PASS_THRU_PROTOCOL on any Qualcomm SDCC5
  SDHCI controller mapped as MMIO.  It supports both eMMC and SD card slots.
  Platform-specific values (clock frequencies, DLL tuning, GPIO config) are
  supplied via the SdccPlatformLib library class.

  Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __SDCC_MMIO_HC_DXE_H__
#define __SDCC_MMIO_HC_DXE_H__

#include <Uefi.h>
#include <Protocol/SdMmcPassThru.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/SdccPlatformLib.h>

// ─── SDHCI Standard Register Offsets ────────────────────────────────────────
// Ref: SD Host Controller Standard Specification v3.00 §2.2

#define SDCC_HC_SDMA_ADDR           0x00u
#define SDCC_HC_BLK_SIZE            0x04u
#define SDCC_HC_BLK_COUNT           0x06u
#define SDCC_HC_ARG1                0x08u
#define SDCC_HC_TRANS_MOD           0x0Cu
#define SDCC_HC_COMMAND             0x0Eu
#define SDCC_HC_RESPONSE            0x10u
#define SDCC_HC_BUF_DAT_PORT        0x20u
#define SDCC_HC_PRESENT_STATE       0x24u
#define SDCC_HC_HOST_CTRL1          0x28u
#define SDCC_HC_POWER_CTRL          0x29u
#define SDCC_HC_CLOCK_CTRL          0x2Cu
#define SDCC_HC_TIMEOUT_CTRL        0x2Eu
#define SDCC_HC_SW_RST              0x2Fu
#define SDCC_HC_NOR_INT_STS         0x30u
#define SDCC_HC_ERR_INT_STS         0x32u
#define SDCC_HC_NOR_INT_STS_EN      0x34u
#define SDCC_HC_ERR_INT_STS_EN      0x36u
#define SDCC_HC_HOST_CTRL2          0x3Eu
#define SDCC_HC_CAP                 0x40u
#define SDCC_HC_ADMA_ERR_STS        0x54u
#define SDCC_HC_ADMA_SYS_ADDR       0x58u
#define SDCC_HC_CTRL_VER            0xFEu

// ─── Qualcomm SDCC5 Vendor-Specific Register Offsets ─────────────────────────
// Ref: Qualcomm SDCC5 HPG / ipcat PERIPH_SS_SDC1_SDCC_SDCC5_HC

#define SDCC_HC_VS_DLL_CONFIG       0x200u  ///< DLL control; RST[30],PDN[29],DLL_EN[16],CK_OUT_EN[18]
#define SDCC_HC_VS_DLL_TEST_CTL     0x204u
#define SDCC_HC_VS_DLL_STATUS       0x208u  ///< DLL_LOCK[7], DDR_DLL_LOCK_JDR[11]
#define SDCC_HC_VS_FUNC             0x20Cu  ///< HC_SELECT_IN[21:19], HC_SELECT_IN_EN[18], PWRSAVE[1]
#define SDCC_HC_VS_FUNC2            0x210u  ///< AXI transaction attributes
#define SDCC_HC_VS_ADMA_ERR_ADDR0   0x214u
#define SDCC_HC_VS_CAPS0            0x21Cu  ///< Shadow CAP1: BaseClk[15:8], 8BIT[18], HS[21]
#define SDCC_HC_VS_CAPS1            0x220u  ///< Shadow CAP2: HS400[3], DDR50[2], SDR104[1]
#define SDCC_HC_VS_DDR200_CFG       0x224u  ///< CMD_IN_RCLK_EN[1]
#define SDCC_HC_PWRCTL_STATUS       0x240u  ///< BUS_OFF[0], BUS_ON[1]
#define SDCC_HC_PWRCTL_MASK         0x244u
#define SDCC_HC_PWRCTL_CLEAR        0x248u
#define SDCC_HC_PWRCTL_CTL          0x24Cu
#define SDCC_HC_VS_FUNC3            0x250u  ///< PWRSAVE_DLL[0]
#define SDCC_HC_VS_DLL_CONFIG2      0x254u  ///< DLL_CLOCK_DISABLE[21], MCLK_FREQ_CALC[17:10], DDR_CAL_EN[0]
#define SDCC_HC_VS_DLL_CONFIG3      0x258u
#define SDCC_HC_VS_DDR_CONFIG       0x25Cu  ///< PRG_RCLK_DLY[8:0]
#define SDCC_HC_VS_FUNC4            0x260u
#define SDCC_HC_VS_DLL_USER_CTL     0x388u  ///< Platform-specific DLL user control

// ─── HOST_CTRL1 (0x28) bits ──────────────────────────────────────────────────
#define SDCC_HC_CTRL1_LED             BIT0
#define SDCC_HC_CTRL1_BUS_WIDTH_4     BIT1
#define SDCC_HC_CTRL1_HIGH_SPEED      BIT2
#define SDCC_HC_CTRL1_DMA_SEL_ADMA    BIT4
#define SDCC_HC_CTRL1_DMA_SEL_ADMA64  (BIT4 | BIT3)
#define SDCC_HC_CTRL1_BUS_WIDTH_8     BIT5

// ─── HOST_CTRL2 (0x3E) bits ──────────────────────────────────────────────────
#define SDCC_HC_CTRL2_UHS_MASK        0x0007u
#define SDCC_HC_CTRL2_1P8V_EN         BIT3
#define SDCC_HC_CTRL2_V4_EN           BIT12
#define SDCC_HC_CTRL2_64_ADDR_EN      BIT13
#define SDCC_HC_CTRL2_26_DATA_LEN     BIT10

// ─── PRESENT_STATE (0x24) bits ───────────────────────────────────────────────
#define SDCC_HC_STATE_CMD_INHIBIT     BIT0
#define SDCC_HC_STATE_DAT_INHIBIT     BIT1
#define SDCC_HC_STATE_CARD_PRESENT    BIT16
#define SDCC_HC_STATE_CARD_STABLE     BIT17
#define SDCC_HC_STATE_DAT_0_3_MASK    0x00F00000u

// ─── SW_RST (0x2F) bits ──────────────────────────────────────────────────────
#define SDCC_HC_RST_ALL               0xFFu
#define SDCC_HC_RST_CMD_LINE          BIT1
#define SDCC_HC_RST_DAT_LINE          BIT2

// ─── NOR_INT_STS (0x30) bits ─────────────────────────────────────────────────
#define SDCC_HC_NOR_INT_CMD_COMPLETE  BIT0
#define SDCC_HC_NOR_INT_XFER_COMPLETE BIT1
#define SDCC_HC_NOR_INT_DMA_INT       BIT3
#define SDCC_HC_NOR_INT_BUF_RD_READY  BIT5
#define SDCC_HC_NOR_INT_ERR_INT       BIT15

// ─── POWER_CTRL (0x29) ───────────────────────────────────────────────────────
#define SDCC_HC_POWER_ON    BIT0
#define SDCC_HC_POWER_1P8V  (0x05u << 1)
#define SDCC_HC_POWER_3P0V  (0x06u << 1)
#define SDCC_HC_POWER_3P3V  (0x07u << 1)

// ─── CTRL_VER (0xFE) ─────────────────────────────────────────────────────────
#define SDCC_HC_CTRL_VER_100  0x00u
#define SDCC_HC_CTRL_VER_200  0x01u
#define SDCC_HC_CTRL_VER_300  0x02u
#define SDCC_HC_CTRL_VER_400  0x03u
#define SDCC_HC_CTRL_VER_410  0x04u
#define SDCC_HC_CTRL_VER_420  0x05u

// ─── Timing constants ────────────────────────────────────────────────────────
#define SDCC_HC_GENERIC_TIMEOUT_US    (3u * 1000000u)
#define SDCC_HC_RESET_TIMEOUT_US      50000u
#define SDCC_HC_CLK_STABLE_TIMEOUT_US 150000u
#define SDCC_HC_IDENTIFY_CLOCK_KHZ    400u
#define SDCC_HC_UHS1_SWITCH_DELAY_US  5000u
#define SDCC_TRB_RETRIES              5u
#define SDCC_ASYNC_TIMER_PERIOD       EFI_TIMER_PERIOD_MILLISECONDS(1)

// ─── ADMA2 64-bit descriptor (12 bytes, Qualcomm SDCC5 layout) ───────────────
// Field order: nADMAFlags(UINT16) | nDataSize(UINT16) | nDataAddress(UINT64)
// Bit layout in nADMAFlags: Valid=bit0, End=bit1, Int=bit2, Act=bits[5:4]
// Act=0b10 (TRAN) = 0x20 → combined with Valid: 0x21

#pragma pack (1)
typedef struct {
  UINT16    Valid     : 1;   ///< bit[0]
  UINT16    End       : 1;   ///< bit[1]
  UINT16    Int       : 1;   ///< bit[2]
  UINT16    Reserved1 : 1;   ///< bit[3]
  UINT16    Act       : 2;   ///< bit[5:4]  0b10 = Tran
  UINT16    Reserved2 : 10;  ///< bit[15:6]
  UINT16    Length;           ///< transfer length in bytes; 0 = 65536
  UINT32    LowerAddress;     ///< PA[31:0]
  UINT32    UpperAddress;     ///< PA[63:32]
} SDCC_ADMA2_64_DESC;
#pragma pack ()

#define SDCC_ADMA2_ACT_TRAN       2u
#define SDCC_ADMA2_MAX_DATA_16B   SIZE_64KB

//
// Maximum number of ADMA2 descriptors per transfer.
// SDHCI EmmcDxe MaxBlock = 0xFFFF blocks × 512 bytes = ~32 MB.
// Ceil(32 MB / 64 KB) = 512 entries exactly.
// Downstream Kodiak/Agatti UEFI uses 128 (sufficient for small bootloader shims).
// 512 is the minimum correct value for a driver that loads unified kernel images.
//
#define SDCC_ADMA_MAX_DESC_COUNT  512u

// ─── Per-instance capability cache ──────────────────────────────────────────

typedef union {
  UINT64    Raw;
  struct {
    UINT32    TimeoutClkFreq   : 6;   ///< CAP1[5:0]
    UINT32    Rsvd1            : 1;
    UINT32    TimeoutClkUnit   : 1;   ///< CAP1[7]
    UINT32    BaseClkFreq      : 8;   ///< CAP1[15:8]  in MHz
    UINT32    MaxBlkLen        : 2;   ///< CAP1[17:16]
    UINT32    BusWidth8        : 1;   ///< CAP1[18]
    UINT32    Adma2            : 1;   ///< CAP1[19] — NOT standard; SDCC5 maps ADMA here
    UINT32    Rsvd2            : 1;
    UINT32    HighSpeed        : 1;   ///< CAP1[21]
    UINT32    Sdma             : 1;   ///< CAP1[22]
    UINT32    SuspResume       : 1;   ///< CAP1[23]
    UINT32    Voltage33        : 1;   ///< CAP1[24]
    UINT32    Voltage30        : 1;   ///< CAP1[25]
    UINT32    Voltage18        : 1;   ///< CAP1[26]
    UINT32    Rsvd3            : 1;
    UINT32    SysBus64V3       : 1;   ///< CAP1[28]
    UINT32    AsyncInt         : 1;   ///< CAP1[29]
    UINT32    SlotType         : 2;   ///< CAP1[31:30]
    UINT32    Sdr50            : 1;   ///< CAP2[0]
    UINT32    Sdr104           : 1;   ///< CAP2[1]
    UINT32    Ddr50            : 1;   ///< CAP2[2]
    UINT32    Hs400            : 1;   ///< CAP2[3]
    UINT32    DrvTypeA         : 1;
    UINT32    DrvTypeC         : 1;
    UINT32    DrvTypeD         : 1;
    UINT32    Rsvd4            : 1;
    UINT32    RetuningTimer    : 4;   ///< CAP2[11:8]
    UINT32    SdrUseTuning     : 1;
    UINT32    RetuningModes    : 2;
    UINT32    ClkMult          : 8;
    UINT32    Rsvd5            : 5;
    UINT32    SysBus64V4       : 1;   ///< CAP2[27]
  };
} SDCC_SLOT_CAP;

// ─── TRB ─────────────────────────────────────────────────────────────────────

#define SDCC_TRB_SIGNATURE  SIGNATURE_32 ('S', 'T', 'R', 'B')

typedef struct _SDCC_HC_PRIVATE_DATA SDCC_HC_PRIVATE_DATA;

typedef struct {
  UINT32                                Signature;
  LIST_ENTRY                            TrbList;
  SDCC_HC_PRIVATE_DATA                 *Private;
  UINT8                                 Slot;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet;
  UINT16                                BlockSize;
  VOID                                 *Data;
  UINT32                                DataLen;
  BOOLEAN                               Read;
  EFI_PHYSICAL_ADDRESS                  DataPhy;
  BOOLEAN                               DataCacheFlushed;
  VOID                                 *AdmaDesc;
  EFI_PHYSICAL_ADDRESS                  AdmaDescPhy;
  UINTN                                 AdmaDescPages;
  EFI_EVENT                             Event;
  BOOLEAN                               Started;
  UINT64                                Timeout;
  UINT32                                Retries;
} SDCC_HC_TRB;

#define SDCC_TRB_FROM_LIST(a)  CR (a, SDCC_HC_TRB, TrbList, SDCC_TRB_SIGNATURE)

// ─── EFI_SDCC_HC_RCA_PROTOCOL ────────────────────────────────────────────────
//
// Installed by SdccMmioHcDxe alongside EFI_SD_MMC_PASS_THRU_PROTOCOL on the HC
// handle.  Communicates the pre-init assigned RCA to EmmcDxe, which starts its
// enumeration from CMD13 (device already in Transfer state) rather than CMD3.
// For HC drivers that do not pre-init (e.g. SdMmcPciHcDxe), the protocol is
// absent and EmmcDxe falls back to Slot+1.
//

#define EFI_SDCC_HC_RCA_PROTOCOL_GUID \
  { 0xB7C3D4E5, 0xF6A7, 0x48B9, \
    { 0xC0, 0xD1, 0xE2, 0xF3, 0xA4, 0xB5, 0xC6, 0xD7 } }

// gEfiSdccHcRcaProtocolGuid is resolved by AutoGen from the INF [Protocols] section.

typedef struct {
  UINT16    Rca;   ///< Relative Card Address assigned by host CMD3 (eMMC).
} EFI_SDCC_HC_RCA_PROTOCOL;

// ─── Per-instance private data ───────────────────────────────────────────────

#define SDCC_HC_PRIVATE_SIGNATURE  SIGNATURE_32 ('S', 'D', 'H', 'C')

struct _SDCC_HC_PRIVATE_DATA {
  UINT32                        Signature;
  EFI_HANDLE                    Handle;
  EFI_SD_MMC_PASS_THRU_PROTOCOL PassThru;
  EFI_SDCC_HC_RCA_PROTOCOL      RcaProtocol;     ///< installed on Handle; Rca set after CMD3
  SDCC5_PLATFORM_CONFIG         Config;          ///< from SdccGetPlatformConfig()
  SDCC_SLOT_CAP                 Capability;      ///< from CAPS registers
  UINT16                        ControllerVersion;
  BOOLEAN                       MediaPresent;
  LIST_ENTRY                    Queue;           ///< async TRB queue
  EFI_EVENT                     TimerEvent;      ///< 1 ms async drain
  EFI_EVENT                     ConnectEvent;    ///< SD hot-plug detection (TPL_CALLBACK)
  EFI_EVENT                     DeferredConnectEvent; ///< deferred ConnectController (TPL_APPLICATION)
  //
  // Per-instance ADMA2 descriptor table.
  //
  // Must NOT be a module-global: two live instances (SDC1 eMMC + SDC2 SD) would
  // share a single table, and one instance's async timer firing during the other's
  // active DMA transfer would ZeroMem the table mid-AXI-walk.  Embedding the table
  // here gives each instance its own allocation on the heap page returned by
  // AllocateZeroPool, which is Normal WB memory — identical to BSS and equally
  // safe for ADMA2 AXI accesses.
  //
  SDCC_ADMA2_64_DESC            AdmaDescTable[SDCC_ADMA_MAX_DESC_COUNT];
};

#define SDCC_HC_PRIVATE_FROM_PASSTHRU(a) \
  CR (a, SDCC_HC_PRIVATE_DATA, PassThru, SDCC_HC_PRIVATE_SIGNATURE)

// ─── EXT_CSD field offsets (JEDEC JESD84-B51) ────────────────────────────────
#define EMMC_EXT_CSD_SIZE             512u
#define EMMC_EXT_CSD_STROBE_SUPPORT   184u  ///< BIT0=ES supported
#define EMMC_EXT_CSD_BUS_WIDTH        183u  ///< target for CMD6 BUS_WIDTH
#define EMMC_EXT_CSD_HS_TIMING        185u  ///< target for CMD6 HS_TIMING
#define EMMC_EXT_CSD_DEVICE_TYPE      196u  ///< BIT1=HS52, BIT2=HS200-1.8V, BIT4=HS400-1.8V, BIT6=HS400-ES
#define EMMC_EXT_CSD_REV              192u
#define EMMC_EXT_CSD_SEC_COUNT        212u  ///< 4-byte little-endian sector count
#define EMMC_EXT_CSD_BOOT_SIZE_MULT   226u
#define EMMC_EXT_CSD_RPMB_SIZE_MULT   168u
#define EMMC_EXT_CSD_PARTITION_CFG    179u

// ─── CMD6 SWITCH argument helpers ────────────────────────────────────────────
// Format: Access[25:24] | Index[23:16] | Value[15:8] | CmdSet[2:0]
// Access=03 = Write Byte
#define EMMC_CMD6_WRITE_BYTE(idx, val)  \
  (0x03000000u | ((UINT32)(idx) << 16) | ((UINT32)(val) << 8))

#define EMMC_CMD6_HS_TIMING_HS200   EMMC_CMD6_WRITE_BYTE (185, 2)
#define EMMC_CMD6_HS_TIMING_HS      EMMC_CMD6_WRITE_BYTE (185, 1)
#define EMMC_CMD6_HS_TIMING_HS400   EMMC_CMD6_WRITE_BYTE (185, 3)
#define EMMC_CMD6_BUS_WIDTH_DDR8ES  EMMC_CMD6_WRITE_BYTE (183, 0x86) ///< DDR 8-bit + ES bit7
#define EMMC_CMD6_BUS_WIDTH_8BIT    EMMC_CMD6_WRITE_BYTE (183, 0x02) ///< 8-bit SDR

// ─── Function declarations (SdccMmioHci.c) ──────────────────────────────────

EFI_STATUS SdccHcMapMmio      (IN EFI_PHYSICAL_ADDRESS BaseAddress, IN UINTN Size);
EFI_STATUS SdccHcReset        (IN UINTN Base);
EFI_STATUS SdccHcEnableV4     (IN UINTN Base, IN UINT16 ControllerVersion, IN SDCC_SLOT_CAP Cap);
EFI_STATUS SdccHcClockSupply  (IN UINTN Base, IN UINT16 ControllerVersion,
                                IN UINT32 BaseClkFreqKHz, IN UINT32 TargetFreqKHz);
EFI_STATUS SdccHcInitPower    (IN UINTN Base, IN SDCC_SLOT_CAP Cap);
EFI_STATUS SdccHcInitTimeout  (IN UINTN Base);
EFI_STATUS SdccHcReadCapability (IN UINTN Base, OUT SDCC_SLOT_CAP *Cap);
EFI_STATUS SdccHcCardDetect   (IN UINTN Base, OUT BOOLEAN *Present);
EFI_STATUS SdccHcColdInit     (IN SDCC_HC_PRIVATE_DATA *Private);
EFI_STATUS SdccHcUhs1SignalSwitch (IN UINTN Base);

EFI_STATUS SdccCheckTrbEnv    (IN SDCC_HC_PRIVATE_DATA *Private, IN SDCC_HC_TRB *Trb);
EFI_STATUS SdccExecTrb        (IN SDCC_HC_PRIVATE_DATA *Private, IN SDCC_HC_TRB *Trb);
EFI_STATUS SdccCheckTrbResult (IN SDCC_HC_PRIVATE_DATA *Private, IN SDCC_HC_TRB *Trb);

EFI_STATUS SdccDmaMap    (IN SDCC_HC_TRB *Trb);
VOID       SdccDmaUnmap  (IN SDCC_HC_TRB *Trb);
EFI_STATUS SdccBuildAdmaDescTable (IN SDCC_HC_PRIVATE_DATA *Private, IN OUT SDCC_HC_TRB *Trb);
VOID       SdccFreeAdmaDescTable  (IN SDCC_HC_TRB *Trb);

EFI_STATUS SdccWaitSet8  (IN UINTN Base, IN UINT32 Off, IN UINT8  Mask, IN UINT8  Expected, IN UINT64 TimeoutUs);

EFI_STATUS SdccEmmcCmd (
  IN  SDCC_HC_PRIVATE_DATA         *Private,
  IN  UINT8                         CmdIdx,
  IN  EFI_SD_MMC_COMMAND_TYPE       CmdType,
  IN  EFI_SD_MMC_RESPONSE_TYPE      RespType,
  IN  UINT32                        Argument,
  IN  VOID                         *InData   OPTIONAL,
  IN  UINT32                        InLen,
  OUT EFI_SD_MMC_STATUS_BLOCK      *StsBlk,
  IN  UINT32                        TimeoutUs   ///< 0 = 3 000 000 µs (3 s)
  );

// ─── Function declarations (SdccMmioPassThru.c) ─────────────────────────────

SDCC_HC_TRB * SdccCreateTrb (IN SDCC_HC_PRIVATE_DATA *Private, IN UINT8 Slot,
                              IN EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
                              IN EFI_EVENT Event);
VOID          SdccFreeTrb   (IN SDCC_HC_TRB *Trb);

EFI_STATUS EFIAPI SdccPassThruPassThru        (IN EFI_SD_MMC_PASS_THRU_PROTOCOL *,
                                                IN UINT8, IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *,
                                                IN EFI_EVENT OPTIONAL);
EFI_STATUS EFIAPI SdccPassThruGetNextSlot     (IN EFI_SD_MMC_PASS_THRU_PROTOCOL *, IN OUT UINT8 *);
EFI_STATUS EFIAPI SdccPassThruBuildDevicePath (IN EFI_SD_MMC_PASS_THRU_PROTOCOL *, IN UINT8,
                                                IN OUT EFI_DEVICE_PATH_PROTOCOL **);
EFI_STATUS EFIAPI SdccPassThruGetSlotNumber   (IN EFI_SD_MMC_PASS_THRU_PROTOCOL *,
                                                IN EFI_DEVICE_PATH_PROTOCOL *, OUT UINT8 *);
EFI_STATUS EFIAPI SdccPassThruResetDevice     (IN EFI_SD_MMC_PASS_THRU_PROTOCOL *, IN UINT8);

VOID EFIAPI SdccProcessAsyncTaskList (IN EFI_EVENT Event, IN VOID *Context);
VOID EFIAPI SdccEnumerateDevice      (IN EFI_EVENT Event, IN VOID *Context);
VOID EFIAPI SdccDeferredConnect      (IN EFI_EVENT Event, IN VOID *Context);

// ─── Function declarations (SdccMmioEmmcMode.c) ─────────────────────────────

EFI_STATUS SdccNegotiateEmmcMode (IN SDCC_HC_PRIVATE_DATA *Private);
EFI_STATUS SdccReadExtCsd        (IN SDCC_HC_PRIVATE_DATA *Private,
                                   OUT UINT8 ExtCsdBuf[EMMC_EXT_CSD_SIZE]);

#endif // __SDCC_MMIO_HC_DXE_H__
