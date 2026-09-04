#define _POSIX_C_SOURCE 200809L

#include "oms_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static void print_hex(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (i != 0)
            putchar(' ');
        printf("%02x", data[i]);
    }
    putchar('\n');
}

static void print_info(const OmsUsbInfo *info)
{
    printf("device: %04x:%04x, USB %x.%02x, device %x.%02x\n",
           OMS_VID, OMS_PID, info->usb_version >> 8,
           info->usb_version & 0xff, info->device_version >> 8,
           info->device_version & 0xff);
    printf("manufacturer: %s\n", info->manufacturer);
    printf("product: %s\n", info->product);
    printf("serial: %s\n", info->serial);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--trace] <command> [arguments]\n"
            "commands:\n"
            "  info                 USB descriptors only; sends no command\n"
            "  read-version         handshake and read firmware version\n"
            "  read-chip-serial     handshake and read 32-byte chip serial\n"
            "  template-count       handshake and read stored-template count\n"
            "  template-index       handshake and read 32-byte index table\n"
            "  probe-readonly       run every non-destructive query above\n"
            "  identify             match a finger against on-chip templates\n"
            "  native-duplicate-test\n"
            "                       verify a known finger, then test firmware 0x22\n"
            "  enroll [slot] [timeout-ms]\n"
            "                       enroll into an empty slot (60s by default)\n"
            "  delete-template N --yes\n"
            "                       permanently delete template slot N\n"
            "  sleep                ask the sensor to enter low-power mode\n"
            "  drain-one-response   recover one response after interruption\n",
            program);
}

static int report_error(const char *operation, int rc)
{
    fprintf(stderr, "%s failed: %s (%d)\n", operation,
            oms_error_name(rc), rc);
    return EXIT_FAILURE;
}

