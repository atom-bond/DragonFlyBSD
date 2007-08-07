/*
 * $NetBSD: uplcom.c,v 1.21 2001/11/13 06:24:56 lukem Exp $
 * $FreeBSD: src/sys/dev/usb/uplcom.c,v 1.39 2006/09/07 00:06:42 imp Exp $
 * $DragonFly: src/sys/dev/usbmisc/uplcom/uplcom.c,v 1.19 2007/08/07 10:42:41 hasso Exp $
 */

/*-
 * Copyright (c) 2001-2003, 2005 Shunsuke Akiyama <akiyama@jp.FreeBSD.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Ichiro FUKUHARA (ichiro@ichiro.org).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This driver supports several devices devices driven by Prolific PL-2303
 * (known also as PL-2303H), PL-2303X and PL-2303HX USB-to-RS232 bridge chip.
 * The devices are sold under many different brand names.
 *
 * Datasheets are available at Prolific www site at http://www.prolific.com.tw
 * The datasheets don't contain full programming information for the chip.
 *
 * PL-2303HX has the same features as PL-2303X (at least from the point of
 * view of device driver) but is pin-to-pin compatible with PL-2303.
 *
 * There are several differences between PL-2303 and PL-2303(H)X.
 * PL-2303(H)X can do higher bitrate in bulk mode, has _probably_
 * different command for controlling CRTSCTS and needs special
 * sequence of commands for initialization which aren't also
 * documented in the datasheet.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/ioccom.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <bus/usb/usb.h>
#include <bus/usb/usbcdc.h>

#include <bus/usb/usbdi.h>
#include <bus/usb/usbdi_util.h>
#include <bus/usb/usbdevs.h>
#include <bus/usb/usb_quirks.h>

#include "../ucom/ucomvar.h"

SYSCTL_NODE(_hw_usb, OID_AUTO, uplcom, CTLFLAG_RW, 0, "USB uplcom");
#ifdef USB_DEBUG
static int	uplcomdebug = 0;
SYSCTL_INT(_hw_usb_uplcom, OID_AUTO, debug, CTLFLAG_RW,
	   &uplcomdebug, 0, "uplcom debug level");

#define DPRINTFN(n, x)	do { \
				if (uplcomdebug > (n)) \
					kprintf x; \
			} while (0)
#else
#define DPRINTFN(n, x)
#endif
#define DPRINTF(x) DPRINTFN(0, x)

#define UPLCOM_MODVER			1	/* module version */

#define	UPLCOM_CONFIG_INDEX		0
#define	UPLCOM_IFACE_INDEX		0
#define	UPLCOM_SECOND_IFACE_INDEX	1

#ifndef UPLCOM_INTR_INTERVAL
#define UPLCOM_INTR_INTERVAL		100	/* ms */
#endif

#define	UPLCOM_SET_REQUEST		0x01
#define	UPLCOM_SET_CRTSCTS		0x41
#define	UPLCOM_SET_CRTSCTS_PL2303X	0x61
#define RSAQ_STATUS_CTS			0x80
#define RSAQ_STATUS_DSR			0x02
#define RSAQ_STATUS_DCD			0x01

#define TYPE_PL2303			0
#define TYPE_PL2303X			1

struct	uplcom_softc {
	struct ucom_softc	sc_ucom;

	int			sc_iface_number;	/* interface number */

	usbd_interface_handle	sc_intr_iface;	/* interrupt interface */
	int			sc_intr_number;	/* interrupt number */
	usbd_pipe_handle	sc_intr_pipe;	/* interrupt pipe */
	u_char			*sc_intr_buf;	/* interrupt buffer */
	int			sc_isize;

	usb_cdc_line_state_t	sc_line_state;	/* current line state */
	u_char			sc_dtr;		/* current DTR state */
	u_char			sc_rts;		/* current RTS state */
	u_char			sc_status;

	u_char			sc_lsr;		/* Local status register */
	u_char			sc_msr;		/* uplcom status register */

	int			sc_chiptype;	/* Type of chip */

	struct task		sc_task;
};

/*
 * These are the maximum number of bytes transferred per frame.
 * The output buffer size cannot be increased due to the size encoding.
 */
#define UPLCOMIBUFSIZE 256
#define UPLCOMOBUFSIZE 256

static	usbd_status uplcom_reset(struct uplcom_softc *);
static	usbd_status uplcom_set_line_coding(struct uplcom_softc *,
					   usb_cdc_line_state_t *);
static	usbd_status uplcom_set_crtscts(struct uplcom_softc *);
static	void uplcom_intr(usbd_xfer_handle, usbd_private_handle, usbd_status);

