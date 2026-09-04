#define _POSIX_C_SOURCE 200809L

#include "oms_protocol.h"

#include <libusb.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OMS_INTERFACE 0
#define OMS_EP_OUT 0x01
#define OMS_EP_IN 0x81
#define OMS_TIMEOUT_MS 1500

struct oms_device {
    libusb_context *context;
    libusb_device_handle *handle;
    bool claimed;
    bool trace;
    FILE *trace_stream;
};

static const uint8_t handshake_request[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
    0x03, 0x35, 0x00, 0x39,
};

static const uint8_t handshake_response[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x07, 0x00,
    0x03, 0x00, 0x00, 0x0a,
};

static const uint8_t version_request[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
    0x04, 0xaf, 0x10, 0x00, 0xc4,
};

static const uint8_t chip_serial_request[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
    0x04, 0x34, 0x00, 0x00, 0x39,
};

static const uint8_t template_count_request[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
    0x03, 0x1d, 0x00, 0x21,
};

static const uint8_t template_index_request[] = {
    0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
    0x04, 0x1f, 0x00, 0x00, 0x24,
};

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void sleep_ms(long milliseconds)
{
    const struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static void trace_packet(OmsDevice *device, const char *label,
                         const uint8_t *data, size_t length)
{
    if (!device->trace)
        return;

    FILE *stream = device->trace_stream != NULL ? device->trace_stream : stderr;
    fprintf(stream, "%s (%zu bytes):", label, length);
    for (size_t i = 0; i < length; ++i) {
        if (i % 16 == 0)
            fputs("\n  ", stream);
        fprintf(stream, "%02x ", data[i]);
    }
    fputc('\n', stream);
}

static int bulk_out(OmsDevice *device, const uint8_t *data, int length,
                    const char *label)
{
    int transferred = 0;
    trace_packet(device, label, data, (size_t)length);
    int rc = libusb_bulk_transfer(device->handle, OMS_EP_OUT,
                                  (unsigned char *)data, length, &transferred,
                                  OMS_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS)
        return rc;
    return transferred == length ? 0 : LIBUSB_ERROR_IO;
}

static int bulk_in(OmsDevice *device, uint8_t *data, int maximum,
                   const char *label)
{
    int transferred = 0;
    int rc = libusb_bulk_transfer(device->handle, OMS_EP_IN, data, maximum,
                                  &transferred, OMS_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS)
        return rc;
    trace_packet(device, label, data, (size_t)transferred);
    return transferred;
}

static int validate_acknowledgement(const uint8_t *data, int length)
{
    static const uint8_t usbs[] = {'U', 'S', 'B', 'S'};
    return length == (int)sizeof(usbs) &&
           memcmp(data, usbs, sizeof(usbs)) == 0
               ? 0
               : OMS_ERROR_PROTOCOL;
}

static int request_queued_response(OmsDevice *device, uint8_t *response,
                                   uint32_t maximum)
{
    const uint8_t read_header[8] = {
        'U', 'S', 'B', 'C', 0x80, (uint8_t)maximum,
        (uint8_t)(maximum >> 8), (uint8_t)(maximum >> 16),
    };
    uint8_t acknowledgement[OMS_PACKET_SIZE] = {0};

    int rc = bulk_out(device, read_header, sizeof(read_header),
                      "OUT USBC read header");
    if (rc < 0)
        return rc;

    rc = bulk_in(device, response, (int)maximum, "IN response");
    if (rc < 0)
        return rc;
    int response_length = rc;

    rc = bulk_in(device, acknowledgement, sizeof(acknowledgement),
                 "IN read acknowledgement");
    if (rc < 0)
        return rc;
    rc = validate_acknowledgement(acknowledgement, rc);
    return rc < 0 ? rc : response_length;
}

static int exchange(OmsDevice *device, const uint8_t *request,
                    size_t request_length,
                    uint8_t response[OMS_PACKET_SIZE])
{
    const uint8_t write_header[8] = {
        'U', 'S', 'B', 'C', 0x00, OMS_PACKET_SIZE, 0x00, 0x00,
    };
    uint8_t packet[OMS_PACKET_SIZE] = {0};
    uint8_t acknowledgement[OMS_PACKET_SIZE] = {0};

    if (request_length > sizeof(packet))
        return LIBUSB_ERROR_INVALID_PARAM;
    memcpy(packet, request, request_length);

    int rc = bulk_out(device, write_header, sizeof(write_header),
                      "OUT USBC write header");
    if (rc < 0)
        return rc;
    rc = bulk_out(device, packet, sizeof(packet), "OUT command");
    if (rc < 0)
        return rc;
    rc = bulk_in(device, acknowledgement, sizeof(acknowledgement),
                 "IN write acknowledgement");
    if (rc < 0)
        return rc;
    rc = validate_acknowledgement(acknowledgement, rc);
    if (rc < 0)
        return rc;

    sleep_ms(50);
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        rc = request_queued_response(device, response, OMS_PACKET_SIZE);
        if (rc < 0)
            return rc;

        bool frame_ready = false;
        for (int i = 0; i + 1 < rc; ++i) {
            if (response[i] == 0xef && response[i + 1] == 0x01) {
                frame_ready = true;
                break;
            }
        }
        if (frame_ready)
            return rc;
        sleep_ms(20);
    }
    return LIBUSB_ERROR_TIMEOUT;
}

