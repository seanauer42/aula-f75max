#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gif_lib.h>
#include "aula.h"

/*
 * From our capture analysis:
 *
 * The display is 128x128 pixels, RGB565 encoded (2 bytes per pixel).
 * One full frame = 16384 pixels = 32768 bytes.
 *
 * Each USB interrupt transfer carries 4096 bytes.
 * One GIF frame requires 8 USB transfers:
 *
 *   Transfer 0 (header):
 *     byte 0:		0xfb  (command byte: "image data incoming")
 *     bytes 1-251: 0x05  (padding)
 *     bytes 252-255: ff ff ff ff (start delimiter)
 *     bytes 256-4095: first 1920 pixels (3840 bytes
 *
 *   Transfers 1-6: 2048 pixels each (4096 bytes), pure pixel data
 *
 *   Transfer 7 (tail):
 *     bytes 0-255: final 128 pixels (256 bytes)
 *     bytes 256-259: ff ff ff ff (end delimiter)
 *     bytes 260-4096: zeros
 */

#define USB_TRANSFER_LEN		4096
#define HEADER_PAYLOAD_START	256
#define HEADER_PAYLOAD_BYTES	(USB_TRANSFER_LEN - HEADER_PAYLOAD_START) /* 3840 */
#define MIDDLE_PAYLOAD_BYTES 	USB_TRANSFER_LEN						  /* 4096 */
#define TAIL_PAYLOAD_BYTES		256
#define TRANSFERS_PER_FRAME		8
#define FRAME_BYTES				(AULA_DISPLAY_PIXELS * 2) /* 32768 */
#define RESPONSE_LEN 			128
#define REQUEUE_LEN				64
#define CMD_LEN					64

/*
 * Convert a GIF frame to a flat RGB565 buffer.
 *
 * GIF uses a palette (colormap) -- each pixel is an index into a table
 * of RGB colors, not a direct color value. We look up each pixel's index
 * in the colormap and convert that RGB triplet to RGB565.
 *
 * RGB565 packs one pixel into 16 bits:
 *   bits 15-11: red   (5 bits)
 *   bits 10-5:  green (6 bits)
 *   bits 4-0:   blue  (5 bits)
 *
 * To convert 8-bit channel (0-255) to 5-bit (0-31): >> 3
 * To convert 8-bit channel (0-255) to 6-bit (0-63): >> 2
 */
static void frame_to_rgb565(GifFileType *gif, int frame_idx, uint8_t *out) {
	SavedImage *frame		= &gif->SavedImages[frame_idx];
	ColorMapObject *cmap	= frame->ImageDesc.ColorMap
							? frame->ImageDesc.ColorMap
							: gif->SColorMap;
	int width  = gif->SWidth;
	int height = gif->SHeight;
	int i;

	for (i = 0; i < width * height; i++) {
		uint8_t idx = (uint8_t)frame->RasterBits[i];
		GifColorType *color = &cmap->Colors[idx];

		uint16_t r = (uint16_t)(color->Red   >> 3);
		uint16_t g = (uint16_t)(color->Green >> 2);
		uint16_t b = (uint16_t)(color->Blue  >> 3);

		uint16_t pixel = (r << 11) | (g << 5) | b;

		/* Write little-endian: low byte first */
		out[i * 2]     = (uint8_t)(pixel & 0xFF);
		out[i * 2 + 1] = (uint8_t)(pixel >> 8);
	}
}