static	void uplcom_set(void *, int, int, int);
static	void uplcom_dtr(struct uplcom_softc *, int);
static	void uplcom_rts(struct uplcom_softc *, int);
static	void uplcom_break(struct uplcom_softc *, int);
static	void uplcom_set_line_state(struct uplcom_softc *);
static	void uplcom_get_status(void *, int, u_char *, u_char *);
#if 0 /* TODO */
static	int  uplcom_ioctl(void *, int, u_long, caddr_t, int, struct thread *);
#endif
static	int  uplcom_param(void *, int, struct termios *);
static	int  uplcom_open(void *, int);
static	void uplcom_close(void *, int);
static	void uplcom_notify(void *, int);

struct ucom_callback uplcom_callback = {
	uplcom_get_status,
	uplcom_set,
	uplcom_param,
	NULL, /* uplcom_ioctl, TODO */
	uplcom_open,
	uplcom_close,
	NULL,
	NULL
};

static const struct usb_devno uplcom_devs[] = {
	/* Alcatel One Touch 535/735 phones */
	{ USB_VENDOR_ALCATEL, USB_PRODUCT_ALCATEL_OT535 },
	/* Alcor AU9720 USB to serial controller */
	{ USB_VENDOR_ALCOR, USB_PRODUCT_ALCOR_AU9720 },
	/* Anchor serial */
	{ USB_VENDOR_ANCHOR, USB_PRODUCT_ANCHOR_SERIAL },
	/* Aten UC232A USB to serial adapter */
	{ USB_VENDOR_ATEN, USB_PRODUCT_ATEN_UC232A },
	/* Belkin F5U257 USB to serial adapter */
	{ USB_VENDOR_BELKIN, USB_PRODUCT_BELKIN_F5U257 },
	/* ELECOM UC-SGT USB to serial adapters */
	{ USB_VENDOR_ELECOM, USB_PRODUCT_ELECOM_UCSGT },
	{ USB_VENDOR_ELECOM, USB_PRODUCT_ELECOM_UCSGT0 },
	/* HAL Corporation Crossam2+USB */
	{ USB_VENDOR_HAL, USB_PRODUCT_HAL_IMR001 },
	/* Huawei UMTS/HSDPA adapters */
	{ USB_VENDOR_HUAWEI, USB_PRODUCT_HUAWEI_MOBILE },
	/* I/O DATA USB-RSAQ USB to serial cable */
	{ USB_VENDOR_IODATA, USB_PRODUCT_IODATA_USBRSAQ },
	/* I/O DATA USB-RSAQ5 USB to serial cable */
	{ USB_VENDOR_IODATA, USB_PRODUCT_IODATA_USBRSAQ5 },
	/* Itegno GSM/GPRS modem */
	{ USB_VENDOR_ITEGNO, USB_PRODUCT_ITEGNO_GSM },
	/* Itegno CDMA 1x card */
	{ USB_VENDOR_ITEGNO, USB_PRODUCT_ITEGNO_CDMA },
	/* Leadtek 9531 GPS */
	{ USB_VENDOR_LEADTEK, USB_PRODUCT_LEADTEK_9531 },
	/* Sitecom USB to serial adapter */
	{ USB_VENDOR_MCT, USB_PRODUCT_MCT_SITECOM_USB232 },
	/* Mobile Action MA-620 IrDA */
	{ USB_VENDOR_MOBILEACTION, USB_PRODUCT_MOBILEACTION_MA620 },
	/* Willcom W-SIM */
	{ USB_VENDOR_NETINDEX, USB_PRODUCT_NETINDEX_WSIM },
	/* Nokia CA-42 USB data cable clones*/
	{ USB_VENDOR_NOKIA, USB_PRODUCT_NOKIA_CA42 },
	/* Panasonic 50" touch panel */
	{ USB_VENDOR_PANASONIC, USB_PRODUCT_PANASONIC_TYTP50P6S },
	/* PLX CA-42 USB data cable clone */
	{ USB_VENDOR_PLX, USB_PRODUCT_PLX_CA42 },
	/* Unbranded DCU-11 clone */
	{ USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_DCU11 },
	/* IOGEAR/ATEN UC-232A, ST Lab USB-SERIAL-X etc */
	{ USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_PL2303 },
	/* Microsoft OEM Pharos 360 GPS */
	{ USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_PL2303X },
	/* I/O DATA USB-RSAQ2 USB to serial cable */
	{ USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_RSAQ2 },
	/* I/O DATA USB-RSAQ3 USB to serial cable */
	{ USB_VENDOR_PROLIFIC, USB_PRODUCT_PROLIFIC_RSAQ3 },
	/* RADIOSHACK USB cable */
	{ USB_VENDOR_RADIOSHACK, USB_PRODUCT_RADIOSHACK_USBCABLE },
	/* RATOC REX-USB60 USB to serial cable */
	{ USB_VENDOR_RATOC, USB_PRODUCT_RATOC_REXUSB60 },
	/* Sagem USB data cables */
	{ USB_VENDOR_SAGEM, USB_PRODUCT_SAGEM_USBSERIAL },
	/* Samsung I330 smartphone cradle */
	{ USB_VENDOR_SAMSUNG, USB_PRODUCT_SAMSUNG_I330 },
	/* Siemens SX1 cellphone */
	{ USB_VENDOR_SIEMENS3, USB_PRODUCT_SIEMENS3_SX1 },
	/* Siemens x65 series cellphones */
	{ USB_VENDOR_SIEMENS3, USB_PRODUCT_SIEMENS3_X65 },
	/* Siemens x75 series cellphones */
	{ USB_VENDOR_SIEMENS3, USB_PRODUCT_SIEMENS3_X75 },
	/* Sitecom USB to serial cable */
	{ USB_VENDOR_SITECOM, USB_PRODUCT_SITECOM_CN104 },
	/* Sony-Ericsson DCU-10 and DCU-11 USB data cables */
	{ USB_VENDOR_SUSTEEN, USB_PRODUCT_SUSTEEN_DCU10 },
	/* Susteen Datapilot Universal-2 Phone Cable */
	{ USB_VENDOR_SUSTEEN, USB_PRODUCT_SUSTEEN_U2 },
	/* SOURCENEXT KeikaiDenwa 8 */
	{ USB_VENDOR_SOURCENEXT, USB_PRODUCT_SOURCENEXT_KEIKAI8 },
	/* SOURCENEXT KeikaiDenwa 8 with charger */
	{ USB_VENDOR_SOURCENEXT, USB_PRODUCT_SOURCENEXT_KEIKAI8_CHG },
	/* Speed Dragon Multimedia MS3303H USB to serial controller */
	{ USB_VENDOR_SPEEDDRAGON, USB_PRODUCT_SPEEDDRAGON_MS3303H },
	/* Syntech CPT-8001C barcode scanner USB IR cradle */
	{ USB_VENDOR_SYNTECH, USB_PRODUCT_SYNTECH_SERIAL },
	/* TDK USB-PHS adapter UHA6400 */
	{ USB_VENDOR_TDK, USB_PRODUCT_TDK_UHA6400 },
	/* TDK USB-PDC adapter UPA9664 */
	{ USB_VENDOR_TDK, USB_PRODUCT_TDK_UPA9664 },
	/* Tripp-Lite U209-000-R USB to serial cable */
	{ USB_VENDOR_TRIPPLITE, USB_PRODUCT_TRIPPLITE_U209 },
	{ 0, 0 }
};

