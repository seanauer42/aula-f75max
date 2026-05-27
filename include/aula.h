#ifndef AULA_H
#define AULA_H

#include <stdint.h>
#include <libusb-1.0/libusb.h>

// USB identifiers
#define AULA_VENDOR_ID 0x0c45
#define AULA_PRODUCT_ID 0x800a

// Interface and endpoints
#define AULA_CMD_INTERFACE  3
#define AULA_IMG_INTERFACE	2
#define AULA_EP_OUT         0x03 /* Keep for display interface 2 */
#define AULA_EP_IN          0x84

// Display properties
#define AULA_DISPLAY_WIDTH  128
#define AULA_DISPLAY_HEIGHT 128
#define AULA_DISPLAY_PIXELS (AULA_DISPLAY_WIDTH * AULA_DISPLAY_HEIGHT)

// Return codes
#define AULA_OK              0
#define AULA_ERR_NOT_FOUND  -1
#define AULA_ERR_ACCESS     -2
#define AULA_ERR_IO         -3
#define AULA_ERR_IMAGE      -4

/*
 * Represents an open connection to the keyboard.
 * Callers treat this as an opaque handle -- don't
 * access the fields directly
 */
typedef struct {
    libusb_context          *usb_ctx;
    libusb_device_handle    *handle;
	int						kernel_detached_cmd;
    int                     kernel_detached_img;
} aula_device_t;

// device.c
int aula_open(aula_device_t *dev);
void aula_close(aula_device_t *dev);

// display.c
int aula_send_gif(aula_device_t *dev, const char *path);

int aula_cmd_exchange(aula_device_t *dev, uint8_t *buf);

// time_sync.c
int aula_sync_time(aula_device_t *dev, int year, int mon, int mday,
		int hour, int min, int sec);

// reset.c
int aula_factory_reset(aula_device_t *dev);


#endif /* AULA_H */
