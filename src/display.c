#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gif_lib.h>
#include "aula.h"

/*
 * Stream format confirmed from OSX reference implementation:
 *
 * Header (256 bytes, zero-padded):
 *   [0]       = frameCount (uint8, max 255)
 *   [1..N]    = per-frame delay, one byte each, in units of 1/500 second
 *               (e.g. 0x32 = 50 = 100ms, matches GIF centisecond delay * 5)
 *   [N+1..255]= zeros
 *
 * Payload (frameCount * FRAME_BYTES):
 *   Frames stored back to back, each FRAME_BYTES = 128*128*2 = 32768 bytes
 *   Pixels are RGB565 little-endian (low byte first)
 *
 * Total stream is padded to a multiple of 4096 and sent in 4096-byte chunks.
 * chunkCount = ceil((HEADER_LEN + frameCount * FRAME_BYTES) / CHUNK_LEN)
 *
 * Protocol sequence:
 *   1. aula_cmd_exchange: 04 18 (enter config mode)
 *   2. aula_cmd_exchange: 04 72 [slot] 00 00 00 00 00 [chunkCount_lo] [chunkCount_hi]
 *   3. Read 128-byte ready signal from EP4 IN
 *   4. For each 4096-byte chunk: interrupt OUT on EP3, read 128-byte ack on EP4
 *   5. aula_cmd_exchange: 04 02 (commit)
 *
 * Slot number (buf[2] of the 04:72 command) selects which display slot to write.
 * Writing to an existing slot overwrites it. Default slot is 1.
 */

#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 128
#define HEADER_LEN     256
#define FRAME_BYTES    (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)  /* 32768 */
#define CHUNK_LEN      4096
#define CMD_LEN        64
#define RESPONSE_LEN   128
#define MAX_FRAMES     255

/*
 * Convert GIF delay (in seconds) to the device's delay unit.
 * The device uses 1/500 second units, matching the OSX implementation.
 * Minimum 1, maximum 255.
 */
static uint8_t seconds_to_delay_byte(double seconds) {
    if (seconds <= 0.0)
        seconds = 0.01;
    int value = (int)(seconds * 500.0 + 0.5);  /* round to nearest */
    if (value < 1)   value = 1;
    if (value > 255) value = 255;
    return (uint8_t)value;
}

/*
 * Convert a single GIF frame's palette-indexed pixels to RGB565 LE,
 * writing directly into the stream at the given offset.
 *
 * GIF pixels are palette indices. Each index maps to an RGB triplet
 * in the colormap. We convert each triplet to 16-bit RGB565:
 *   bits 15-11: red   (5 bits, >> 3 from 8-bit)
 *   bits 10-5:  green (6 bits, >> 2 from 8-bit)
 *   bits 4-0:   blue  (5 bits, >> 3 from 8-bit)
 * Written little-endian: low byte first.
 */
static void write_frame_rgb565(uint8_t *stream, size_t offset,
                                GifFileType *gif, int frame_idx) {
    SavedImage *frame    = &gif->SavedImages[frame_idx];
    ColorMapObject *cmap = frame->ImageDesc.ColorMap
                         ? frame->ImageDesc.ColorMap
                         : gif->SColorMap;
    int total = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    int i;

    for (i = 0; i < total; i++) {
        uint8_t idx         = (uint8_t)frame->RasterBits[i];
        GifColorType *color = &cmap->Colors[idx];

        uint16_t r = (uint16_t)(color->Red   >> 3);
        uint16_t g = (uint16_t)(color->Green >> 2);
        uint16_t b = (uint16_t)(color->Blue  >> 3);

        uint16_t pixel = (r << 11) | (g << 5) | b;

        stream[offset + i * 2]     = (uint8_t)(pixel & 0xFF);  /* low byte */
        stream[offset + i * 2 + 1] = (uint8_t)(pixel >> 8);    /* high byte */
    }
}

/*
 * Send one 4096-byte chunk to the device and read the 128-byte ack.
 * The device sends an ack after every chunk. If we don't read it,
 * the endpoint buffer fills and the device stalls on the next chunk.
 */
static int send_chunk(aula_device_t *dev, const uint8_t *chunk) {
    uint8_t response[RESPONSE_LEN];
    int transferred;
    int ret;

    ret = libusb_interrupt_transfer(
        dev->handle,
        AULA_EP_OUT,
        (uint8_t *)chunk,
        CHUNK_LEN,
        &transferred,
        2000
    );
    if (ret < 0) {
        fprintf(stderr, "Chunk OUT failed: %s\n", libusb_strerror(ret));
        return AULA_ERR_IO;
    }

    ret = libusb_interrupt_transfer(
        dev->handle,
        AULA_EP_IN,
        response,
        RESPONSE_LEN,
        &transferred,
        200
    );
    if (ret < 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        fprintf(stderr, "Chunk ack failed: %s\n", libusb_strerror(ret));
        return AULA_ERR_IO;
    }

    return AULA_OK;
}

/*
 * Public API: decode a GIF and upload it to the keyboard display.
 *
 * path  - path to a 128x128 GIF file (animated or static)
 * slot  - display slot to write (1-255), overwrites existing content
 */
