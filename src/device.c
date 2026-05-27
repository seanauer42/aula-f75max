#include <stdio.h>
#include <stdlib.h>
#include "aula.h"

#define CMD_LEN 64

int aula_open(aula_device_t *dev) {
	int ret;

	/* Zero out the struct so no fields are garbage */
	dev->usb_ctx			 = NULL;
	dev->handle				 = NULL;
	dev->kernel_detached_cmd = 0;
	dev->kernel_detached_img = 0;

	/*
	   Initialize a libusb context. libusb is designed to support
	   multiple independent sessions -- the context holds all the
	   state for one session. You always need one before doing anything else
	*/
	ret = libusb_init(&dev->usb_ctx);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize libusb: %s\n", libusb_strerror(ret));
		return AULA_ERR_IO;
	}

	/*
	   Open the device by vendor and product ID.
	   libusb walks the USB device tree and returns a handle
	   to the first matching device. Returns NULL if not found.
	*/
	dev->handle = libusb_open_device_with_vid_pid(
		dev->usb_ctx,
		AULA_VENDOR_ID,
		AULA_PRODUCT_ID
	);

	if (dev->handle == NULL) {
		fprintf(stderr, "Device not found. Is the keyboard plugged in?\n");
		libusb_exit(dev->usb_ctx);
		return AULA_ERR_NOT_FOUND;
	}

	/*
	   Check if the kernel has a driver attached to interface 2.
	   Even if our earlier check showed it unclaimed, we handle
	   this defensively -- always check at runtime.

	   If a kernel driver is attached, we ask libusb to detach it
	   temporarily so we can claim the interface ourselves.
	   We record that we did this so aula_close() can reattach it.
	*/
	if (libusb_kernel_driver_active(dev->handle, AULA_CMD_INTERFACE) == 1) {
		ret = libusb_detach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);
		if (ret < 0) {
			fprintf(stderr, "Failed to detach kernel driver: %s\n",
					libusb_strerror(ret));
			libusb_close(dev->handle);
			libusb_exit(dev->usb_ctx);
			return AULA_ERR_ACCESS;
		}
		dev->kernel_detached_cmd = 1;
	}

	/*
	   Claim the interface. This is like knocking on a door and
	   saying "I'm using this now." Only one process can claim an
	   interface at a time. this will fail if another process already
	   has is claimed.
	*/
	ret = libusb_claim_interface(dev->handle, AULA_CMD_INTERFACE);
	if (ret < 0) {
		fprintf(stderr, "Failed to claim interface %d: %s\n",
				AULA_CMD_INTERFACE, libusb_strerror(ret));

		/* Reattach the kernel driver if we detached it */
		if (dev->kernel_detached_cmd)
			libusb_attach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);

		libusb_close(dev->handle);
		libusb_exit(dev->usb_ctx);
		return AULA_ERR_ACCESS;
	}

	ret = libusb_claim_interface(dev->handle, AULA_CMD_INTERFACE);
	if (ret < 0) {
		fprintf(stderr, "Failed to claim interface %d: %s\n",
				AULA_CMD_INTERFACE, libusb_strerror(ret));

		/* Reattach the kernel driver if we detached it */
		if (dev->kernel_detached_cmd)
			libusb_attach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);

		libusb_close(dev->handle);
		libusb_exit(dev->usb_ctx);
		return AULA_ERR_ACCESS;
	}

	/* Detach and claim image interface (2) */
	if (libusb_kernel_driver_active(dev->handle, AULA_IMG_INTERFACE) == 1) {
		ret = libusb_detach_kernel_driver(dev->handle, AULA_IMG_INTERFACE);
		if (ret < 0) {
			fprintf(stderr, "Failed to detach kernel driver from img interface: %s\n",
					libusb_strerror(ret));
			libusb_release_interface(dev->handle, AULA_CMD_INTERFACE);
			if (dev->kernel_detached_cmd)
				libusb_attach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);
			libusb_close(dev->handle);
			libusb_exit(dev->usb_ctx);
			return AULA_ERR_ACCESS;
		}
		dev->kernel_detached_img = 1;
	}

	ret = libusb_claim_interface(dev->handle, AULA_IMG_INTERFACE);
	if (ret < 0) {
		fprintf(stderr, "Failed to claim img interface %d: %s\n",
				AULA_IMG_INTERFACE, libusb_strerror(ret));
		if (dev->kernel_detached_img)
			libusb_attach_kernel_driver(dev->handle, AULA_IMG_INTERFACE);
		libusb_release_interface(dev->handle, AULA_CMD_INTERFACE);
		if (dev->kernel_detached_cmd)
			libusb_attach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);
		libusb_close(dev->handle);
		libusb_exit(dev->usb_ctx);
		return AULA_ERR_ACCESS;
	}

	return AULA_OK;
}

/*
 * Every SET_REPORT must be followed by a GET_REPORT.
 * The device won't process the next command until
 * it has responded to the previous one.
 */
int aula_cmd_exchange(aula_device_t *dev, uint8_t *buf) {
	uint8_t response[CMD_LEN];
	int ret;

	/* SET_REPORT: send command to device */
	ret = libusb_control_transfer(
			dev->handle,
			0x21,			/* bmRequestType: host->device, class, interface */
			0x09,			/* bRequest: SET_REPORT */
			0x0300,			/* wValue: report type 3, report ID 0 */
			AULA_CMD_INTERFACE, /* wIndex: interface 3 */
			buf,
			CMD_LEN,
			1000
			);
	if (ret < 0) {
		fprintf(stderr, "SET_REPORT failed: %s\n", libusb_strerror(ret));
		return AULA_ERR_IO;
	}

	/* GET_REPORT: read device acknowledgement */
	ret = libusb_control_transfer(
			dev->handle,
			0xa1,			/* bmRequestType: device->host, class, interface */
			0x01,			/* bRequest: GET_REPORT */
			0x0300,			/* wValue */
			AULA_CMD_INTERFACE, /* wIndex */
			response,
			CMD_LEN,
			1000
			);
	if (ret < 0) {
		fprintf(stderr, "GET_REPORT failed: %s\n", libusb_strerror(ret));
		return AULA_ERR_IO;
	}
	return AULA_OK;
}

void aula_close(aula_device_t *dev) {
	if (dev->handle == NULL)
		return;

	/*
	   Always release the interface before closing.
	   Skipping this can leave the device in a weird state
	   until it's unplugged and replugged
	*/
	libusb_release_interface(dev->handle, AULA_CMD_INTERFACE);
	libusb_release_interface(dev->handle, AULA_IMG_INTERFACE);

	/*
	   If we detached the kernel driver on open, reattach it now
	   so the OS regains normal control of the interface.
	*/
	if (dev->kernel_detached_cmd)
		libusb_attach_kernel_driver(dev->handle, AULA_CMD_INTERFACE);
	if (dev->kernel_detached_img)
		libusb_attach_kernel_driver(dev->handle, AULA_IMG_INTERFACE);

	libusb_close(dev->handle);
	libusb_exit(dev->usb_ctx);

	/* Zero out the struct so it can't be accidentally reused */
	dev->handle				 = NULL;
	dev->usb_ctx			 = NULL;
	dev->kernel_detached_cmd = 0;
	dev->kernel_detached_img = 0;
}
