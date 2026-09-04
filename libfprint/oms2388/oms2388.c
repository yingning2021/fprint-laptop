/*
 * Jiangxi OMS/O-Film 33a7:2388 Match-on-Chip fingerprint driver
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "oms2388"

#include "drivers_api.h"
#include "oms2388.h"

#include <string.h>

G_DEFINE_TYPE (FpiDeviceOms2388, fpi_device_oms2388, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .vid = 0x33a7, .pid = 0x2388 },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static GError *
oms2388_protocol_error (const char *message)
{
  return fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "%s", message);
}

static guint16
oms2388_checksum (const guint8 *data, gsize length)
{
  guint16 sum = 0;

  for (gsize i = 0; i < length; i++)
    sum = (guint16) (sum + data[i]);
  return sum;
}

static gboolean
oms2388_build_frame (const guint8 *payload,
                     gsize         payload_length,
                     guint8        output[OMS2388_PACKET_SIZE])
{
  guint16 length;
  guint16 checksum;

  if ((!payload && payload_length) || payload_length > 53)
    return FALSE;

  memset (output, 0, OMS2388_PACKET_SIZE);
  output[0] = 0xef;
  output[1] = 0x01;
  memset (output + 2, 0xff, 4);
  output[6] = 0x01;
  length = payload_length + 2;
  output[7] = length >> 8;
  output[8] = length;
  memcpy (output + 9, payload, payload_length);
  checksum = oms2388_checksum (output + 6, payload_length + 3);
  output[9 + payload_length] = checksum >> 8;
  output[10 + payload_length] = checksum;
  return TRUE;
}

static gboolean
oms2388_next_frame (FpiDeviceOms2388 *self,
                    gsize            *cursor,
                    const guint8    **payload,
                    gsize            *payload_length,
                    GError          **error)
{
  gsize offset = *cursor;
  guint16 encoded_length;
  gsize total;
  guint16 actual_checksum;
  guint16 expected_checksum;

  while (offset + 9 <= self->response_length &&
         !(self->response[offset] == 0xef && self->response[offset + 1] == 0x01))
    offset++;
  if (offset + 9 > self->response_length)
    return FALSE;

  encoded_length = ((guint16) self->response[offset + 7] << 8) |
                   self->response[offset + 8];
  total = 9 + encoded_length;
  if (encoded_length < 2 || offset + total > self->response_length)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "truncated OMS response frame");
      return FALSE;
    }
  if (self->response[offset + 6] != 0x07)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "unexpected OMS response direction");
      return FALSE;
    }

  actual_checksum = oms2388_checksum (self->response + offset + 6,
                                      total - 8);
  expected_checksum = ((guint16) self->response[offset + total - 2] << 8) |
                      self->response[offset + total - 1];
  if (actual_checksum != expected_checksum)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "invalid OMS response checksum");
      return FALSE;
    }

  *payload = self->response + offset + 9;
  *payload_length = encoded_length - 2;
  *cursor = offset + total;
  return TRUE;
}

static gboolean
oms2388_get_single_payload (FpiDeviceOms2388 *self,
                            const guint8    **payload,
                            gsize            *payload_length,
                            GError          **error)
{
  gsize cursor = 0;

  if (!oms2388_next_frame (self, &cursor, payload, payload_length, error))
    {
      if (!*error)
        g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                             "OMS response did not contain a frame");
      return FALSE;
    }
  return TRUE;
}

static void
oms2388_transfer_next_cb (FpiUsbTransfer *transfer,
                          FpDevice       *device,
                          gpointer        user_data,
                          GError         *error)
{
  if (error)
    fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&error));
  else
    fpi_ssm_next_state (transfer->ssm);
}

static void
oms2388_ack_cb (FpiUsbTransfer *transfer,
                FpDevice       *device,
                gpointer        user_data,
                GError         *error)
{
  static const guint8 acknowledgement[] = { 'U', 'S', 'B', 'S' };

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&error));
      return;
    }
  if (transfer->actual_length != sizeof acknowledgement ||
      memcmp (transfer->buffer, acknowledgement, sizeof acknowledgement) != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           oms2388_protocol_error ("invalid USBS acknowledgement"));
      return;
    }
  fpi_ssm_next_state (transfer->ssm);
}

static void
oms2388_response_cb (FpiUsbTransfer *transfer,
                     FpDevice       *device,
                     gpointer        user_data,
                     GError         *error)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&error));
      return;
    }
  self->response_length = MIN (transfer->actual_length, sizeof self->response);
  memcpy (self->response, transfer->buffer, self->response_length);
  fpi_ssm_next_state (transfer->ssm);
}

static void
oms2388_delay_done (FpDevice *device, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  fpi_ssm_next_state (ssm);
}

enum {
  COMMAND_WRITE_HEADER,
  COMMAND_WRITE_PACKET,
  COMMAND_WRITE_ACK,
  COMMAND_DELAY,
  COMMAND_READ_HEADER,
  COMMAND_READ_RESPONSE,
  COMMAND_READ_ACK,
  COMMAND_STATES,
};

static void
oms2388_command_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case COMMAND_WRITE_HEADER:
      if (!self->command_has_write)
        {
          fpi_ssm_jump_to_state (ssm, COMMAND_READ_HEADER);
          return;
        }
      {
        static const guint8 header[] = {
          'U', 'S', 'B', 'C', 0x00, OMS2388_PACKET_SIZE, 0x00, 0x00,
        };
        transfer = fpi_usb_transfer_new (device);
        fpi_usb_transfer_fill_bulk_full (transfer, OMS2388_EP_OUT,
                                         g_memdup2 (header, sizeof header),
                                         sizeof header, g_free);
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, 1500, NULL,
                                 oms2388_transfer_next_cb, NULL);
      }
      break;

    case COMMAND_WRITE_PACKET:
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk_full (transfer, OMS2388_EP_OUT,
                                       g_memdup2 (self->command_packet,
                                                  OMS2388_PACKET_SIZE),
                                       OMS2388_PACKET_SIZE, g_free);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, 1500, NULL,
                               oms2388_transfer_next_cb, NULL);
      break;

    case COMMAND_WRITE_ACK:
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk (transfer, OMS2388_EP_IN, 4);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_submit (transfer, 1500, NULL, oms2388_ack_cb, NULL);
      break;

    case COMMAND_DELAY:
      fpi_device_add_timeout (device, 50, oms2388_delay_done, ssm, NULL);
      break;

    case COMMAND_READ_HEADER:
      {
        const guint8 header[] = {
          'U', 'S', 'B', 'C', 0x80, self->response_maximum, 0x00, 0x00,
        };
        transfer = fpi_usb_transfer_new (device);
        fpi_usb_transfer_fill_bulk_full (transfer, OMS2388_EP_OUT,
                                         g_memdup2 (header, sizeof header),
                                         sizeof header, g_free);
        transfer->ssm = ssm;
        fpi_usb_transfer_submit (transfer, 1500, NULL,
                                 oms2388_transfer_next_cb, NULL);
      }
      break;

    case COMMAND_READ_RESPONSE:
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk (transfer, OMS2388_EP_IN,
                                  self->response_maximum);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, 1500, NULL,
                               oms2388_response_cb, NULL);
      break;

    case COMMAND_READ_ACK:
      transfer = fpi_usb_transfer_new (device);
      fpi_usb_transfer_fill_bulk (transfer, OMS2388_EP_IN, 4);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_submit (transfer, 1500, NULL, oms2388_ack_cb, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void oms2388_command_done (FpiSsm *ssm,
                                  FpDevice *device,
                                  GError *error);

static void
oms2388_command_begin (FpiDeviceOms2388 *self)
{
  self->command_ssm = fpi_ssm_new (FP_DEVICE (self),
                                   oms2388_command_run_state,
                                   COMMAND_STATES);
  fpi_ssm_start (self->command_ssm, oms2388_command_done);
}

static gboolean
oms2388_response_contains_frame (FpiDeviceOms2388 *self)
{
  for (gsize i = 0; i + 1 < self->response_length; i++)
    if (self->response[i] == 0xef && self->response[i + 1] == 0x01)
      return TRUE;
  return FALSE;
}

static void
oms2388_empty_response_retry (FpDevice *device, gpointer user_data)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);

  self->command_has_write = FALSE;
  oms2388_command_begin (self);
}

static void
oms2388_command_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  Oms2388CommandCallback callback = self->command_callback;

  self->command_ssm = NULL;
  if (!error && self->command_wait_for_frame &&
      !oms2388_response_contains_frame (self))
    {
      if (self->empty_response_attempts++ < 50)
        {
          fpi_device_add_timeout (device, 20, oms2388_empty_response_retry,
                                  NULL, NULL);
          return;
        }
      error = oms2388_protocol_error ("timed out waiting for OMS response frame");
    }
  self->command_callback = NULL;
  callback (self, error);
}

static void
oms2388_command_start (FpiDeviceOms2388      *self,
                       const guint8          *payload,
                       gsize                  payload_length,
                       guint8                 response_maximum,
                       Oms2388CommandCallback callback)
{
  g_assert (!self->command_ssm);
  g_assert (callback);

  if (!oms2388_build_frame (payload, payload_length, self->command_packet))
    {
      callback (self, oms2388_protocol_error ("could not build OMS command"));
      return;
    }

  self->command_has_write = TRUE;
  self->command_wait_for_frame = TRUE;
  self->empty_response_attempts = 0;
  self->response_maximum = response_maximum;
  self->response_length = 0;
  self->command_callback = callback;
  oms2388_command_begin (self);
}

static void
oms2388_response_start (FpiDeviceOms2388      *self,
                        guint8                 response_maximum,
                        gboolean               wait_for_frame,
                        Oms2388CommandCallback callback)
{
  g_assert (!self->command_ssm);
  self->command_has_write = FALSE;
  self->command_wait_for_frame = wait_for_frame;
  self->empty_response_attempts = 0;
  self->response_maximum = response_maximum;
  self->response_length = 0;
  self->command_callback = callback;
  oms2388_command_begin (self);
}

static void
oms2388_poll_start (FpiDeviceOms2388      *self,
                    Oms2388CommandCallback callback)
{
  oms2388_response_start (self, 32, FALSE, callback);
}

static gboolean
oms2388_parse_status (FpiDeviceOms2388 *self, GError **error)
{
  const guint8 *payload = NULL;
  gsize payload_length = 0;

  if (!oms2388_get_single_payload (self, &payload, &payload_length, error))
    return FALSE;
  if (payload_length < 1 || payload[0] != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                   "OMS operation failed with status 0x%02x",
                   payload_length ? payload[0] : 0xff);
      return FALSE;
    }
  return TRUE;
}

static gboolean
oms2388_parse_index (FpiDeviceOms2388 *self, GError **error)
{
  const guint8 *payload = NULL;
  gsize payload_length = 0;

  if (!oms2388_get_single_payload (self, &payload, &payload_length, error))
    return FALSE;
  if (payload_length < 33 || payload[0] != 0)
    {
      g_set_error_literal (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "invalid OMS template index response");
      return FALSE;
    }
  memcpy (self->template_index, payload + 1, OMS2388_TEMPLATE_INDEX_BYTES);
  return TRUE;
}

static void
oms2388_set_print_data (FpiDeviceOms2388 *self,
                        FpPrint          *print,
                        guint16           slot)
{
  GVariant *serial;
  GVariant *data;

  serial = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                      self->chip_serial,
                                      sizeof self->chip_serial, 1);
  data = g_variant_new ("(q@ay)", slot, serial);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
}

static gboolean
oms2388_get_print_slot (FpiDeviceOms2388 *self,
                        FpPrint          *print,
                        guint16          *slot)
{
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) serial = NULL;
  const guint8 *serial_data;
  gsize serial_length = 0;
  guint16 value = 0xffff;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_check_format_string (data, "(q@ay)", FALSE))
    return FALSE;
  g_variant_get (data, "(q@ay)", &value, &serial);
  serial_data = g_variant_get_fixed_array (serial, &serial_length, 1);
  if (value >= OMS2388_TEMPLATE_ID_COUNT ||
      serial_length != sizeof self->chip_serial ||
      memcmp (serial_data, self->chip_serial, serial_length) != 0)
    return FALSE;
  *slot = value;
  return TRUE;
}

static FpPrint *
oms2388_print_for_slot (FpiDeviceOms2388 *self, guint16 slot)
{
  g_autofree gchar *user_id = g_strdup_printf ("oms2388-slot-%u", slot);
  g_autofree gchar *description = g_strdup_printf ("OMS template %u", slot);
  FpPrint *print = fp_print_new (FP_DEVICE (self));

  oms2388_set_print_data (self, print, slot);
  fpi_print_fill_from_user_id (print, user_id);
  fp_print_set_description (print, description);
  return print;
}

static void
oms2388_task_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->task_ssm = NULL;
  self->cancel_requested = FALSE;
  self->duplicate_detected = FALSE;
  self->enroll_restart_requested = FALSE;
  if (error)
    fpi_device_action_error (device, g_steal_pointer (&error));
}

static void
oms2388_open_handshake_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  const guint8 *payload = NULL;
  gsize length = 0;

  if (!local_error &&
      (!oms2388_get_single_payload (self, &payload, &length, &local_error) ||
       length < 1 || payload[0] != 0))
    g_set_error_literal (&local_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                         "OMS handshake failed");
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
oms2388_open_serial_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  const guint8 *payload = NULL;
  gsize length = 0;

  if (!local_error &&
      !oms2388_get_single_payload (self, &payload, &length, &local_error))
    goto out;
  if (local_error)
    goto out;
  if (length >= 33 && payload[0] == 0)
    {
      memcpy (self->chip_serial, payload + 1, sizeof self->chip_serial);
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  if (self->resync_attempts++ < 8)
    {
      fp_warn ("discarding stale OMS response while opening device");
      oms2388_response_start (self, 64, TRUE, oms2388_open_serial_cb);
      return;
    }
  g_set_error_literal (&local_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "could not synchronize OMS chip serial response");

out:
  fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
}

enum {
  OPEN_HANDSHAKE,
  OPEN_READ_SERIAL,
  OPEN_STATES,
};

static void
oms2388_open_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_HANDSHAKE:
      {
        const guint8 payload[] = { 0x35 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_open_handshake_cb);
      }
      break;
    case OPEN_READ_SERIAL:
      {
        const guint8 payload[] = { 0x34, 0x00 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_open_serial_cb);
      }
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
oms2388_open_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->task_ssm = NULL;
  if (error && self->interface_claimed)
    {
      g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                      OMS2388_INTERFACE, 0, NULL);
      self->interface_claimed = FALSE;
    }
  fpi_device_open_complete (device, error);
}

static void
oms2388_open (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  g_autoptr(GError) error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device),
                                     OMS2388_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (device, g_steal_pointer (&error));
      return;
    }
  self->interface_claimed = TRUE;
  self->resync_attempts = 0;
  self->task_ssm = fpi_ssm_new (device, oms2388_open_run_state, OPEN_STATES);
  fpi_ssm_start (self->task_ssm, oms2388_open_done);
}

static void
oms2388_close_sleep_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error)
    oms2388_parse_status (self, &local_error);
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
oms2388_close_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  const guint8 payload[] = { 0x33 };
  oms2388_command_start (self, payload, sizeof payload, 64,
                         oms2388_close_sleep_cb);
}

static void
oms2388_close_done (FpiSsm *ssm, FpDevice *device, GError *error)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  g_autoptr(GError) release_error = NULL;

  self->task_ssm = NULL;
  if (self->interface_claimed &&
      !g_usb_device_release_interface (fpi_device_get_usb_device (device),
                                       OMS2388_INTERFACE, 0, &release_error) &&
      !error)
    error = g_steal_pointer (&release_error);
  self->interface_claimed = FALSE;
  fpi_device_close_complete (device, error);
}

static void
oms2388_close (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->task_ssm = fpi_ssm_new (device, oms2388_close_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms2388_close_done);
}

static void
oms2388_list_index_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  GPtrArray *prints;

  if (!local_error)
    oms2388_parse_index (self, &local_error);
  if (local_error)
    {
      fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
      return;
    }

  prints = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < OMS2388_TEMPLATE_ID_COUNT; i++)
    if (self->template_index[i / 8] & (1U << (i % 8)))
      g_ptr_array_add (prints, oms2388_print_for_slot (self, i));
  fpi_device_list_complete (FP_DEVICE (self), prints, NULL);
  fpi_ssm_mark_completed (self->task_ssm);
}

static void
oms2388_list_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  const guint8 payload[] = { 0x1f, 0x00 };
  oms2388_command_start (self, payload, sizeof payload, 64,
                         oms2388_list_index_cb);
}

static void
oms2388_list (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->task_ssm = fpi_ssm_new (device, oms2388_list_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms2388_task_done);
}

static gboolean
oms2388_process_identify (FpiDeviceOms2388 *self, GError **error)
{
  gsize cursor = 0;
  const guint8 *payload;
  gsize length;

  while (oms2388_next_frame (self, &cursor, &payload, &length, error))
    {
      if (length >= 4 && payload[1] == 0x05)
        {
          guint16 slot = ((guint16) payload[2] << 8) | payload[3];
          FpDevice *device = FP_DEVICE (self);
          FpPrint *scanned = NULL;
          FpPrint *matched = NULL;
          GPtrArray *prints;

          if (payload[0] == 0 && slot < OMS2388_TEMPLATE_ID_COUNT)
            {
              scanned = oms2388_print_for_slot (self, slot);
              fpi_device_get_identify_data (device, &prints);
              for (guint i = 0; i < prints->len; i++)
                {
                  guint16 candidate;
                  if (oms2388_get_print_slot (self,
                                              g_ptr_array_index (prints, i),
                                              &candidate) && candidate == slot)
                    {
                      matched = g_ptr_array_index (prints, i);
                      break;
                    }
                }
            }
          fpi_device_identify_report (device, matched, scanned, NULL);
          fpi_device_identify_complete (device, NULL);
          fpi_ssm_mark_completed (self->task_ssm);
          return TRUE;
        }
    }
  return *error != NULL;
}

static void oms2388_identify_poll_cb (FpiDeviceOms2388 *self, GError *error);

static void
oms2388_identify_suspend_complete (FpiDeviceOms2388 *self)
{
  if (!self->suspend_requested || self->suspended || self->command_ssm)
    return;

  g_clear_pointer (&self->identify_poll_source, g_source_destroy);
  self->suspended = TRUE;
  fpi_device_suspend_complete (FP_DEVICE (self), NULL);
}

static void
oms2388_identify_fail (FpiDeviceOms2388 *self, GError *error)
{
  if (self->suspend_requested && !self->suspended)
    {
      self->suspend_requested = FALSE;
      fpi_device_suspend_complete (FP_DEVICE (self), g_error_copy (error));
    }
  fpi_ssm_mark_failed (self->task_ssm, error);
}

static void
oms2388_identify_start_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error && oms2388_process_identify (self, &local_error))
    {
      oms2388_identify_suspend_complete (self);
      return;
    }
  if (local_error)
    oms2388_identify_fail (self, g_steal_pointer (&local_error));
  else if (self->suspend_requested)
    oms2388_identify_suspend_complete (self);
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
oms2388_poll_delay_done (FpDevice *device, gpointer user_data)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);

  self->identify_poll_source = NULL;
  if (self->cancel_requested)
    fpi_ssm_next_state (self->task_ssm);
  else if (self->suspend_requested)
    oms2388_identify_suspend_complete (self);
  else
    oms2388_poll_start (self, oms2388_identify_poll_cb);
}

static void
oms2388_identify_poll_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error && oms2388_process_identify (self, &local_error))
    {
      oms2388_identify_suspend_complete (self);
      return;
    }
  if (local_error)
    oms2388_identify_fail (self, g_steal_pointer (&local_error));
  else if (self->cancel_requested)
    fpi_ssm_next_state (self->task_ssm);
  else if (self->suspend_requested)
    oms2388_identify_suspend_complete (self);
  else
    fpi_ssm_jump_to_state (self->task_ssm, 1);
}

static void
oms2388_cancel_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  const guint8 *payload = NULL;
  gsize length = 0;

  if (!local_error &&
      !oms2388_get_single_payload (self, &payload, &length, &local_error))
    goto out;
  if (local_error)
    goto out;
  if (length == 1)
    {
      if (payload[0] != 0)
        g_set_error (&local_error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                     "OMS cancel failed with status 0x%02x", payload[0]);
      else if (self->duplicate_detected)
        local_error = fpi_device_error_new (FP_DEVICE_ERROR_DATA_DUPLICATE);
      else
        local_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                           "fingerprint operation cancelled");
      goto out;
    }

  if (self->resync_attempts++ < 8)
    {
      fp_warn ("discarding pending OMS operation response while cancelling");
      oms2388_response_start (self, 64, TRUE, oms2388_cancel_cb);
      return;
    }
  local_error = oms2388_protocol_error ("could not synchronize OMS cancel response");

out:
  fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
}

static void
oms2388_identify_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case 0:
      {
        const guint8 payload[] = { 0x32, 0x05, 0xff, 0xff, 0x00, 0x00 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_identify_start_cb);
      }
      break;
    case 1:
      self->identify_poll_source =
        fpi_device_add_timeout (device, 20, oms2388_poll_delay_done, NULL, NULL);
      break;
    case 2:
      {
        const guint8 payload[] = { 0x30 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_cancel_cb);
      }
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
oms2388_identify (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->cancel_requested = FALSE;
  self->suspend_requested = FALSE;
  self->suspended = FALSE;
  self->resync_attempts = 0;
  self->task_ssm = fpi_ssm_new (device, oms2388_identify_run_state, 3);
  fpi_ssm_start (self->task_ssm, oms2388_task_done);
}

static gboolean
oms2388_process_enroll (FpiDeviceOms2388 *self, GError **error)
{
  gsize cursor = 0;
  const guint8 *payload;
  gsize length;

  while (oms2388_next_frame (self, &cursor, &payload, &length, error))
    {
      if (length < 2)
        continue;
      switch (payload[1])
        {
        case 0x01:
        case 0x04:
          self->enroll_errors[0] = payload[0];
          if (payload[0] == OMS2388_ENROLL_ERROR_DUPLICATE)
            {
              self->duplicate_detected = TRUE;
              fpi_ssm_jump_to_state (self->task_ssm, 4);
              return TRUE;
            }
          break;
        case 0x02:
        case 0x05:
          self->enroll_errors[1] = payload[0];
          break;
        case 0x03:
          if (length >= 3)
            {
              self->capture_count = payload[2];
              fpi_device_enroll_progress (FP_DEVICE (self),
                                          MIN (self->capture_count,
                                               OMS2388_ENROLL_STAGES),
                                          NULL, NULL);
              if (self->capture_count < OMS2388_ENROLL_STAGES)
                self->enroll_restart_requested = TRUE;
            }
          break;
        case 0x06:
          {
            FpPrint *print;
            self->enroll_errors[2] = payload[0];
            if (self->enroll_errors[0] || self->enroll_errors[1] ||
                self->enroll_errors[2])
              {
                g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                             "OMS enrollment failed (%02x %02x %02x)",
                             self->enroll_errors[0], self->enroll_errors[1],
                             self->enroll_errors[2]);
                return TRUE;
              }
            fpi_device_get_enroll_data (FP_DEVICE (self), &print);
            oms2388_set_print_data (self, print, self->selected_slot);
            fpi_device_enroll_complete (FP_DEVICE (self),
                                        g_object_ref (print), NULL);
            fpi_ssm_mark_completed (self->task_ssm);
            return TRUE;
          }
        default:
          break;
        }
    }
  return *error != NULL;
}

static void oms2388_enroll_poll_cb (FpiDeviceOms2388 *self, GError *error);

static void
oms2388_enroll_index_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error)
    oms2388_parse_index (self, &local_error);
  if (!local_error)
    {
      self->selected_slot = 0xffff;
      for (guint i = 0; i < OMS2388_TEMPLATE_SLOTS; i++)
        if (!(self->template_index[i / 8] & (1U << (i % 8))))
          {
            self->selected_slot = i;
            break;
          }
      if (self->selected_slot == 0xffff)
        local_error = fpi_device_error_new (FP_DEVICE_ERROR_DATA_FULL);
    }
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
oms2388_enroll_start_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error && oms2388_process_enroll (self, &local_error))
    {
      if (local_error)
        fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
      return;
    }
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    fpi_ssm_next_state (self->task_ssm);
}

static void
oms2388_enroll_poll_delay_done (FpDevice *device, gpointer user_data)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  if (self->cancel_requested)
    fpi_ssm_next_state (self->task_ssm);
  else if (self->enroll_restart_requested)
    {
      self->enroll_restart_requested = FALSE;
      fpi_ssm_jump_to_state (self->task_ssm, 2);
    }
  else
    oms2388_poll_start (self, oms2388_enroll_poll_cb);
}

static void
oms2388_enroll_poll_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error && oms2388_process_enroll (self, &local_error))
    {
      if (local_error)
        fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
      return;
    }
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else if (self->cancel_requested)
    fpi_ssm_next_state (self->task_ssm);
  else
    fpi_ssm_jump_to_state (self->task_ssm, 3);
}

static void
oms2388_enroll_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case 0:
      /* The Windows driver waits 500 ms after stopping USB idle before it
       * reads the free slot and starts command 0x31. */
      fpi_device_add_timeout (device, 500, oms2388_delay_done, ssm, NULL);
      break;
    case 1:
      {
        const guint8 payload[] = { 0x1f, 0x00 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_enroll_index_cb);
      }
      break;
    case 2:
      {
        const guint8 payload[] = {
          0x31, self->selected_slot >> 8, self->selected_slot,
          0x06, 0x00, 0x00,
        };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_enroll_start_cb);
      }
      break;
    case 3:
      fpi_device_add_timeout (device, 20, oms2388_enroll_poll_delay_done,
                              NULL, NULL);
      break;
    case 4:
      {
        const guint8 payload[] = { 0x30 };
        oms2388_command_start (self, payload, sizeof payload, 64,
                               oms2388_cancel_cb);
      }
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
oms2388_enroll (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->cancel_requested = FALSE;
  self->duplicate_detected = FALSE;
  self->enroll_restart_requested = FALSE;
  self->capture_count = 0;
  self->resync_attempts = 0;
  memset (self->enroll_errors, 0, sizeof self->enroll_errors);
  self->task_ssm = fpi_ssm_new (device, oms2388_enroll_run_state, 5);
  fpi_ssm_start (self->task_ssm, oms2388_task_done);
}