/* read 64-byte ack and requeue */
static int send_transfer(aula_device_t *dev, uint8_t *buf) {
	uint8_t response[RESPONSE_LEN];
	int transferred;
	int ret;

	/* Send 4096 bytes of pixel data */
	ret = libusb_interrupt_transfer(
		dev->handle,
		AULA_EP_OUT,
		buf,
		USB_TRANSFER_LEN,
		&transferred,
		1000
	);
	if (ret < 0) {
		fprintf(stderr, "OUT transfer failed: %s\n", libusb_strerror(ret));
		return AULA_ERR_IO;
	}

	//printf("OUT transfer: ret=%d transferred=%d\n", ret, transferred);
	fflush(stdout);

	/* Read 64-byte (128?) device ack on EP4 IN
	ret = libusb_interrupt_transfer(
		dev->handle,
		AULA_EP_IN,
		response,
		RESPONSE_LEN,
		&transferred,
		1000
	);

	/* Requeue zero-lenth IN transfer to keep EP4 ready */
	ret = libusb_interrupt_transfer(
		dev->handle,
		AULA_EP_IN,
		response,
		REQUEUE_LEN,
		&transferred,
		100
	);
	/* timeout here is expected and fine */
	if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "IN ack failed: %s\n", libusb_strerror(ret));
		return AULA_ERR_IO;
	}
	return AULA_OK;
}

/*
 * Send preamble commands before GIF transfer.
 * frame_count tells the device how many frames are coming.
 */
static int send_preamble(aula_device_t *dev, int frame_count, int slot) {
	uint8_t buf[CMD_LEN];
	//uint8_t response[RESPONSE_LEN];
	//int transferred;
	int ret;
	int total_transfers = frame_count * TRANSFERS_PER_FRAME;

	/* CMD 1: enter config mode */
	memset(buf, 0, CMD_LEN);
	buf[0] = 0x04;
	buf[1] = 0x18;
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	/* CMD 2: start GIF upload, send frame count */
	memset(buf, 0, CMD_LEN);
	buf[0] = 0x04;
	buf[1] = 0x72;
	buf[2] = (uint8_t)slot;
	buf[8] = (uint8_t)(total_transfers & 0xFF);
	buf[9] = (uint8_t)((total_transfers >> 8) & 0xFF);
	ret = aula_cmd_exchange(dev, buf);
	if (ret != AULA_OK) return ret;

	/*
	 * Wait for the device's "ready" signal on EP4 IN
	*/

	return AULA_OK;
}

/*
 * Send postamble commit command after all frames sent.
 */
static int send_postamble(aula_device_t *dev) {
	uint8_t buf[CMD_LEN];
	memset(buf, 0, CMD_LEN);
	buf[0] = 0x04;
	buf[1] = 0x02;
	return aula_cmd_exchange(dev, buf);
}

/*
 * Send one complete display frame (128x128 RGB565) to the keyboard.
 * Splits the frame across 8 USB interrupt transfers in the format
 * the firmware expects.
 */
static int send_frame(aula_device_t *dev, uint8_t *frame_rgb565) {
	uint8_t transfer_buf[USB_TRANSFER_LEN];
	int i;
	int ret;

	/* --- Transfer 0: header --- */
	memset(transfer_buf, 0, USB_TRANSFER_LEN);
	transfer_buf[0] = 0xfb; 					/* command byte */
	memset(&transfer_buf[1], 0x05, 251);		/* padding */
	transfer_buf[252] = 0xff;					/* start delimiter */
	transfer_buf[253] = 0xff;
	transfer_buf[254] = 0xff;
	transfer_buf[255] = 0xff;


	/* Copy first 3840 bytes of pixel data after the header */
	memcpy(&transfer_buf[HEADER_PAYLOAD_START],
			frame_rgb565,
			HEADER_PAYLOAD_BYTES);

	ret = send_transfer(dev, transfer_buf);
	if (ret != AULA_OK) return ret;

	/* --- Transfers 1-6: middle chunks --- */
	for (i = 1; i < TRANSFERS_PER_FRAME - 1; i++) {
		int offset = HEADER_PAYLOAD_BYTES + (i - 1) * USB_TRANSFER_LEN;
		memcpy(transfer_buf, frame_rgb565 + offset, USB_TRANSFER_LEN);

		ret = send_transfer(dev, transfer_buf);
		if (ret != AULA_OK) return ret;
	}

	/* --- Transfer 7: tail --- */
	memset(transfer_buf, 0, USB_TRANSFER_LEN);
	memcpy(transfer_buf, frame_rgb565 + (FRAME_BYTES -
			TAIL_PAYLOAD_BYTES),
			TAIL_PAYLOAD_BYTES);

	/* End delimiter */
	transfer_buf[256] = 0xff;
	transfer_buf[257] = 0xff;
	transfer_buf[258] = 0xff;
	transfer_buf[259] = 0xff;

	return send_transfer(dev, transfer_buf);
}