static device_probe_t uplcom_match;
static device_attach_t uplcom_attach;
static device_detach_t uplcom_detach;

static device_method_t uplcom_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, uplcom_match),
	DEVMETHOD(device_attach, uplcom_attach),
	DEVMETHOD(device_detach, uplcom_detach),
	{ 0, 0 }
};

static driver_t uplcom_driver = {
	"ucom",
	uplcom_methods,
	sizeof (struct uplcom_softc)
};

DRIVER_MODULE(uplcom, uhub, uplcom_driver, ucom_devclass, usbd_driver_load, 0);
MODULE_DEPEND(uplcom, usb, 1, 1, 1);
MODULE_DEPEND(uplcom, ucom, UCOM_MINVER, UCOM_PREFVER, UCOM_MAXVER);
MODULE_VERSION(uplcom, UPLCOM_MODVER);

static int	uplcominterval = UPLCOM_INTR_INTERVAL;

static int
sysctl_hw_usb_uplcom_interval(SYSCTL_HANDLER_ARGS)
{
	int err, val;

	val = uplcominterval;
	err = sysctl_handle_int(oidp, &val, sizeof(val), req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	if (0 < val && val <= 1000)
		uplcominterval = val;
	else
		err = EINVAL;

	return (err);
}

SYSCTL_PROC(_hw_usb_uplcom, OID_AUTO, interval, CTLTYPE_INT | CTLFLAG_RW,
	    0, sizeof(int), sysctl_hw_usb_uplcom_interval,
	    "I", "uplcom interrupt pipe interval");

static int
uplcom_match(device_t self)
{
	struct usb_attach_arg *uaa = device_get_ivars(self);

	if (uaa->iface != NULL)
		return (UMATCH_NONE);

	return (usb_lookup(uplcom_devs, uaa->vendor, uaa->product) != NULL ?
		UMATCH_VENDOR_PRODUCT : UMATCH_NONE);
}

static int
uplcom_attach(device_t self)
{
	struct uplcom_softc *sc = device_get_softc(self);
	struct usb_attach_arg *uaa = device_get_ivars(self);
	usbd_device_handle dev = uaa->device;
	usb_device_descriptor_t *dd;
	struct ucom_softc *ucom;
	usb_config_descriptor_t *cdesc;
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	char *devinfo;
	const char *devname;
	usbd_status err;
	int i;

	devinfo = kmalloc(1024, M_USBDEV, M_INTWAIT);
	ucom = &sc->sc_ucom;

	bzero(sc, sizeof (struct uplcom_softc));

	usbd_devinfo(dev, 0, devinfo);
	ucom->sc_dev = self;
	device_set_desc_copy(self, devinfo);

	ucom->sc_udev = dev;
	ucom->sc_iface = uaa->iface;

	devname = device_get_nameunit(ucom->sc_dev);
	kprintf("%s: %s\n", devname, devinfo);

	DPRINTF(("uplcom attach: sc = %p\n", sc));

	dd = usbd_get_device_descriptor(uaa->device);

	if (!dd)
		goto error;

	/*
	 * Determine chip type with algorithm sequence taken from the
	 * Linux driver as I'm not aware of any better method. Device
	 * release number in chip could be (and in fact is in many cases)
	 * replaced by the contents of external EEPROM etc.
	 */
	else if (dd->bDeviceClass == 0x02)
		sc->sc_chiptype = TYPE_PL2303;
	else if (dd->bMaxPacketSize == 0x40)
		sc->sc_chiptype = TYPE_PL2303X;
	else
		sc->sc_chiptype = TYPE_PL2303;

#ifdef USB_DEBUG
	/* print the chip type */
	if (sc->sc_chiptype == TYPE_PL2303X) {
		DPRINTF(("uplcom_attach: chiptype 2303X\n"));
	} else {
		DPRINTF(("uplcom_attach: chiptype 2303\n"));
	}
#endif

	/* initialize endpoints */
	ucom->sc_bulkin_no = ucom->sc_bulkout_no = -1;
	sc->sc_intr_number = -1;
	sc->sc_intr_pipe = NULL;

	/* Move the device into the configured state. */
	err = usbd_set_config_index(dev, UPLCOM_CONFIG_INDEX, 1);
	if (err) {
		kprintf("%s: failed to set configuration: %s\n",
			devname, usbd_errstr(err));
		ucom->sc_dying = 1;
		goto error;
	}

	/* get the config descriptor */
	cdesc = usbd_get_config_descriptor(ucom->sc_udev);

	if (cdesc == NULL) {
		kprintf("%s: failed to get configuration descriptor\n",
			device_get_nameunit(ucom->sc_dev));
		ucom->sc_dying = 1;
		goto error;
	}

	/* get the (first/common) interface */
	err = usbd_device2interface_handle(dev, UPLCOM_IFACE_INDEX,
					   &ucom->sc_iface);
	if (err) {
		kprintf("%s: failed to get interface: %s\n",
			devname, usbd_errstr(err));
		ucom->sc_dying = 1;
		goto error;
	}

	/* Find the interrupt endpoints */

	id = usbd_get_interface_descriptor(ucom->sc_iface);
	sc->sc_iface_number = id->bInterfaceNumber;

	for (i = 0; i < id->bNumEndpoints; i++) {
		ed = usbd_interface2endpoint_descriptor(ucom->sc_iface, i);
		if (ed == NULL) {
			kprintf("%s: no endpoint descriptor for %d\n",
				device_get_nameunit(ucom->sc_dev), i);
			ucom->sc_dying = 1;
			goto error;
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_INTERRUPT) {
			sc->sc_intr_number = ed->bEndpointAddress;
			sc->sc_isize = UGETW(ed->wMaxPacketSize);
		}
	}

	if (sc->sc_intr_number == -1) {
		kprintf("%s: Could not find interrupt in\n",
			device_get_nameunit(ucom->sc_dev));
		ucom->sc_dying = 1;
		goto error;
	}

	/* keep interface for interrupt */
	sc->sc_intr_iface = ucom->sc_iface;

	/*
	 * USB-RSAQ1 has two interface
	 *
	 *  USB-RSAQ1       | USB-RSAQ2
	 * -----------------+-----------------
	 * Interface 0      |Interface 0
	 *  Interrupt(0x81) | Interrupt(0x81)
	 * -----------------+ BulkIN(0x02)
	 * Interface 1	    | BulkOUT(0x83)
	 *   BulkIN(0x02)   |
	 *   BulkOUT(0x83)  |
	 */
	if (cdesc->bNumInterface == 2) {
		err = usbd_device2interface_handle(dev,
						   UPLCOM_SECOND_IFACE_INDEX,
						   &ucom->sc_iface);
		if (err) {
			kprintf("%s: failed to get second interface: %s\n",
				devname, usbd_errstr(err));
			ucom->sc_dying = 1;
			goto error;
		}
	}

	/* Find the bulk{in,out} endpoints */

	id = usbd_get_interface_descriptor(ucom->sc_iface);
	sc->sc_iface_number = id->bInterfaceNumber;

	for (i = 0; i < id->bNumEndpoints; i++) {
		ed = usbd_interface2endpoint_descriptor(ucom->sc_iface, i);
		if (ed == NULL) {
			kprintf("%s: no endpoint descriptor for %d\n",
				device_get_nameunit(ucom->sc_dev), i);
			ucom->sc_dying = 1;
			goto error;
		}

		if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_IN &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			ucom->sc_bulkin_no = ed->bEndpointAddress;
		} else if (UE_GET_DIR(ed->bEndpointAddress) == UE_DIR_OUT &&
		    UE_GET_XFERTYPE(ed->bmAttributes) == UE_BULK) {
			ucom->sc_bulkout_no = ed->bEndpointAddress;
		}
	}

	if (ucom->sc_bulkin_no == -1) {
		kprintf("%s: Could not find data bulk in\n",
			device_get_nameunit(ucom->sc_dev));
		ucom->sc_dying = 1;
		goto error;
	}

	if (ucom->sc_bulkout_no == -1) {
		kprintf("%s: Could not find data bulk out\n",
			device_get_nameunit(ucom->sc_dev));
		ucom->sc_dying = 1;
		goto error;
	}

	sc->sc_dtr = sc->sc_rts = -1;
	ucom->sc_parent = sc;
	ucom->sc_portno = UCOM_UNK_PORTNO;
	/* bulkin, bulkout set above */
	ucom->sc_ibufsize = UPLCOMIBUFSIZE;
	ucom->sc_obufsize = UPLCOMOBUFSIZE;
	ucom->sc_ibufsizepad = UPLCOMIBUFSIZE;
	ucom->sc_opkthdrlen = 0;
	ucom->sc_callback = &uplcom_callback;

	err = uplcom_reset(sc);

	if (err) {
		kprintf("%s: reset failed: %s\n",
		       device_get_nameunit(ucom->sc_dev), usbd_errstr(err));
		ucom->sc_dying = 1;
		goto error;
	}

	DPRINTF(("uplcom: in = 0x%x, out = 0x%x, intr = 0x%x\n",
		 ucom->sc_bulkin_no, ucom->sc_bulkout_no, sc->sc_intr_number));

	TASK_INIT(&sc->sc_task, 0, uplcom_notify, sc);
	ucom_attach(&sc->sc_ucom);

	kfree(devinfo, M_USBDEV);
	return 0;

error:
	kfree(devinfo, M_USBDEV);
	return ENXIO;
}

