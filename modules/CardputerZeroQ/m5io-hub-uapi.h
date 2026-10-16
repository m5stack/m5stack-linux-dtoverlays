/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_M5IO_HUB_H
#define _UAPI_M5IO_HUB_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* CardputerQ STM32 IAP image: APP, CRC word, and persistent user pages. */
#define M5IO_HUB_OTA_START_ADDRESS 0x08002800U
#define M5IO_HUB_OTA_END_ADDRESS   0x08020000U
#define M5IO_HUB_OTA_PAGE_SIZE     0x0800U
#define M5IO_HUB_OTA_IMAGE_SIZE \
    (M5IO_HUB_OTA_END_ADDRESS - M5IO_HUB_OTA_START_ADDRESS)
#define M5IO_HUB_OTA_PAGE_COUNT \
    (M5IO_HUB_OTA_IMAGE_SIZE / M5IO_HUB_OTA_PAGE_SIZE)

#define M5IO_HUB_IOC_MAGIC 'M'

/*
 * image_ptr points to a complete M5IO-HUB/STM32 IAP image in the calling
 * process.  The ioctl is synchronous.  On return, completed_pages and the
 * response fields contain the last page/command acknowledged by the MCU.
 */
struct m5io_hub_ota_request {
    __u64 image_ptr;
    __u32 image_len;
    __u32 timeout_ms;
    __u32 completed_pages;
    __u32 response_status;
    __u32 response_address;
    __u16 response_length;
    __u8 response_command;
    __u8 reserved;
};

#define M5IO_HUB_IOCTL_OTA \
    _IOWR(M5IO_HUB_IOC_MAGIC, 0x01, struct m5io_hub_ota_request)

#endif /* _UAPI_M5IO_HUB_H */