static void
oms2388_delete_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error)
    oms2388_parse_status (self, &local_error);
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    {
      fpi_device_delete_complete (FP_DEVICE (self), NULL);
      fpi_ssm_mark_completed (self->task_ssm);
    }
}

static void
oms2388_delete_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  const guint8 payload[] = {
    0x0c, self->selected_slot >> 8, self->selected_slot, 0x00, 0x01,
  };
  oms2388_command_start (self, payload, sizeof payload, 64,
                         oms2388_delete_cb);
}

static void
oms2388_delete (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  FpPrint *print;

  fpi_device_get_delete_data (device, &print);
  if (!oms2388_get_print_slot (self, print, &self->selected_slot))
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }
  self->task_ssm = fpi_ssm_new (device, oms2388_delete_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms2388_task_done);
}

static void oms2388_clear_delete_next (FpiDeviceOms2388 *self);

static void
oms2388_clear_delete_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error)
    oms2388_parse_status (self, &local_error);
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    oms2388_clear_delete_next (self);
}

static void
oms2388_clear_delete_next (FpiDeviceOms2388 *self)
{
  while (self->clear_cursor < OMS2388_TEMPLATE_ID_COUNT &&
         !(self->template_index[self->clear_cursor / 8] &
           (1U << (self->clear_cursor % 8))))
    self->clear_cursor++;
  if (self->clear_cursor == OMS2388_TEMPLATE_ID_COUNT)
    {
      fpi_device_clear_storage_complete (FP_DEVICE (self), NULL);
      fpi_ssm_mark_completed (self->task_ssm);
      return;
    }

  self->selected_slot = self->clear_cursor++;
  const guint8 payload[] = {
    0x0c, self->selected_slot >> 8, self->selected_slot, 0x00, 0x01,
  };
  oms2388_command_start (self, payload, sizeof payload, 64,
                         oms2388_clear_delete_cb);
}