static int
uplcom_detach(device_t self)
{
	struct uplcom_softc *sc = device_get_softc(self);
	int rv = 0;

	DPRINTF(("uplcom_detach: sc = %p\n", sc));

	if (sc->sc_intr_pipe != NULL) {
		usbd_abort_pipe(sc->sc_intr_pipe);
		usbd_close_pipe(sc->sc_intr_pipe);
		kfree(sc->sc_intr_buf, M_USBDEV);
		sc->sc_intr_pipe = NULL;
	}

	sc->sc_ucom.sc_dying = 1;

	rv = ucom_detach(&sc->sc_ucom);

	return (rv);
}

static usbd_status
uplcom_reset(struct uplcom_softc *sc)
{
	usb_device_request_t req;
	usbd_status err;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = UPLCOM_SET_REQUEST;
	USETW(req.wValue, 0);
	USETW(req.wIndex, sc->sc_iface_number);
	USETW(req.wLength, 0);

	err = usbd_do_request(sc->sc_ucom.sc_udev, &req, 0);
	if (err) {
		kprintf("%s: uplcom_reset: %s\n",
		       device_get_nameunit(sc->sc_ucom.sc_dev), usbd_errstr(err));
		return (EIO);
	}

	return (0);
}

struct pl2303x_init {
	uint8_t		req_type;
	uint8_t		request;
	uint16_t	value;
	uint16_t	index;
	uint16_t	length;
};

