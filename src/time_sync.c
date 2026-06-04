#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aula.h"

/*
 * [TODO] I'm not sure this is totally correct
 * 		I'll probably have to play with this with system time
 * 		Or I can create a way to send a certain time
 * From our Wireshark capture, the time command was:
 * 04 72 01 00 00 00 00 [year_lo] [year_hi] 00 00 ...
 *
 * Sent as a SET_REPORT control transfer to interface 2 (EP0).
 * The full packet is always 64 bytes, zero padded.
 */
#define CMD_LEN 64

int aula_sync_time(aula_device_t *dev, int year, int mon, int mday,
		int hour, int min, int sec) {
	uint8_t buf[CMD_LEN];
	struct tm *t;
	time_t now;
	int ret;

	/* Get current local time */
	now = time(NULL);
	t   = localtime(&now);

	/*
	 * For each field, use the override if provided (-1 means use system).
	 * tm_year is years since 1900, tm_mon is 0-11 -- we convert
	 * from human-friendly input to struct tm's internal representation.
	 */
	int use_year = (year != -1) ? year 	        : t->tm_year + 1900;
	int use_mon	 = (mon  != -1) ? mon	 		: t->tm_mon + 1;
	int use_mday = (mday != -1) ? mday			: t->tm_mday;
	int use_hour = (hour != -1) ? hour			: t->tm_hour;
	int use_min  = (min  != -1) ? min			: t->tm_min;
	int use_sec  = (sec  != -1) ? sec			: t->tm_sec;
	int use_wday = t->tm_wday;

	/* --- Command 1: enter config mode --- */
	memset(buf, 0, CMD_LEN);
	buf[0] = 0x04;
	buf[1] = 0x18;
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	/* --- Command 2: Prepare for time update --- */
	memset(buf, 0, CMD_LEN);
	buf[0] = 0x04;
	buf[1] = 0x28;
	buf[8] = 0x01;
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	/*
	 *  --- Command 3: time data ---
	 * Build the time command packet.
	 * Fields we identified from the capture:
	 *   [0]   = 0x00 packet type
	 *   [1]   = 0x01 subcommand (am/pm?)
	 *   [2]   = year - 2000
	 *   [3]   = 0x1a
	 * 	 [4]   = month
	 * 	 [5]   = day
	 * 	 [6]   = hour (24 hours)
	 * 	 [7]   = minute
	 * 	 [8]   = second
	 */

	memset(buf, 0, CMD_LEN);
	buf[0]  = 0x00;
	buf[1]  = 0x01;
	buf[2]  = 0x5a; /* unknown -- constant in capture */
	buf[3]  = 0x1a; /* unknown -- constant in capture */
	buf[4]  = (uint8_t)use_mon;
	buf[5]  = (uint8_t)use_mday;
	buf[6]  = (uint8_t)use_hour;
	buf[7]  = (uint8_t)use_min;
	buf[8]  = (uint8_t)use_sec;
	buf[10] = (uint8_t)use_wday;

	buf[62] = 0xaa;
	buf[63] = 0x55;
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	/*
	 * Send as an HID SET_REPORT control transfer.
	 * this is the same mechanism the Epomaker software uses
	 *
	 * libusb_control_transfer parameters:
	 *   handle        - our device
	 *   bmRequestType - direction | type | recipient
	 *                   0x21 = host->device, class, interface
	 *   bRequest      - 0x09 = HID SET_REPORT
	 *   wValue        - 0x0300 = report type 3 (feature), report ID 0
	 *   wIndex        - interface number (2)
	 *   data          - our packet
	 *   wLength       - packet length
	 *   timeout       - milliseconds before giving up
	 */

	memset(buf, 0, CMD_LEN);
	/* TODO: does buf empty out? */
	printf(buf);
	buf[0] = 0x04;
	buf[1] = 0x02;
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
	    use_year,
		use_mon,
		use_mday,
		use_hour,
		use_min,
		use_sec
	);

	return AULA_OK;
}