int aula_send_gif(aula_device_t *dev, const char *path, int slot) {
    int gif_err;
    int ret = AULA_OK;
    int i;
    if (slot < 1)
        slot = 1;
    if (slot > 255)
        slot = 255;

    /* --- Open and decode GIF --- */
    GifFileType *gif = DGifOpenFileName(path, &gif_err);
    if (!gif) {
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

    if (gif->SWidth != DISPLAY_WIDTH || gif->SHeight != DISPLAY_HEIGHT) {
        fprintf(stderr, "GIF must be %dx%d pixels (got %dx%d)\n",
                DISPLAY_WIDTH, DISPLAY_HEIGHT,
                gif->SWidth, gif->SHeight);
        DGifCloseFile(gif, &gif_err);
        return AULA_ERR_IMAGE;
    }

    /*
     * Cap frame count at MAX_FRAMES (255).
     * The device uses a uint8 for frame count so 255 is the hard limit.
     * The official software also enforces this.
     */
    int frame_count = gif->ImageCount;
    if (frame_count > MAX_FRAMES) {
        printf("GIF has %d frames, capping at %d.\n", frame_count, MAX_FRAMES);
        frame_count = MAX_FRAMES;
    }

    /* --- Build stream --- */

    /*
     * Total payload = 256-byte header + all frame pixel data.
     * Padded to a multiple of CHUNK_LEN for transmission.
     */
    size_t payload_len = HEADER_LEN + (size_t)frame_count * FRAME_BYTES;
    size_t chunk_count = (payload_len + CHUNK_LEN - 1) / CHUNK_LEN;
    size_t stream_len  = chunk_count * CHUNK_LEN;

    uint8_t *stream = calloc(stream_len, 1);
    if (!stream) {
        fprintf(stderr, "Out of memory allocating %zu bytes\n", stream_len);
        DGifCloseFile(gif, &gif_err);
        return AULA_ERR_IO;
    }

    /*
     * Header:
     *   [0]     = frame count
     *   [1..N]  = per-frame delay bytes
     *
     * Delay is read from GIF metadata for animated GIFs.
     * For static GIFs (1 frame), a default of 100ms is used.
     */
    stream[0] = (uint8_t)frame_count;

    for (i = 0; i < frame_count; i++) {
        double delay_seconds = 0.1;  /* default 100ms for static */

        if (gif->ImageCount > 1) {
            /*
             * Read per-frame delay from GIF Graphic Control Extension.
             * GIF stores delay in centiseconds (1/100 second).
             * GCE layout (4 bytes after the packed flags byte):
             *   [0] = packed flags
             *   [1] = delay low byte  (centiseconds)
             *   [2] = delay high byte (centiseconds)
             *   [3] = transparent color index
             */
            SavedImage *frame = &gif->SavedImages[i];
            int e;
            for (e = 0; e < frame->ExtensionBlockCount; e++) {
                ExtensionBlock *eb = &frame->ExtensionBlocks[e];
                if (eb->Function == GRAPHICS_EXT_FUNC_CODE && eb->ByteCount >= 4) {
                    uint16_t centiseconds = (uint16_t)eb->Bytes[1]
                                          | ((uint16_t)eb->Bytes[2] << 8);
                    if (centiseconds > 0)
                        delay_seconds = centiseconds / 100.0;
                    break;
                }
            }
        }

        stream[1 + i] = seconds_to_delay_byte(delay_seconds);
    }

    /* Write pixel data for each frame starting at offset HEADER_LEN */
    for (i = 0; i < frame_count; i++) {
        size_t frame_offset = HEADER_LEN + (size_t)i * FRAME_BYTES;
        write_frame_rgb565(stream, frame_offset, gif, i);
    }

    DGifCloseFile(gif, &gif_err);

    /* --- Send to device --- */

    printf("Uploading %d frame GIF to slot %d (%zu chunks)...\n",
           frame_count, slot, chunk_count);

    /* CMD 1: enter config mode */
    uint8_t buf[CMD_LEN];
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x18;
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) goto cleanup;

    /*
     * CMD 2: metadata
     * buf[2]   = slot number (overwrites existing content)
     * buf[8-9] = chunk count as little-endian uint16
     */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x72;
    buf[2] = (uint8_t)slot;
    buf[8] = (uint8_t)(chunk_count & 0xFF);
    buf[9] = (uint8_t)((chunk_count >> 8) & 0xFF);
    ret = aula_cmd_exchange(dev, buf);
    if (ret != AULA_OK) goto cleanup;

    /*
     * Wait for device ready signal on EP4 IN.
     * After the metadata command the device sends 128 bytes indicating
     * it is ready to receive pixel data. We must read this before
     * sending chunks or the protocol is out of sync from the start.
     */
    /* I think this is wrong
    {
        uint8_t ready[RESPONSE_LEN];
        int transferred;
        libusb_interrupt_transfer(dev->handle, AULA_EP_IN,
                                  ready, RESPONSE_LEN,
                                  &transferred, 2000);
    }
    */

    /* Send all chunks with progress bar */
    for (i = 0; i < (int)chunk_count; i++) {
        ret = send_chunk(dev, stream + (size_t)i * CHUNK_LEN);
        if (ret != AULA_OK) goto cleanup;

        /*
         * Progress bar. \r returns to start of line without newline
         * so each update overwrites the previous one.
         * fflush forces output since stdout is line-buffered.
         */
        printf("\rChunk %d/%zu [", i, chunk_count);
        int bar_width = 40;
        int filled    = (int)((long)(i) * bar_width / (long)chunk_count);
        int j;
        for (j = 0; j < bar_width; j++)
            putchar(j < filled ? '#' : '-');
        printf("] %.0f%%", (float)(i) / (float)chunk_count * 100.0f);
        fflush(stdout);

        /* 40ms between chunks matches official software timing */
        //usleep(40000);
    }

    /* CMD 3: commit */
    memset(buf, 0, CMD_LEN);
    buf[0] = 0x04;
    buf[1] = 0x02;
    ret = aula_cmd_exchange(dev, buf);

cleanup:
    printf("\n");
    free(stream);
    return ret;
}