static const struct pl2303x_init pl2303x[] = {
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8484,    0, 0 },
	{ UT_WRITE_VENDOR_DEVICE, UPLCOM_SET_REQUEST, 0x0404,    0, 0 },
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8484,    0, 0 },
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8383,    0, 0 },
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8484,    0, 0 },
	{ UT_WRITE_VENDOR_DEVICE, UPLCOM_SET_REQUEST, 0x0404,    1, 0 },
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8484,    0, 0 },
	{ UT_READ_VENDOR_DEVICE,  UPLCOM_SET_REQUEST, 0x8383,    0, 0 },
	{ UT_WRITE_VENDOR_DEVICE, UPLCOM_SET_REQUEST,      0,    1, 0 },
	{ UT_WRITE_VENDOR_DEVICE, UPLCOM_SET_REQUEST,      1,    0, 0 },
	{ UT_WRITE_VENDOR_DEVICE, UPLCOM_SET_REQUEST,      2, 0x44, 0 }
};
#define N_PL2302X_INIT	(sizeof(pl2303x)/sizeof(pl2303x[0]))

static usbd_status
uplcom_pl2303x_init(struct uplcom_softc *sc)
{
	usb_device_request_t req;
	usbd_status err;
	int i;

	for (i = 0; i < N_PL2302X_INIT; i++) {
		req.bmRequestType = pl2303x[i].req_type;
		req.bRequest = pl2303x[i].request;
		USETW(req.wValue, pl2303x[i].value);
		USETW(req.wIndex, pl2303x[i].index);
		USETW(req.wLength, pl2303x[i].length);

		err = usbd_do_request(sc->sc_ucom.sc_udev, &req, 0);
		if (err) {
			kprintf("%s: uplcom_pl2303x_init: %d: %s\n",
				device_get_nameunit(sc->sc_ucom.sc_dev), i,
				usbd_errstr(err));
			return (EIO);
		}
	}

	return (0);
}