int oms_frame_build(uint8_t direction, const uint8_t *payload,
                    size_t payload_length, uint8_t frame[OMS_PACKET_SIZE],
                    size_t *frame_length)
{
    if (frame == NULL || frame_length == NULL ||
        (payload_length != 0 && payload == NULL) || payload_length > 53)
        return LIBUSB_ERROR_INVALID_PARAM;

    memset(frame, 0, OMS_PACKET_SIZE);
    frame[0] = 0xef;
    frame[1] = 0x01;
    memset(frame + 2, 0xff, 4);
    frame[6] = direction;
    uint16_t encoded_length = (uint16_t)(payload_length + 2);
    frame[7] = (uint8_t)(encoded_length >> 8);
    frame[8] = (uint8_t)encoded_length;
    if (payload_length != 0)
        memcpy(frame + 9, payload, payload_length);

    uint16_t checksum = 0;
    for (size_t i = 6; i < 9 + payload_length; ++i)
        checksum = (uint16_t)(checksum + frame[i]);
    frame[9 + payload_length] = (uint8_t)(checksum >> 8);
    frame[10 + payload_length] = (uint8_t)checksum;
    *frame_length = payload_length + 11;
    return 0;
}

int oms_frame_next(const uint8_t *buffer, size_t length, size_t *cursor,
                   OmsFrameView *frame)
{
    if (buffer == NULL || cursor == NULL || frame == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;

    size_t offset = *cursor;
    while (offset + 9 <= length &&
           !(buffer[offset] == 0xef && buffer[offset + 1] == 0x01))
        ++offset;
    if (offset + 9 > length) {
        *cursor = length;
        return 0;
    }

    uint16_t encoded_length =
        (uint16_t)((uint16_t)buffer[offset + 7] << 8) | buffer[offset + 8];
    if (encoded_length < 2)
        return OMS_ERROR_PARSE;
    size_t total = 9 + encoded_length;
    if (offset + total > length)
        return OMS_ERROR_PARSE;

    uint16_t checksum = 0;
    for (size_t i = offset + 6; i < offset + total - 2; ++i)
        checksum = (uint16_t)(checksum + buffer[i]);
    uint16_t expected =
        (uint16_t)((uint16_t)buffer[offset + total - 2] << 8) |
        buffer[offset + total - 1];
    if (checksum != expected)
        return OMS_ERROR_PROTOCOL;

    frame->direction = buffer[offset + 6];
    frame->payload = buffer + offset + 9;
    frame->payload_length = encoded_length - 2;
    frame->frame_offset = offset;
    frame->frame_length = total;
    *cursor = offset + total;
    return 1;
}

static int check_common_response(const uint8_t *response, size_t length,
                                 uint8_t payload_length)
{
    static const uint8_t prefix[] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x07, 0x00,
    };
    if (length < 10 || memcmp(response, prefix, sizeof(prefix)) != 0)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    if (response[8] != payload_length || response[9] != 0x00)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    return 0;
}

