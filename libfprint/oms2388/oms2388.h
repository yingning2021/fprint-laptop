/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fpi-device.h"
#include "fpi-ssm.h"

G_DECLARE_FINAL_TYPE (FpiDeviceOms2388, fpi_device_oms2388,
                      FPI, DEVICE_OMS2388, FpDevice)

#define OMS2388_EP_OUT 0x01
#define OMS2388_EP_IN  0x81
#define OMS2388_INTERFACE 0
#define OMS2388_PACKET_SIZE 64
#define OMS2388_TEMPLATE_INDEX_BYTES 32
#define OMS2388_TEMPLATE_ID_COUNT (OMS2388_TEMPLATE_INDEX_BYTES * 8)
#define OMS2388_TEMPLATE_SLOTS 30
#define OMS2388_ENROLL_STAGES 6
#define OMS2388_ENROLL_ERROR_DUPLICATE 0x22

typedef void (*Oms2388CommandCallback) (FpiDeviceOms2388 *self,
                                        GError           *error);

struct _FpiDeviceOms2388
{
  FpDevice parent;

  FpiSsm *task_ssm;
  FpiSsm *command_ssm;
  Oms2388CommandCallback command_callback;

  gboolean command_has_write;
  gboolean command_wait_for_frame;
  guint8 empty_response_attempts;
  guint8 command_packet[OMS2388_PACKET_SIZE];
  guint8 response[OMS2388_PACKET_SIZE];
  gsize response_length;
  guint8 response_maximum;

  guint8 chip_serial[32];
  guint8 template_index[OMS2388_TEMPLATE_INDEX_BYTES];
  guint16 selected_slot;
  guint8 capture_count;
  guint8 enroll_errors[3];
  guint8 resync_attempts;
  gboolean cancel_requested;
  gboolean suspend_requested;
  gboolean suspended;
  gboolean duplicate_detected;
  gboolean enroll_restart_requested;
  gboolean interface_claimed;
  GSource *identify_poll_source;
  guint clear_cursor;
};