static void
uplcom_set_line_state(struct uplcom_softc *sc)
{
	usb_device_request_t req;
	int ls;
	usbd_status err;

	ls = (sc->sc_dtr ? UCDC_LINE_DTR : 0) |
		(sc->sc_rts ? UCDC_LINE_RTS : 0);
	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UCDC_SET_CONTROL_LINE_STATE;
	USETW(req.wValue, ls);
	USETW(req.wIndex, sc->sc_iface_number);
	USETW(req.wLength, 0);

	err = usbd_do_request(sc->sc_ucom.sc_udev, &req, 0);
	if (err)
		kprintf("%s: uplcom_set_line_status: %s\n",
		       device_get_nameunit(sc->sc_ucom.sc_dev), usbd_errstr(err));
}

static void
uplcom_set(void *addr, int portno, int reg, int onoff)
{
	struct uplcom_softc *sc = addr;

	switch (reg) {
	case UCOM_SET_DTR:
		uplcom_dtr(sc, onoff);
		break;
	case UCOM_SET_RTS:
		uplcom_rts(sc, onoff);
		break;
	case UCOM_SET_BREAK:
		uplcom_break(sc, onoff);
		break;
	default:
		break;
	}
}

static void
uplcom_dtr(struct uplcom_softc *sc, int onoff)
{
	DPRINTF(("uplcom_dtr: onoff = %d\n", onoff));

	if (sc->sc_dtr == onoff)
		return;
	sc->sc_dtr = onoff;

	uplcom_set_line_state(sc);
}

static void
uplcom_rts(struct uplcom_softc *sc, int onoff)
{
	DPRINTF(("uplcom_rts: onoff = %d\n", onoff));

	if (sc->sc_rts == onoff)
		return;
	sc->sc_rts = onoff;

	uplcom_set_line_state(sc);
}

static void
uplcom_break(struct uplcom_softc *sc, int onoff)
{
	usb_device_request_t req;
	usbd_status err;

	DPRINTF(("uplcom_break: onoff = %d\n", onoff));

	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UCDC_SEND_BREAK;
	USETW(req.wValue, onoff ? UCDC_BREAK_ON : UCDC_BREAK_OFF);
	USETW(req.wIndex, sc->sc_iface_number);
	USETW(req.wLength, 0);

	err = usbd_do_request(sc->sc_ucom.sc_udev, &req, 0);
	if (err)
		kprintf("%s: uplcom_break: %s\n",
		       device_get_nameunit(sc->sc_ucom.sc_dev), usbd_errstr(err));
}