static int get_ascii_descriptor(libusb_device_handle *handle, uint8_t index,
                                char output[256])
{
    output[0] = '\0';
    if (index == 0)
        return 0;
    int rc = libusb_get_string_descriptor_ascii(handle, index,
                                                 (unsigned char *)output, 255);
    if (rc < 0)
        return rc;
    output[rc] = '\0';
    return 0;
}

const char *oms_error_name(int error)
{
    switch (error) {
    case OMS_ERROR_PROTOCOL:
        return "OMS transport protocol error";
    case OMS_ERROR_UNEXPECTED_RESPONSE:
        return "unexpected OMS response";
    case OMS_ERROR_PARSE:
        return "OMS response parse error";
    case OMS_ERROR_DEVICE:
        return "OMS device reported an operation error";
    default:
        return libusb_error_name(error);
    }
}

int oms_device_open(OmsDevice **output, bool trace, FILE *trace_stream)
{
    if (output == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    *output = NULL;

    OmsDevice *device = calloc(1, sizeof(*device));
    if (device == NULL)
        return LIBUSB_ERROR_NO_MEM;
    device->trace = trace;
    device->trace_stream = trace_stream;

    int rc = libusb_init(&device->context);
    if (rc < 0) {
        free(device);
        return rc;
    }
    device->handle = libusb_open_device_with_vid_pid(device->context,
                                                     OMS_VID, OMS_PID);
    if (device->handle == NULL) {
        libusb_exit(device->context);
        free(device);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    *output = device;
    return 0;
}

void oms_device_close(OmsDevice *device)
{
    if (device == NULL)
        return;
    if (device->claimed)
        (void)libusb_release_interface(device->handle, OMS_INTERFACE);
    libusb_close(device->handle);
    libusb_exit(device->context);
    free(device);
}

int oms_device_get_info(OmsDevice *device, OmsUsbInfo *info)
{
    if (device == NULL || info == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    memset(info, 0, sizeof(*info));

    struct libusb_device_descriptor descriptor;
    int rc = libusb_get_device_descriptor(libusb_get_device(device->handle),
                                          &descriptor);
    if (rc < 0)
        return rc;
    info->usb_version = descriptor.bcdUSB;
    info->device_version = descriptor.bcdDevice;
    rc = get_ascii_descriptor(device->handle, descriptor.iManufacturer,
                              info->manufacturer);
    if (rc < 0)
        return rc;
    rc = get_ascii_descriptor(device->handle, descriptor.iProduct,
                              info->product);
    if (rc < 0)
        return rc;
    return get_ascii_descriptor(device->handle, descriptor.iSerialNumber,
                                info->serial);
}

int oms_device_claim(OmsDevice *device)
{
    if (device == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (device->claimed)
        return 0;

    int rc = libusb_kernel_driver_active(device->handle, OMS_INTERFACE);
    if (rc > 0)
        return LIBUSB_ERROR_BUSY;
    if (rc < 0 && rc != LIBUSB_ERROR_NOT_SUPPORTED)
        return rc;
    rc = libusb_claim_interface(device->handle, OMS_INTERFACE);
    if (rc == 0)
        device->claimed = true;
    return rc;
}

int oms_handshake(OmsDevice *device)
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    int rc = exchange(device, handshake_request, sizeof(handshake_request),
                      response);
    if (rc < 0)
        return rc;
    if (rc < (int)sizeof(handshake_response) ||
        memcmp(response, handshake_response, sizeof(handshake_response)) != 0)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    return 0;
}

int oms_parse_firmware_version(const uint8_t *response, size_t length,
                               char version[OMS_VERSION_MAX + 1])
{
    static const uint8_t marker[] = {'a', 'c', 'm', '3', '2'};
    if (check_common_response(response, length, 0x1c) < 0)
        return OMS_ERROR_UNEXPECTED_RESPONSE;

    for (size_t offset = 10;
         offset <= 37 && offset + sizeof(marker) <= length; ++offset) {
        if (memcmp(response + offset, marker, sizeof(marker)) != 0)
            continue;
        size_t output_length = 0;
        while (offset + output_length < length &&
               output_length < OMS_VERSION_MAX &&
               isprint((unsigned char)response[offset + output_length])) {
            version[output_length] = (char)response[offset + output_length];
            ++output_length;
        }
        version[output_length] = '\0';
        return output_length >= sizeof(marker) ? 0 : OMS_ERROR_PARSE;
    }
    return OMS_ERROR_PARSE;
}

int oms_read_firmware_version(OmsDevice *device,
                              char version[OMS_VERSION_MAX + 1])
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    int rc = exchange(device, version_request, sizeof(version_request),
                      response);
    return rc < 0 ? rc
                  : oms_parse_firmware_version(response, (size_t)rc, version);
}

int oms_parse_chip_serial(const uint8_t *response, size_t length,
                          uint8_t serial[32])
{
    if (length < 42 || check_common_response(response, length, 0x23) < 0)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    memcpy(serial, response + 10, 32);
    return 0;
}

int oms_read_chip_serial(OmsDevice *device, uint8_t serial[32])
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    int rc = exchange(device, chip_serial_request, sizeof(chip_serial_request),
                      response);
    return rc < 0 ? rc : oms_parse_chip_serial(response, (size_t)rc, serial);
}

int oms_parse_template_count(const uint8_t *response, size_t length,
                             uint16_t *count)
{
    int rc = check_common_response(response, length, 0x05);
    if (rc < 0 || length < 12)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    *count = (uint16_t)((uint16_t)response[10] << 8) | response[11];
    return 0;
}

int oms_get_template_count(OmsDevice *device, uint16_t *count)
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    int rc = exchange(device, template_count_request,
                      sizeof(template_count_request), response);
    return rc < 0 ? rc
                  : oms_parse_template_count(response, (size_t)rc, count);
}