static int parse_template_id(const char *text, uint16_t *slot)
{
    if (text == NULL || slot == NULL || *text == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (errno != 0 || *end != '\0' || value >= OMS_TEMPLATE_ID_COUNT)
        return -1;
    *slot = (uint16_t)value;
    return 0;
}

static int parse_timeout(const char *text, unsigned *timeout_ms)
{
    if (text == NULL || timeout_ms == NULL || *text == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 600000)
        return -1;
    *timeout_ms = (unsigned)value;
    return 0;
}

static bool template_slot_occupied(const uint8_t index[OMS_TEMPLATE_INDEX_BYTES],
                                   uint16_t slot)
{
    return (index[slot / 8] & (uint8_t)(1U << (slot % 8))) != 0;
}

static void sleep_milliseconds(long milliseconds)
{
    const struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
}

static void enroll_progress(const OmsEnrollEvent *event, void *user_data)
{
    (void)user_data;
    switch (event->type) {
    case OMS_ENROLL_EVENT_WAITING:
        printf("enroll: waiting (status=0x%02x phase=0x%02x)\n",
               event->code, event->parameter);
        break;
    case OMS_ENROLL_EVENT_CAPTURE:
        printf("enroll: capture accepted, count=%u; lift and touch again\n",
               event->capture_count);
        break;
    case OMS_ENROLL_EVENT_COMPLETE:
        printf("enroll: template slot %u completed\n", event->template_id);
        break;
    case OMS_ENROLL_EVENT_ERROR:
        printf("enroll: device error %02x %02x %02x\n",
               event->error_codes[0], event->error_codes[1],
               event->error_codes[2]);
        break;
    }
    fflush(stdout);
}

static int native_duplicate_test(OmsDevice *device)
{
    uint8_t before[OMS_TEMPLATE_INDEX_BYTES];
    uint8_t after[OMS_TEMPLATE_INDEX_BYTES];
    uint16_t free_slot = 0xffff;
    OmsIdentifyResult identify_result;
    OmsEnrollEvent enroll_result;

    int rc = oms_read_template_index(device, before);
    if (rc < 0)
        return rc;

    bool any_template = false;
    for (uint16_t i = 0; i < OMS_TEMPLATE_ID_COUNT; ++i) {
        if (template_slot_occupied(before, i)) {
            any_template = true;
            break;
        }
    }
    if (!any_template) {
        fprintf(stderr, "native duplicate test needs an existing template\n");
        return OMS_ERROR_DEVICE;
    }

    puts("native duplicate precheck: touch an already enrolled finger");
    fflush(stdout);
    memset(&identify_result, 0, sizeof(identify_result));
    rc = oms_identify(device, 30000, &identify_result);
    if (rc < 0)
        return rc;
    if (identify_result.code != 0 ||
        identify_result.template_id >= OMS_TEMPLATE_ID_COUNT ||
        !template_slot_occupied(before, identify_result.template_id)) {
        fprintf(stderr,
                "precheck did not match an occupied slot; enrollment was not started\n");
        return OMS_ERROR_DEVICE;
    }
    printf("native duplicate precheck matched slot %u\n",
           identify_result.template_id);

    /* Match the official enrollment entry sequence after the independent
     * identity proof above: wait in D0, refresh the index, choose a free
     * Windows-range slot, then use command 0x31 once per accepted capture. */
    sleep_milliseconds(500);
    rc = oms_read_template_index(device, before);
    if (rc < 0)
        return rc;
    for (uint16_t i = 0; i < OMS_TEMPLATE_SLOTS; ++i) {
        if (!template_slot_occupied(before, i)) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == 0xffff) {
        fprintf(stderr, "no free Windows-range slot is available\n");
        return OMS_ERROR_DEVICE;
    }

    printf("native duplicate test: use the same finger again; candidate slot %u\n",
           free_slot);
    fflush(stdout);
    memset(&enroll_result, 0, sizeof(enroll_result));
    rc = oms_enroll(device, free_slot, 90000, enroll_progress, NULL,
                    &enroll_result);

    int index_rc = oms_read_template_index(device, after);
    if (index_rc < 0)
        return index_rc;

    bool new_slot_created = template_slot_occupied(after, free_slot);
    if (new_slot_created) {
        fprintf(stderr, "firmware created unexpected duplicate slot %u; deleting it\n",
                free_slot);
        int delete_rc = oms_delete_template(device, free_slot);
        if (delete_rc < 0)
            return delete_rc;
        index_rc = oms_read_template_index(device, after);
        if (index_rc < 0)
            return index_rc;
        if (memcmp(before, after, sizeof(before)) != 0) {
            fprintf(stderr, "storage did not return to its original index\n");
            return OMS_ERROR_DEVICE;
        }
        puts("native duplicate cleanup: original storage restored");
    }

    if (rc == OMS_ERROR_DEVICE &&
        enroll_result.error_codes[0] == OMS_ENROLL_ERROR_DUPLICATE &&
        !new_slot_created) {
        puts("native duplicate result: firmware returned 0x22; storage unchanged");
        return 0;
    }

    if (rc == 0)
        fprintf(stderr,
                "native duplicate result: firmware completed enrollment without 0x22\n");
    else
        fprintf(stderr,
                "native duplicate result: enrollment ended with rc=%d errors=%02x %02x %02x\n",
                rc, enroll_result.error_codes[0],
                enroll_result.error_codes[1], enroll_result.error_codes[2]);
    return OMS_ERROR_DEVICE;
}

int main(int argc, char **argv)
{
    bool trace = false;
    const char *command = "info";
    if (argc >= 2 && strcmp(argv[1], "--trace") == 0) {
        trace = true;
        --argc;
        ++argv;
    }
    if (argc >= 2)
        command = argv[1];

    bool all = strcmp(command, "probe-readonly") == 0;
    bool known = all || strcmp(command, "info") == 0 ||
                 strcmp(command, "read-version") == 0 ||
                 strcmp(command, "read-chip-serial") == 0 ||
                 strcmp(command, "template-count") == 0 ||
                 strcmp(command, "template-index") == 0 ||
                 strcmp(command, "identify") == 0 ||
                 strcmp(command, "native-duplicate-test") == 0 ||
                 strcmp(command, "enroll") == 0 ||
                 strcmp(command, "delete-template") == 0 ||
                 strcmp(command, "sleep") == 0 ||
                 strcmp(command, "drain-one-response") == 0;
    if (!known) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t requested_slot = 0xffff;
    unsigned enroll_timeout_ms = 60000;
    if (strcmp(command, "enroll") == 0) {
        if (argc > 4 ||
            (argc >= 3 && parse_template_id(argv[2], &requested_slot) < 0) ||
            (requested_slot != 0xffff &&
             requested_slot >= OMS_TEMPLATE_SLOTS) ||
            (argc == 4 && parse_timeout(argv[3], &enroll_timeout_ms) < 0)) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else if (strcmp(command, "delete-template") == 0) {
        if (argc != 4 || parse_template_id(argv[2], &requested_slot) < 0 ||
            strcmp(argv[3], "--yes") != 0) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else if (argc != 2 && argc != 1) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    OmsDevice *device = NULL;
    int rc = oms_device_open(&device, trace, stderr);
    if (rc < 0)
        return report_error("open", rc);

    OmsUsbInfo info;
    rc = oms_device_get_info(device, &info);
    if (rc < 0) {
        oms_device_close(device);
        return report_error("get USB information", rc);
    }
    print_info(&info);

    if (strcmp(command, "info") == 0) {
        puts("inspect-only mode: no interface claimed and no command sent");
        oms_device_close(device);
        return EXIT_SUCCESS;
    }

    rc = oms_device_claim(device);
    if (rc < 0) {
        oms_device_close(device);
        return report_error("claim interface", rc);
    }

    if (strcmp(command, "drain-one-response") == 0) {
        uint8_t response[OMS_PACKET_SIZE] = {0};
        size_t length = 0;
        rc = oms_drain_one_response(device, response, &length);
        if (rc == 0) {
            printf("queued response (%zu bytes): ", length);
            print_hex(response, length);
        }
        oms_device_close(device);
        return rc < 0 ? report_error("drain response", rc) : EXIT_SUCCESS;
    }

    rc = oms_handshake(device);
    if (rc < 0) {
        oms_device_close(device);
        return report_error("handshake", rc);
    }
    puts("handshake: valid");

    if (all || strcmp(command, "read-version") == 0) {
        char version[OMS_VERSION_MAX + 1];
        rc = oms_read_firmware_version(device, version);
        if (rc < 0)
            goto done;
        printf("firmware version: %s\n", version);
    }
    if (all || strcmp(command, "read-chip-serial") == 0) {
        uint8_t serial[32];
        rc = oms_read_chip_serial(device, serial);
        if (rc < 0)
            goto done;
        printf("chip serial: ");
        print_hex(serial, sizeof(serial));
    }
    if (all || strcmp(command, "template-count") == 0) {
        uint16_t count = 0;
        rc = oms_get_template_count(device, &count);
        if (rc < 0)
            goto done;
        printf("valid template count: %u\n", count);
    }
    if (all || strcmp(command, "template-index") == 0) {
        uint8_t index[OMS_TEMPLATE_INDEX_BYTES];
        rc = oms_read_template_index(device, index);
        if (rc < 0)
            goto done;
        printf("template index: ");
        print_hex(index, sizeof(index));
    }
    if (strcmp(command, "identify") == 0) {
        OmsIdentifyResult result;
        puts("touch the fingerprint sensor (20 second timeout)...");
        fflush(stdout);
        rc = oms_identify(device, 20000, &result);
        if (rc < 0)
            goto done;
        printf("identify result: code=0x%02x parameter=0x%02x "
               "template_id=%u\n",
               result.code, result.parameter, result.template_id);
        if (result.error_count != 0) {
            printf("identify error details: ");
            print_hex(result.error_codes, result.error_count);
        }
    }
    if (strcmp(command, "native-duplicate-test") == 0)
        rc = native_duplicate_test(device);
    if (strcmp(command, "enroll") == 0) {
        uint8_t index[OMS_TEMPLATE_INDEX_BYTES];
        /* The Windows driver holds the device in D0 and waits 500 ms here. */
        sleep_milliseconds(500);
        rc = oms_read_template_index(device, index);
        if (rc < 0)
            goto done;
        if (requested_slot == 0xffff) {
            for (uint16_t i = 0; i < OMS_TEMPLATE_SLOTS; ++i) {
                if (!template_slot_occupied(index, i)) {
                    requested_slot = i;
                    break;
                }
            }
        }
        if (requested_slot == 0xffff) {
            rc = OMS_ERROR_DEVICE;
            fprintf(stderr, "no empty on-chip template slot is available\n");
            goto done;
        }
        if (template_slot_occupied(index, requested_slot)) {
            rc = OMS_ERROR_DEVICE;
            fprintf(stderr, "template slot %u is occupied; refusing to overwrite it\n",
                    requested_slot);
            goto done;
        }
        printf("enrolling into template slot %u (%u ms timeout)...\n",
               requested_slot, enroll_timeout_ms);
        puts("touch, lift, and touch the sensor as progress is reported");
        fflush(stdout);
        OmsEnrollEvent result;
        rc = oms_enroll(device, requested_slot, enroll_timeout_ms, enroll_progress,
                        NULL, &result);
    }
    if (strcmp(command, "delete-template") == 0) {
        printf("permanently deleting template slot %u...\n", requested_slot);
        rc = oms_delete_template(device, requested_slot);
        if (rc == 0)
            puts("template deleted");
    }
    if (strcmp(command, "sleep") == 0) {
        rc = oms_sleep(device);
        if (rc == 0)
            puts("sensor entered low-power mode");
    }

done:
    oms_device_close(device);
    return rc < 0 ? report_error(command, rc) : EXIT_SUCCESS;
}