static usbd_status
uplcom_set_crtscts(struct uplcom_softc *sc)
{
	usb_device_request_t req;
	usbd_status err;

	DPRINTF(("uplcom_set_crtscts: on\n"));

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = UPLCOM_SET_REQUEST;
	USETW(req.wValue, 0);
	if (sc->sc_chiptype == TYPE_PL2303X)
		USETW(req.wIndex, UPLCOM_SET_CRTSCTS_PL2303X);
	else
		USETW(req.wIndex, UPLCOM_SET_CRTSCTS);
	USETW(req.wLength, 0);

	err = usbd_do_request(sc->sc_ucom.sc_udev, &req, 0);
	if (err) {
		kprintf("%s: uplcom_set_crtscts: %s\n",
		       device_get_nameunit(sc->sc_ucom.sc_dev), usbd_errstr(err));
		return (err);
	}

	return (USBD_NORMAL_COMPLETION);
}

static usbd_status
uplcom_set_line_coding(struct uplcom_softc *sc, usb_cdc_line_state_t *state)
{
	usb_device_request_t req;
	usbd_status err;

	DPRINTF((
"uplcom_set_line_coding: rate = %d, fmt = %d, parity = %d bits = %d\n",
		 UGETDW(state->dwDTERate), state->bCharFormat,
		 state->bParityType, state->bDataBits));

	if (memcmp(state, &sc->sc_line_state, UCDC_LINE_STATE_LENGTH) == 0) {
		DPRINTF(("uplcom_set_line_coding: already set\n"));
		return (USBD_NORMAL_COMPLETION);
	}

	req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req.bRequest = UCDC_SET_LINE_CODING;
	USETW(req.wValue, 0);
	USETW(req.wIndex, sc->sc_iface_number);
	USETW(req.wLength, UCDC_LINE_STATE_LENGTH);

	err = usbd_do_request(sc->sc_ucom.sc_udev, &req, state);
	if (err) {
		kprintf("%s: uplcom_set_line_coding: %s\n",
		       device_get_nameunit(sc->sc_ucom.sc_dev), usbd_errstr(err));
		return (err);
	}

	sc->sc_line_state = *state;

	return (USBD_NORMAL_COMPLETION);
}

static const int uplcom_rates[] = {
	75, 150, 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600, 14400,
	19200, 28800, 38400, 57600, 115200,
	/*
	 * Higher speeds are probably possible. PL2303X supports up to
	 * 6Mb and can set any rate
	 */
	230400, 460800, 614400, 921600,	1228800
};
#define N_UPLCOM_RATES	(sizeof(uplcom_rates)/sizeof(uplcom_rates[0]))

static int
uplcom_param(void *addr, int portno, struct termios *t)
{
	struct uplcom_softc *sc = addr;
	usbd_status err;
	usb_cdc_line_state_t ls;
	int i;

	DPRINTF(("uplcom_param: sc = %p\n", sc));

	/* Check requested baud rate */
	for (i = 0; i < N_UPLCOM_RATES; i++)
		if (uplcom_rates[i] == t->c_ospeed)
			break;
	if (i == N_UPLCOM_RATES) {
		DPRINTF(("uplcom_param: bad baud rate (%d)\n", t->c_ospeed));
		return (EIO);
	}

	USETDW(ls.dwDTERate, t->c_ospeed);
	if (ISSET(t->c_cflag, CSTOPB))
		ls.bCharFormat = UCDC_STOP_BIT_2;
	else
		ls.bCharFormat = UCDC_STOP_BIT_1;
	if (ISSET(t->c_cflag, PARENB)) {
		if (ISSET(t->c_cflag, PARODD))
			ls.bParityType = UCDC_PARITY_ODD;
		else
			ls.bParityType = UCDC_PARITY_EVEN;
	} else
		ls.bParityType = UCDC_PARITY_NONE;
	switch (ISSET(t->c_cflag, CSIZE)) {
	case CS5:
		ls.bDataBits = 5;
		break;
	case CS6:
		ls.bDataBits = 6;
		break;
	case CS7:
		ls.bDataBits = 7;
		break;
	case CS8:
		ls.bDataBits = 8;
		break;
	}

	err = uplcom_set_line_coding(sc, &ls);
	if (err)
		return (EIO);

	if (ISSET(t->c_cflag, CRTSCTS)) {
		err = uplcom_set_crtscts(sc);
		if (err)
			return (EIO);
	}

	return (0);
}