/*
 * Public API: open a GIF file, decode each fram, send to display
 * Loops continuously until interrupted (Ctrl+C)
 */
int aula_send_gif(aula_device_t *dev, const char *path, int slot) {
	int gif_err;
	int ret = AULA_OK;
	int i;
	if (slot < 0) slot = 5;

	printf("Attempting first transfer...\n");
	fflush(stdout);

	GifFileType *gif = DGifOpenFileName(path, &gif_err);
	if (gif == NULL) {
		fprintf(stderr, "Failed to open GIF '%s': %s\n",
				path, GifErrorString(gif_err));
		return AULA_ERR_IMAGE;
	}

	if (DGifSlurp(gif) != GIF_OK) {
		fprintf(stderr, "Failed to decode GIF: %s\n",
				GifErrorString(gif->Error));
		DGifCloseFile(gif, &gif_err);
		return AULA_ERR_IMAGE;
	}

	/* Validate dimensions */
	if (gif->SWidth != AULA_DISPLAY_WIDTH ||
			gif->SHeight != AULA_DISPLAY_HEIGHT) {
		fprintf(stderr, "GIF MUST be %dx%d pixels (got %dx%d)\n",
				AULA_DISPLAY_WIDTH, AULA_DISPLAY_HEIGHT,
				gif->SWidth, gif->SHeight);
		DGifCloseFile(gif, &gif_err);
		return AULA_ERR_IMAGE;
	}

	/*
	 * Allocate a buffer for one frame of RGB565 pixel data.
	 * malloc returns a void* which C implicitely converts to
	 * any pointer type -- no cast needed unlike C++.
	 */
	uint8_t *frame_buf = malloc(FRAME_BYTES);
	if (frame_buf == NULL) {
		fprintf(stderr, "Out of memory\n");
		DGifCloseFile(gif, &gif_err);
		return AULA_ERR_IO;
	}

	printf("Sending %d frame GIF to display slot %d...\n", gif->ImageCount, slot);

	ret = send_preamble(dev, gif->ImageCount, 1);
	if (ret != AULA_OK) goto cleanup;

	/*
	 * Loop through frames continuously.
	 * In a future version this will respect per-frame delay
	 * from the GIF metadata. For now we send as fast as the
	 * device accepts transfers.
	*/

	for (i = 0; i < gif->ImageCount; i++) {
		frame_to_rgb565(gif, i, frame_buf);

		ret = send_frame(dev, frame_buf);
		if (ret != AULA_OK) goto cleanup;

	   	/*
		 * Simple progress bar.
		 * \r returns to start of line without newline,
		 * so each update overwrites the previous one.
		 * fflush forces the output immediately since
		 * stdout is line-buffered by default.
		 */
		printf("\rFrame %d/%d [",
				i + 1, gif->ImageCount);
		int bar_width = 40;
		int filled = (i + 1) * bar_width / gif->ImageCount;
		int j;
		for (j = 0; j < bar_width; j++)
			putchar(j < filled? '#' : '-');

		printf("] %.0f%%",
				(float)(i + 1) / gif->ImageCount * 100.0f);
		fflush(stdout);
	}

	/* All frames sent successfully -- commit to display */
	ret = send_postamble(dev);

cleanup:
	printf("\n");
	free(frame_buf);
	DGifCloseFile(gif, &gif_err);
	return ret;
}