int oms_parse_template_index(const uint8_t *response, size_t length,
                             uint8_t index[OMS_TEMPLATE_INDEX_BYTES])
{
    int rc = check_common_response(response, length, 0x23);
    if (rc < 0 || length < 42)
        return OMS_ERROR_UNEXPECTED_RESPONSE;
    memcpy(index, response + 10, OMS_TEMPLATE_INDEX_BYTES);
    return 0;
}

int oms_read_template_index(OmsDevice *device,
                            uint8_t index[OMS_TEMPLATE_INDEX_BYTES])
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    int rc = exchange(device, template_index_request,
                      sizeof(template_index_request), response);
    return rc < 0 ? rc
                  : oms_parse_template_index(response, (size_t)rc, index);
}

static int parse_identify_events(const uint8_t *buffer, size_t length,
                                 OmsIdentifyResult *result, bool *finished)
{
    size_t cursor = 0;
    OmsFrameView frame;
    *finished = false;

    for (;;) {
        int rc = oms_frame_next(buffer, length, &cursor, &frame);
        if (rc <= 0)
            return rc;
        if (frame.direction != 0x07 || frame.payload_length < 2)
            continue;

        result->code = frame.payload[0];
        result->parameter = frame.payload[1];
        if (result->parameter == 0x05 && frame.payload_length >= 4) {
            result->template_id =
                (uint16_t)((uint16_t)frame.payload[2] << 8) |
                frame.payload[3];
            *finished = true;
            return 1;
        }
        if (result->parameter == 0xf5 && frame.payload_length >= 3) {
            size_t available = frame.payload_length - 3;
            size_t count = frame.payload[2];
            if (count > available)
                count = available;
            if (count > sizeof(result->error_codes))
                count = sizeof(result->error_codes);
            result->error_count = (uint8_t)count;
            memcpy(result->error_codes, frame.payload + 3, count);
        }
    }
}

int oms_cancel(OmsDevice *device)
{
    const uint8_t payload[] = {0x30};
    uint8_t request[OMS_PACKET_SIZE];
    uint8_t response[OMS_PACKET_SIZE];
    size_t request_length = 0;
    int rc = oms_frame_build(0x01, payload, sizeof(payload), request,
                             &request_length);
    if (rc < 0)
        return rc;
    rc = exchange(device, request, request_length, response);
    return rc < 0 ? rc : 0;
}

