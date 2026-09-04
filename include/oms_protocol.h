#ifndef OMS_PROTOCOL_H
#define OMS_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define OMS_VID 0x33a7
#define OMS_PID 0x2388
#define OMS_PACKET_SIZE 64
#define OMS_VERSION_MAX 31
#define OMS_TEMPLATE_INDEX_BYTES 32
#define OMS_TEMPLATE_ID_COUNT (OMS_TEMPLATE_INDEX_BYTES * 8)
#define OMS_TEMPLATE_SLOTS 30
#define OMS_ENROLL_ERROR_DUPLICATE 0x22

typedef struct oms_device OmsDevice;

typedef struct {
    uint16_t usb_version;
    uint16_t device_version;
    char manufacturer[256];
    char product[256];
    char serial[256];
} OmsUsbInfo;

typedef struct {
    uint8_t direction;
    const uint8_t *payload;
    size_t payload_length;
    size_t frame_offset;
    size_t frame_length;
} OmsFrameView;

typedef struct {
    uint8_t code;
    uint8_t parameter;
    uint16_t template_id;
    uint8_t error_count;
    uint8_t error_codes[16];
} OmsIdentifyResult;

typedef enum {
    OMS_ENROLL_EVENT_WAITING,
    OMS_ENROLL_EVENT_CAPTURE,
    OMS_ENROLL_EVENT_COMPLETE,
    OMS_ENROLL_EVENT_ERROR,
} OmsEnrollEventType;

typedef struct {
    OmsEnrollEventType type;
    uint8_t code;
    uint8_t parameter;
    uint8_t capture_count;
    uint16_t template_id;
    uint8_t error_codes[3];
} OmsEnrollEvent;

typedef void (*OmsEnrollCallback)(const OmsEnrollEvent *event,
                                  void *user_data);

enum {
    OMS_ERROR_PROTOCOL = -1000,
    OMS_ERROR_UNEXPECTED_RESPONSE = -1001,
    OMS_ERROR_PARSE = -1002,
    OMS_ERROR_DEVICE = -1003,
};

const char *oms_error_name(int error);

int oms_device_open(OmsDevice **device, bool trace, FILE *trace_stream);
void oms_device_close(OmsDevice *device);
int oms_device_get_info(OmsDevice *device, OmsUsbInfo *info);
int oms_device_claim(OmsDevice *device);

int oms_handshake(OmsDevice *device);
int oms_read_firmware_version(OmsDevice *device,
                              char version[OMS_VERSION_MAX + 1]);
int oms_read_chip_serial(OmsDevice *device, uint8_t serial[32]);
int oms_get_template_count(OmsDevice *device, uint16_t *count);
int oms_read_template_index(OmsDevice *device,
                            uint8_t index[OMS_TEMPLATE_INDEX_BYTES]);
int oms_identify(OmsDevice *device, unsigned timeout_ms,
                 OmsIdentifyResult *result);
int oms_enroll(OmsDevice *device, uint16_t template_id,
               unsigned timeout_ms, OmsEnrollCallback callback,
               void *user_data, OmsEnrollEvent *result);
int oms_delete_template(OmsDevice *device, uint16_t template_id);
int oms_sleep(OmsDevice *device);
int oms_cancel(OmsDevice *device);
int oms_drain_one_response(OmsDevice *device,
                           uint8_t response[OMS_PACKET_SIZE],
                           size_t *response_length);

/* Response parsers are public so captured packets can be tested offline. */
int oms_parse_firmware_version(const uint8_t *response, size_t length,
                               char version[OMS_VERSION_MAX + 1]);
int oms_parse_chip_serial(const uint8_t *response, size_t length,
                          uint8_t serial[32]);
int oms_parse_template_count(const uint8_t *response, size_t length,
                             uint16_t *count);
int oms_parse_template_index(const uint8_t *response, size_t length,
                             uint8_t index[OMS_TEMPLATE_INDEX_BYTES]);
int oms_parse_enroll_events(const uint8_t *response, size_t length,
                            uint16_t template_id, OmsEnrollEvent *event,
                            bool *finished);
int oms_frame_build(uint8_t direction, const uint8_t *payload,
                    size_t payload_length, uint8_t frame[OMS_PACKET_SIZE],
                    size_t *frame_length);
int oms_frame_next(const uint8_t *buffer, size_t length, size_t *cursor,
                   OmsFrameView *frame);

#endif
