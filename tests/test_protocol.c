#include "oms_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void test_version(void)
{
    const uint8_t response[OMS_PACKET_SIZE] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x07, 0x00,
        0x1c, 0x00, 'a',  'c',  'm',  '3',  '2',  '_',
        'b',  't',  'l',  '_',  'o',  'f',  'i',  'l',
        'm',  '_',  'p',  'c',  '_',  '0',  '0',  '0',
        '0',  '5',  '9',  0x08, 0x8f,
    };
    char version[OMS_VERSION_MAX + 1] = {0};
    CHECK(oms_parse_firmware_version(response, sizeof(response), version) == 0);
    CHECK(strcmp(version, "acm32_btl_ofilm_pc_000059") == 0);
}

static void test_template_count(void)
{
    const uint8_t response[12] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff,
        0x07, 0x00, 0x05, 0x00, 0x00, 0x03,
    };
    uint16_t count = 0;
    CHECK(oms_parse_template_count(response, sizeof(response), &count) == 0);
    CHECK(count == 3);
}

static void test_template_index(void)
{
    uint8_t response[OMS_PACKET_SIZE] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff,
        0x07, 0x00, 0x23, 0x00,
    };
    uint8_t expected[32];
    uint8_t actual[32] = {0};
    for (size_t i = 0; i < sizeof(expected); ++i)
        response[10 + i] = expected[i] = (uint8_t)i;
    CHECK(oms_parse_template_index(response, sizeof(response), actual) == 0);
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
}

static void test_rejects_bad_header(void)
{
    uint8_t response[OMS_PACKET_SIZE] = {0};
    char version[OMS_VERSION_MAX + 1] = {0};
    CHECK(oms_parse_firmware_version(response, sizeof(response), version) < 0);
}

static void test_frame_codec(void)
{
    const uint8_t payload[] = {0x32, 0x05, 0xff, 0xff, 0x00, 0x00};
    const uint8_t expected[] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
        0x08, 0x32, 0x05, 0xff, 0xff, 0x00, 0x00, 0x02, 0x3e,
    };
    uint8_t frame_data[OMS_PACKET_SIZE];
    size_t frame_length = 0;
    CHECK(oms_frame_build(0x01, payload, sizeof(payload), frame_data,
                          &frame_length) == 0);
    CHECK(frame_length == sizeof(expected));
    CHECK(memcmp(frame_data, expected, sizeof(expected)) == 0);

    size_t cursor = 0;
    OmsFrameView frame;
    CHECK(oms_frame_next(frame_data, frame_length, &cursor, &frame) == 1);
    CHECK(frame.direction == 0x01);
    CHECK(frame.payload_length == sizeof(payload));
    CHECK(memcmp(frame.payload, payload, sizeof(payload)) == 0);
    CHECK(cursor == frame_length);
    CHECK(oms_frame_next(frame_data, frame_length, &cursor, &frame) == 0);
}

static void test_enroll_frame_and_events(void)
{
    const uint8_t payload[] = {0x31, 0x00, 0x01, 0x06, 0x00, 0x00};
    const uint8_t expected[] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
        0x08, 0x31, 0x00, 0x01, 0x06, 0x00, 0x00, 0x00, 0x41,
    };
    uint8_t frame[OMS_PACKET_SIZE];
    size_t frame_length = 0;
    CHECK(oms_frame_build(0x01, payload, sizeof(payload), frame,
                          &frame_length) == 0);
    CHECK(frame_length == sizeof(expected));
    CHECK(memcmp(frame, expected, sizeof(expected)) == 0);

    const uint8_t capture_payload[] = {0x00, 0x03, 0x04};
    CHECK(oms_frame_build(0x07, capture_payload, sizeof(capture_payload),
                          frame, &frame_length) == 0);
    OmsEnrollEvent event = {0};
    bool finished = false;
    CHECK(oms_parse_enroll_events(frame, frame_length, 1, &event,
                                  &finished) == 1);
    CHECK(!finished);
    CHECK(event.type == OMS_ENROLL_EVENT_CAPTURE);
    CHECK(event.capture_count == 4);

    const uint8_t complete_payload[] = {0x00, 0x06, 0x00};
    CHECK(oms_frame_build(0x07, complete_payload, sizeof(complete_payload),
                          frame, &frame_length) == 0);
    CHECK(oms_parse_enroll_events(frame, frame_length, 1, &event,
                                  &finished) == 1);
    CHECK(finished);
    CHECK(event.type == OMS_ENROLL_EVENT_COMPLETE);

    const uint8_t duplicate_payload[] = {
        OMS_ENROLL_ERROR_DUPLICATE, 0x01,
    };
    memset(&event, 0, sizeof(event));
    CHECK(oms_frame_build(0x07, duplicate_payload,
                          sizeof(duplicate_payload), frame,
                          &frame_length) == 0);
    CHECK(oms_parse_enroll_events(frame, frame_length, 1, &event,
                                  &finished) == 1);
    CHECK(finished);
    CHECK(event.type == OMS_ENROLL_EVENT_ERROR);
    CHECK(event.error_codes[0] == OMS_ENROLL_ERROR_DUPLICATE);
}

static void test_delete_and_sleep_frames(void)
{
    const uint8_t delete_payload[] = {0x0c, 0x00, 0x01, 0x00, 0x01};
    const uint8_t delete_expected[] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00,
        0x07, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x16,
    };
    uint8_t frame[OMS_PACKET_SIZE];
    size_t frame_length = 0;
    CHECK(oms_frame_build(0x01, delete_payload, sizeof(delete_payload),
                          frame, &frame_length) == 0);
    CHECK(frame_length == sizeof(delete_expected));
    CHECK(memcmp(frame, delete_expected, sizeof(delete_expected)) == 0);

    const uint8_t sleep_payload[] = {0x33};
    const uint8_t sleep_expected[] = {
        0xef, 0x01, 0xff, 0xff, 0xff, 0xff,
        0x01, 0x00, 0x03, 0x33, 0x00, 0x37,
    };
    CHECK(oms_frame_build(0x01, sleep_payload, sizeof(sleep_payload),
                          frame, &frame_length) == 0);
    CHECK(frame_length == sizeof(sleep_expected));
    CHECK(memcmp(frame, sleep_expected, sizeof(sleep_expected)) == 0);
}

int main(void)
{
    CHECK(OMS_TEMPLATE_SLOTS == 30);
    CHECK(OMS_TEMPLATE_ID_COUNT == 256);
    test_version();
    test_template_count();
    test_template_index();
    test_rejects_bad_header();
    test_frame_codec();
    test_enroll_frame_and_events();
    test_delete_and_sleep_frames();
    if (failures == 0)
        puts("all protocol parser tests passed");
    return failures == 0 ? 0 : 1;
}