static int parse_status_response(const uint8_t *response, size_t length)
{
    size_t cursor = 0;
    OmsFrameView frame;
    int rc = oms_frame_next(response, length, &cursor, &frame);
    if (rc <= 0 || frame.direction != 0x07 || frame.payload_length < 1)
        return rc < 0 ? rc : OMS_ERROR_UNEXPECTED_RESPONSE;
    return frame.payload[0] == 0 ? 0 : OMS_ERROR_DEVICE;
}

int oms_parse_enroll_events(const uint8_t *response, size_t length,
                            uint16_t template_id, OmsEnrollEvent *event,
                            bool *finished)
{
    if (response == NULL || event == NULL || finished == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;

    size_t cursor = 0;
    OmsFrameView frame;
    int events = 0;
    *finished = false;

    for (;;) {
        int rc = oms_frame_next(response, length, &cursor, &frame);
        if (rc <= 0)
            return rc < 0 ? rc : events;
        if (frame.direction != 0x07 || frame.payload_length < 2)
            continue;

        ++events;
        event->code = frame.payload[0];
        event->parameter = frame.payload[1];
        event->template_id = template_id;
        event->type = OMS_ENROLL_EVENT_WAITING;

        switch (event->parameter) {
        case 0x01:
        case 0x04:
            event->error_codes[0] = event->code;
            if (event->code == OMS_ENROLL_ERROR_DUPLICATE) {
                *finished = true;
                event->type = OMS_ENROLL_EVENT_ERROR;
                return events;
            }
            break;
        case 0x02:
        case 0x05:
            event->error_codes[1] = event->code;
            break;
        case 0x03:
            if (frame.payload_length >= 3)
                event->capture_count = frame.payload[2];
            event->type = OMS_ENROLL_EVENT_CAPTURE;
            break;
        case 0x06:
            event->error_codes[2] = event->code;
            *finished = true;
            event->type = event->error_codes[0] == 0 &&
                                  event->error_codes[1] == 0 &&
                                  event->error_codes[2] == 0
                              ? OMS_ENROLL_EVENT_COMPLETE
                              : OMS_ENROLL_EVENT_ERROR;
            break;
        default:
            break;
        }
    }
}

int oms_enroll(OmsDevice *device, uint16_t template_id,
               unsigned timeout_ms, OmsEnrollCallback callback,
               void *user_data, OmsEnrollEvent *result)
{
    if (device == NULL || result == NULL || template_id >= OMS_TEMPLATE_SLOTS)
        return LIBUSB_ERROR_INVALID_PARAM;

    const uint8_t payload[] = {
        0x31,
        (uint8_t)(template_id >> 8),
        (uint8_t)template_id,
        0x06, 0x00, 0x00,
    };
    uint8_t request[OMS_PACKET_SIZE];
    uint8_t response[OMS_PACKET_SIZE];
    size_t request_length = 0;
    memset(result, 0, sizeof(*result));
    result->template_id = template_id;

    int rc = oms_frame_build(0x01, payload, sizeof(payload), request,
                             &request_length);
    if (rc < 0)
        return rc;
    rc = exchange(device, request, request_length, response);
    if (rc < 0)
        return rc;

    bool finished = false;
    int parse_rc = oms_parse_enroll_events(response, (size_t)rc, template_id,
                                            result, &finished);
    if (parse_rc < 0)
        return parse_rc;
    if (parse_rc > 0 && callback != NULL)
        callback(result, user_data);
    if (finished) {
        if (result->type != OMS_ENROLL_EVENT_COMPLETE)
            (void)oms_cancel(device);
        return result->type == OMS_ENROLL_EVENT_COMPLETE ? 0
                                                         : OMS_ERROR_DEVICE;
    }

    uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    bool restart_enroll = result->type == OMS_ENROLL_EVENT_CAPTURE &&
                          result->capture_count < 6;
    while (monotonic_milliseconds() < deadline) {
        if (restart_enroll) {
            restart_enroll = false;
            rc = exchange(device, request, request_length, response);
        } else {
            sleep_ms(20);
            rc = request_queued_response(device, response, 32);
        }
        if (rc == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (rc < 0)
            break;
        parse_rc = oms_parse_enroll_events(response, (size_t)rc, template_id,
                                            result, &finished);
        if (parse_rc < 0) {
            rc = parse_rc;
            break;
        }
        if (parse_rc > 0 && callback != NULL)
            callback(result, user_data);
        if (finished) {
            if (result->type != OMS_ENROLL_EVENT_COMPLETE)
                (void)oms_cancel(device);
            return result->type == OMS_ENROLL_EVENT_COMPLETE ? 0
                                                             : OMS_ERROR_DEVICE;
        }
        restart_enroll = result->type == OMS_ENROLL_EVENT_CAPTURE &&
                         result->capture_count < 6;
    }

    int cancel_rc = oms_cancel(device);
    if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT)
        return rc;
    return cancel_rc < 0 ? cancel_rc : LIBUSB_ERROR_TIMEOUT;
}

int oms_delete_template(OmsDevice *device, uint16_t template_id)
{
    if (device == NULL || template_id >= OMS_TEMPLATE_ID_COUNT)
        return LIBUSB_ERROR_INVALID_PARAM;
    const uint8_t payload[] = {
        0x0c, (uint8_t)(template_id >> 8), (uint8_t)template_id, 0x00, 0x01,
    };
    uint8_t request[OMS_PACKET_SIZE];
    uint8_t response[OMS_PACKET_SIZE];
    size_t request_length = 0;
    int rc = oms_frame_build(0x01, payload, sizeof(payload), request,
                             &request_length);
    if (rc < 0)
        return rc;
    rc = exchange(device, request, request_length, response);
    return rc < 0 ? rc : parse_status_response(response, (size_t)rc);
}

int oms_sleep(OmsDevice *device)
{
    if (device == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    const uint8_t payload[] = {0x33};
    uint8_t request[OMS_PACKET_SIZE];
    uint8_t response[OMS_PACKET_SIZE];
    size_t request_length = 0;
    int rc = oms_frame_build(0x01, payload, sizeof(payload), request,
                             &request_length);
    if (rc < 0)
        return rc;
    rc = exchange(device, request, request_length, response);
    return rc < 0 ? rc : parse_status_response(response, (size_t)rc);
}

int oms_identify(OmsDevice *device, unsigned timeout_ms,
                 OmsIdentifyResult *result)
{
    const uint8_t payload[] = {0x32, 0x05, 0xff, 0xff, 0x00, 0x00};
    uint8_t request[OMS_PACKET_SIZE];
    uint8_t response[OMS_PACKET_SIZE];
    size_t request_length = 0;
    if (device == NULL || result == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    memset(result, 0, sizeof(*result));
    result->template_id = 0xffff;

    int rc = oms_frame_build(0x01, payload, sizeof(payload), request,
                             &request_length);
    if (rc < 0)
        return rc;
    rc = exchange(device, request, request_length, response);
    if (rc < 0)
        return rc;

    bool finished = false;
    int parse_rc = parse_identify_events(response, (size_t)rc, result,
                                         &finished);
    if (parse_rc < 0)
        return parse_rc;
    if (finished)
        return 0;

    uint64_t deadline = monotonic_milliseconds() + timeout_ms;
    while (monotonic_milliseconds() < deadline) {
        sleep_ms(20);
        rc = request_queued_response(device, response, 32);
        if (rc == LIBUSB_ERROR_TIMEOUT)
            continue;
        if (rc < 0)
            break;
        parse_rc = parse_identify_events(response, (size_t)rc, result,
                                         &finished);
        if (parse_rc < 0) {
            rc = parse_rc;
            break;
        }
        if (finished)
            return 0;
    }

    int cancel_rc = oms_cancel(device);
    if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT)
        return rc;
    return cancel_rc < 0 ? cancel_rc : LIBUSB_ERROR_TIMEOUT;
}

int oms_drain_one_response(OmsDevice *device,
                           uint8_t response[OMS_PACKET_SIZE],
                           size_t *response_length)
{
    int rc = request_queued_response(device, response, OMS_PACKET_SIZE);
    if (rc < 0)
        return rc;
    if (response_length != NULL)
        *response_length = (size_t)rc;
    return 0;
}
