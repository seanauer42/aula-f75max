#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "aula.h"

/*
 * Factory reset sequence reverse-engineered from USB capture.
 *
 * The reset is composed of 5 blocks, each committed with 04:02.
 * Several blocks are followed by 04:f0 which applies the changes.
 *
 * Block 1: clear display memory slots
 *   04:19            enter reset mode
 *   04:15 ... 08:00  clear 8 display slots
 *   7x zero pages    padding (slot count - 1 = 7 pages)
 *   04:02            commit
 *
 * Block 2: unknown config reset (likely keymap/macro)
 *   04:18            enter config mode
 *   04:11 ... 09:00  config with 9 data pages
 *   9x zero pages    data pages
 *   04:02            commit
 *   04:f0            apply
 *
 * Block 3: unknown config reset (likely lighting)
 *   04:18            enter config mode
 *   04:27 ... 09:00  config with 9 data pages
 *   9x zero pages    data pages
 *   04:02            commit
 *   04:f0            apply
 *
 * Block 4: reset data with magic footer
 *   04:18            enter config mode
 *   04:13 ... 01:00  config with 1 data page
 *   0b:ff...aa:55    reset payload (aa:55 = end marker)
 *   04:02            commit
 *   04:f0            apply
 *
 * Block 5: display config reset
 *   04:18            enter config mode
 *   04:17:01...01:00 display reset command
 *   00:01...02:00:02:00  display config data
 *   04:02            commit (final, no 04:f0)
 */

#define CMD_LEN 64

/*
 * Send a single zero-filled data page.
 * Used as padding between config commands.
 */
static int send_zero_page(aula_device_t *dev) {
    uint8_t buf[CMD_LEN];
    memset(buf, 0, CMD_LEN);

    int ret = libusb_control_transfer(
        dev->handle,
        0x21,
        0x09,
        0x0300,
        AULA_CMD_INTERFACE,
        buf,
        CMD_LEN,
        1000
    );
    if (ret < 0) {
        fprintf(stderr, "Zero page failed: %s\n", libusb_strerror(ret));
        return AULA_ERR_IO;
    }
    return AULA_OK;
}

/*
 * Send N zero pages in sequence.
 */
static int send_zero_pages(aula_device_t *dev, int count) {
    uint8_t buf[CMD_LEN];
    int i;
    int ret;

    for (i = 0; i < count - 1; i++) {
        memset(buf, 0, CMD_LEN);
        ret = libusb_control_transfer(
            dev->handle,
            0x21,
            0x09,
            0x0300,
            AULA_CMD_INTERFACE,
            buf,
            CMD_LEN,
            1000
        );
        if (ret < 0) {
            fprintf(stderr, "Zero page %d failed: %s\n", i, libusb_strerror(ret));
            return AULA_ERR_IO;
        }
        usleep(40000); /* probably unnecesary timeout */
    }

    memset(buf, 0, CMD_LEN);
    return aula_cmd_exchange(dev, buf);
}

int aula_factory_reset(aula_device_t *dev) {
    uint8_t buf[CMD_LEN];
    int ret;

    printf("Performing factory reset...\n");

    /* === Block 1: clear display memory === */

    /* Enter reset mode */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x19;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("Enter reset mode\n");
    /*
     * Clear display slots. Byte 8 = 0x08 = 8 slots.
     * Followed by (slot_count - 1) = 7 zero pages.
     */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x15;
    buf[8] = 0x08;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("display slots cleared ");

    ret = send_zero_pages(dev, 8);
    if (ret != AULA_OK) return ret;

    printf("zero pages sent\n");

    /* Commit block 1 */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("commit block1\n");

    /* === Block 2: keymap/macro reset === */

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x18;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("keymap reset\n");

    /* Config with 9 data pages */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x11;
    buf[8] = 0x09;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("config with 9 data pages\n");

    ret = send_zero_pages(dev, 9);
    if (ret != AULA_OK) return ret;

    printf("sent 9 zero pages\n");

    /* Commit and apply */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("commit and apply\n");

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0xf0;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    /* === Block 3: lighting reset === */

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x18;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("lighting reset\n");

    /* Config with 9 data pages */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x27;
    buf[8] = 0x09;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    ret = send_zero_pages(dev, 9);
    if (ret != AULA_OK) return ret;

    printf("sent 9 zero pages\n");

    /* Commit and apply */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0xf0;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("commited\n");

    /* === Block 4: reset payload with magic footer === */

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x18;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("reseting payload with magic footer");

    /* Config with 1 data page */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x13;
    buf[8] = 0x01;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    /*
     * Reset payload: 0b:ff followed by config bytes and aa:55 footer.
     * Exact bytes from capture frame 105:
     * 0b ff 00 00 00 00 00 00 01 05 03 00 00 00 aa 55
     */
    memset(buf, 0, CMD_LEN);
    buf[0]  = 0x0b;
    buf[1]  = 0xff;
    buf[8]  = 0x01;
    buf[9]  = 0x05;
    buf[10] = 0x03;
    buf[14] = 0xaa;
    buf[15] = 0x55;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("reset payload 0b:ff\n");

    /* Commit and apply */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0xf0;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("commit and apply\n");

    /* === Block 5: display config reset === */

    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x18;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("display config reset\n");
    /*
     * Display reset command: 04:17:01 with 01:00 at byte 8.
     * Exact bytes from capture frame 117:
     * 04 17 01 00 00 00 00 00 01 00
     */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x17;
    buf[2] = 0x01;
    buf[8] = 0x01;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("display reset command: 04:17:01\n");
    /*
     * Display config data. Exact bytes from capture frame 121:
     * 00 01 00 00 00 00 02 00 02 00
     */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x00;
    buf[1] = 0x01;
    buf[6] = 0x02;
    buf[8] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("display config data 00 01 00\n");

    /* Final commit — no 04:f0 after this one */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) return ret;

    printf("final commit\n");

    printf("Factory reset complete.\n");
    return AULA_OK;
}