static int
uplcom_open(void *addr, int portno)
{
	struct uplcom_softc *sc = addr;
	int err;

	if (sc->sc_ucom.sc_dying)
		return (ENXIO);

	DPRINTF(("uplcom_open: sc = %p\n", sc));

	if (sc->sc_intr_number != -1 && sc->sc_intr_pipe == NULL) {
		sc->sc_status = 0; /* clear status bit */
		sc->sc_intr_buf = kmalloc(sc->sc_isize, M_USBDEV, M_WAITOK);
		err = usbd_open_pipe_intr(sc->sc_intr_iface,
					  sc->sc_intr_number,
					  USBD_SHORT_XFER_OK,
					  &sc->sc_intr_pipe,
					  sc,
					  sc->sc_intr_buf,
					  sc->sc_isize,
					  uplcom_intr,
					  uplcominterval);
		if (err) {
			kprintf("%s: cannot open interrupt pipe (addr %d)\n",
			       device_get_nameunit(sc->sc_ucom.sc_dev),
			       sc->sc_intr_number);
			return (EIO);
		}
	}

	if (sc->sc_chiptype == TYPE_PL2303X)
		return (uplcom_pl2303x_init(sc));

	return (0);
}

static void
uplcom_close(void *addr, int portno)
{
	struct uplcom_softc *sc = addr;
	int err;

	if (sc->sc_ucom.sc_dying)
		return;

	DPRINTF(("uplcom_close: close\n"));

	if (sc->sc_intr_pipe != NULL) {
		err = usbd_abort_pipe(sc->sc_intr_pipe);
		if (err)
			kprintf("%s: abort interrupt pipe failed: %s\n",
			       device_get_nameunit(sc->sc_ucom.sc_dev),
			       usbd_errstr(err));
		err = usbd_close_pipe(sc->sc_intr_pipe);
		if (err)
			kprintf("%s: close interrupt pipe failed: %s\n",
			       device_get_nameunit(sc->sc_ucom.sc_dev),
			       usbd_errstr(err));
		kfree(sc->sc_intr_buf, M_USBDEV);
		sc->sc_intr_pipe = NULL;
	}
}

static void
uplcom_intr(usbd_xfer_handle xfer, usbd_private_handle priv, usbd_status status)
{
	struct uplcom_softc *sc = priv;
	u_char *buf = sc->sc_intr_buf;
	u_char pstatus;

	if (sc->sc_ucom.sc_dying)
		return;

	if (status != USBD_NORMAL_COMPLETION) {
		if (status == USBD_NOT_STARTED || status == USBD_CANCELLED)
			return;

		DPRINTF(("%s: uplcom_intr: abnormal status: %s\n",
			device_get_nameunit(sc->sc_ucom.sc_dev),
			usbd_errstr(status)));
		usbd_clear_endpoint_stall_async(sc->sc_intr_pipe);
		return;
	}

	DPRINTF(("%s: uplcom status = %02x\n",
		 device_get_nameunit(sc->sc_ucom.sc_dev), buf[8]));

	sc->sc_lsr = sc->sc_msr = 0;
	pstatus = buf[8];
	if (ISSET(pstatus, RSAQ_STATUS_CTS))
		sc->sc_msr |= UMSR_CTS;
	else
		sc->sc_msr &= ~UMSR_CTS;
	if (ISSET(pstatus, RSAQ_STATUS_DSR))
		sc->sc_msr |= UMSR_DSR;
	else
		sc->sc_msr &= ~UMSR_DSR;
	if (ISSET(pstatus, RSAQ_STATUS_DCD))
		sc->sc_msr |= UMSR_DCD;
	else
		sc->sc_msr &= ~UMSR_DCD;

	/* Deferred notifying to the ucom layer */
	taskqueue_enqueue(taskqueue_swi, &sc->sc_task);
}

static void
uplcom_notify(void *arg, int count)
{
	struct uplcom_softc *sc;

	sc = (struct uplcom_softc *)arg;
	if (sc->sc_ucom.sc_dying)
		return;
	ucom_status_change(&sc->sc_ucom);
}

static void
uplcom_get_status(void *addr, int portno, u_char *lsr, u_char *msr)
{
	struct uplcom_softc *sc = addr;

	DPRINTF(("uplcom_get_status:\n"));

	if (lsr != NULL)
		*lsr = sc->sc_lsr;
	if (msr != NULL)
		*msr = sc->sc_msr;
}

#if 0 /* TODO */
static int
uplcom_ioctl(void *addr, int portno, u_long cmd, caddr_t data, int flag,
	     struct thread *p)
{
	struct uplcom_softc *sc = addr;
	int error = 0;

	if (sc->sc_ucom.sc_dying)
		return (EIO);

	DPRINTF(("uplcom_ioctl: cmd = 0x%08lx\n", cmd));

	switch (cmd) {
	case TIOCNOTTY:
	case TIOCMGET:
	case TIOCMSET:
	case USB_GET_CM_OVER_DATA:
	case USB_SET_CM_OVER_DATA:
		break;

	default:
		DPRINTF(("uplcom_ioctl: unknown\n"));
		error = ENOTTY;
		break;
	}

	return (error);
}
#endif