static void
oms2388_clear_index_cb (FpiDeviceOms2388 *self, GError *error)
{
  g_autoptr(GError) local_error = error;
  if (!local_error)
    oms2388_parse_index (self, &local_error);
  if (local_error)
    fpi_ssm_mark_failed (self->task_ssm, g_steal_pointer (&local_error));
  else
    {
      self->clear_cursor = 0;
      oms2388_clear_delete_next (self);
    }
}

static void
oms2388_clear_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  const guint8 payload[] = { 0x1f, 0x00 };
  oms2388_command_start (self, payload, sizeof payload, 64,
                         oms2388_clear_index_cb);
}

static void
oms2388_clear_storage (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->task_ssm = fpi_ssm_new (device, oms2388_clear_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms2388_task_done);
}

static void
oms2388_cancel (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  self->cancel_requested = TRUE;
}

static void
oms2388_suspend (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);
  FpiDeviceAction action = fpi_device_get_current_action (device);

  if (action != FPI_DEVICE_ACTION_IDENTIFY &&
      action != FPI_DEVICE_ACTION_VERIFY)
    {
      fpi_device_suspend_complete (
        device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  /* Command 0x32 has already armed the sensor's autonomous identify path.
   * Stop host polling before D3; the firmware keeps matching and retains the
   * completion frame until the USB link resumes.  This mirrors the vendor
   * driver's OMSAutoIdentify path. */
  self->suspend_requested = TRUE;
  oms2388_identify_suspend_complete (self);
}

static void
oms2388_resume (FpDevice *device)
{
  FpiDeviceOms2388 *self = FPI_DEVICE_OMS2388 (device);

  if (!self->suspended || !self->suspend_requested || !self->task_ssm)
    {
      fpi_device_resume_complete (
        device, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
      return;
    }

  self->suspended = FALSE;
  self->suspend_requested = FALSE;
  if (self->cancel_requested)
    fpi_ssm_jump_to_state (self->task_ssm, 2);
  else
    fpi_ssm_jump_to_state (self->task_ssm, 1);
  fpi_device_resume_complete (device, NULL);
}

static void
oms2388_probe (FpDevice *device)
{
  fpi_device_probe_complete (device, NULL, NULL, NULL);
}

static void
fpi_device_oms2388_init (FpiDeviceOms2388 *self)
{
  self->selected_slot = 0xffff;
}

static void
fpi_device_oms2388_class_init (FpiDeviceOms2388Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "Jiangxi OMS/O-Film 33a7:2388 MOC Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = OMS2388_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = oms2388_probe;
  dev_class->open = oms2388_open;
  dev_class->close = oms2388_close;
  dev_class->enroll = oms2388_enroll;
  dev_class->identify = oms2388_identify;
  dev_class->list = oms2388_list;
  dev_class->delete = oms2388_delete;
  dev_class->clear_storage = oms2388_clear_storage;
  dev_class->cancel = oms2388_cancel;
  dev_class->suspend = oms2388_suspend;
  dev_class->resume = oms2388_resume;

  fpi_device_class_auto_initialize_features (dev_class);
}
