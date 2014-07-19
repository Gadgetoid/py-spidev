/*
 * spidev_module.c - Python bindings for Linux SPI access through spidev
 * Copyright (C) 2009 Volker Thoms <unconnected@gmx.de>
 * Copyright (C) 2012 Stephen Caudle <scaudle@doceme.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <Python.h>
#include "structmember.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <sys/ioctl.h>

#define SPIDEV_MAXPATH 4096

PyDoc_STRVAR(SpiDev_module_doc,
	"This module defines an object type that allows SPI transactions\n"
	"on hosts running the Linux kernel. The host kernel must have SPI\n"
	"support and SPI device interface support.\n"
	"All of these can be either built-in to the kernel, or loaded from\n"
	"modules.\n"
	"\n"
	"Because the SPI device interface is opened R/W, users of this\n"
	"module usually must have root permissions.\n");

typedef struct {
	PyObject_HEAD

	int fd;	/* open file descriptor: /dev/spi-X.Y */
	uint8_t mode;	/* current SPI mode */
	uint8_t bits_per_word;	/* current SPI bits per word setting */
	uint32_t max_speed_hz;	/* current SPI max speed setting in Hz */
} SpiDevObject;

static char *wrmsg = "Argument must be a buffer or list of at least one, "
				"but not more than 4096 integers";

static void
get_tx_data(PyObject *arg, Py_buffer *pybuf)
{
	int status;
	pybuf->buf = NULL;
	if (PyObject_CheckBuffer(arg)) {
		status = PyObject_GetBuffer(arg, pybuf, PyBUF_SIMPLE);
		if (status < 0) {
		    PyErr_SetString(PyExc_TypeError, wrmsg);
		    return;
		}
	} else if (PyList_Check(arg)) {
		uint8_t *buf;
		int len, ii;

		len = PyList_GET_SIZE(arg);
		if (len > SPIDEV_MAXPATH) {
			PyErr_SetString(PyExc_OverflowError, wrmsg);
			return;
		}

		buf = malloc(len);
		for (ii = 0; ii < len; ii++) {
			PyObject *val = PyList_GET_ITEM(arg, ii);
			if (!PyInt_Check(val)) {
				PyErr_SetString(PyExc_TypeError, wrmsg);
				free(buf);
			}
			buf[ii] = (__u8)PyInt_AS_LONG(val);
		}
		pybuf->buf = &buf[0];
		pybuf->len = len;
		pybuf->itemsize = 0;
	} else {
		PyErr_SetString(PyExc_TypeError, wrmsg);
	}
}

static void
free_tx_data(Py_buffer *pybuf)
{
	if (!pybuf->buf)
		return;

	if (pybuf->itemsize != 0) {
		PyBuffer_Release(pybuf);
	} else {
		free(pybuf->buf);
	}
}

static PyObject *
SpiDev_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	SpiDevObject *self;
	if ((self = (SpiDevObject *)type->tp_alloc(type, 0)) == NULL)
		return NULL;

	self->fd = -1;
	self->mode = 0;
	self->bits_per_word = 0;
	self->max_speed_hz = 0;

	Py_INCREF(self);
	return (PyObject *)self;
}

PyDoc_STRVAR(SpiDev_close_doc,
	"close()\n\n"
	"Disconnects the object from the interface.\n");

static PyObject *
SpiDev_close(SpiDevObject *self)
{
	if ((self->fd != -1) && (close(self->fd) == -1)) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	self->fd = -1;
	self->mode = 0;
	self->bits_per_word = 0;
	self->max_speed_hz = 0;

	Py_INCREF(Py_None);
	return Py_None;
}

static void
SpiDev_dealloc(SpiDevObject *self)
{
	PyObject *ref = SpiDev_close(self);
	Py_XDECREF(ref);

	self->ob_type->tp_free((PyObject *)self);
}

PyDoc_STRVAR(SpiDev_write_doc,
	"write([values]) -> None\n\n"
	"Write bytes to SPI device.\n");

static PyObject *
SpiDev_writebytes(SpiDevObject *self, PyObject *args)
{
	int		status;
	int		len;
	PyObject	*list;
	Py_buffer  pybuf;

	if (!PyArg_ParseTuple(args, "O:write", &list))
		return NULL;

	get_tx_data(list, &pybuf);
	if (!pybuf.buf)
		return NULL;
	len = pybuf.len;

	Py_BEGIN_ALLOW_THREADS
	status = write(self->fd, pybuf.buf, len);
	Py_END_ALLOW_THREADS

	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free_tx_data(&pybuf);
		return NULL;
	}

	free_tx_data(&pybuf);

	if (status != len) {
		perror("short write");
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(SpiDev_read_doc,
	"read(len) -> [values]\n\n"
	"Read len bytes from SPI device.\n");

static PyObject *
SpiDev_readbytes(SpiDevObject *self, PyObject *args)
{
	uint8_t	rxbuf[SPIDEV_MAXPATH];
	int		status, len, ii;
	PyObject	*list;

	if (!PyArg_ParseTuple(args, "i:read", &len))
		return NULL;

	/* read at least 1 byte, no more than SPIDEV_MAXPATH */
	if (len < 1)
		len = 1;
	else if (len > sizeof(rxbuf))
		len = sizeof(rxbuf);

	Py_BEGIN_ALLOW_THREADS
	memset(rxbuf, 0, sizeof rxbuf);
	status = read(self->fd, &rxbuf[0], len);
	Py_END_ALLOW_THREADS

	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}

	if (status != len) {
		perror("short read");
		return NULL;
	}

	list = PyList_New(len);

	for (ii = 0; ii < len; ii++) {
		PyObject *val = Py_BuildValue("l", (long)rxbuf[ii]);
		PyList_SET_ITEM(list, ii, val);
	}

	Py_INCREF(list);
	return list;
}

PyDoc_STRVAR(SpiDev_xfer_doc,
	"xfer([values]) -> [values]\n\n"
	"Perform SPI transaction.\n"
	"CS will be released and reactivated between blocks.\n"
	"delay specifies delay in usec between blocks.\n");

static PyObject *
SpiDev_xfer(SpiDevObject *self, PyObject *args)
{
	uint16_t ii, len;
	int status;
	uint16_t delay_usecs = 0;
	uint32_t speed_hz = 0;
	uint8_t bits_per_word = 0;
	PyObject *list;
	Py_buffer pybuf;
#ifdef SPIDEV_SINGLE
	struct spi_ioc_transfer *xferptr;
#else
	struct spi_ioc_transfer xfer;
#endif
	uint8_t *txbuf, *rxbuf;

	if (!PyArg_ParseTuple(args, "O|IHB:xfer", &list, &speed_hz, &delay_usecs, &bits_per_word))
		return NULL;

	get_tx_data(list, &pybuf);
	if (!pybuf.buf)
		return NULL;

	txbuf = pybuf.buf;
	len = pybuf.len;
	rxbuf = malloc(sizeof(__u8) * len);

#ifdef SPIDEV_SINGLE
	xferptr = (struct spi_ioc_transfer*) malloc(sizeof(struct spi_ioc_transfer) * len);

	for (ii = 0; ii < len; ii++) {
		xferptr[ii].tx_buf = (unsigned long)&txbuf[ii];
		xferptr[ii].rx_buf = (unsigned long)&rxbuf[ii];
		xferptr[ii].len = 1;
		xferptr[ii].delay_usecs = delay;
		xferptr[ii].speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
		xferptr[ii].bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;
	}

	Py_BEGIN_ALLOW_THREADS
	status = ioctl(self->fd, SPI_IOC_MESSAGE(len), xferptr);
	Py_END_ALLOW_THREADS
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free(xferptr);
		free_tx_data(&pybuf);
		free(rxbuf);
		return NULL;
	}
#else
	xfer.tx_buf = (unsigned long)txbuf;
	xfer.rx_buf = (unsigned long)rxbuf;
	xfer.len = len;
	xfer.delay_usecs = delay_usecs;
	xfer.speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
	xfer.bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;

	Py_BEGIN_ALLOW_THREADS
	status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
	Py_END_ALLOW_THREADS
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free_tx_data(&pybuf);
		free(rxbuf);
		return NULL;
	}
#endif

	list = PyList_New(len);
	for (ii = 0; ii < len; ii++) {
		PyObject *val = Py_BuildValue("l", (long)rxbuf[ii]);
		PyList_SET_ITEM(list, ii, val);
	}

	// WA:
	// in CS_HIGH mode CS isn't pulled to low after transfer, but after read
	// reading 0 bytes doesnt matter but brings cs down
	// tomdean:
	// Stop generating an extra CS except in mode CS_HOGH
	if (self->mode & SPI_CS_HIGH) status = read(self->fd, &rxbuf[0], 0);

	free_tx_data(&pybuf);
	free(rxbuf);

	return list;
}


PyDoc_STRVAR(SpiDev_xfer2_doc,
	"xfer2([values]) -> [values]\n\n"
	"Perform SPI transaction.\n"
	"CS will be held active between blocks.\n");

static PyObject *
SpiDev_xfer2(SpiDevObject *self, PyObject *args)
{
	int status;
	uint16_t delay_usecs = 0;
	uint32_t speed_hz = 0;
	uint8_t bits_per_word = 0;
	uint16_t ii, len;
	PyObject *list;
	Py_buffer pybuf;
	struct spi_ioc_transfer xfer;
	uint8_t *txbuf, *rxbuf;

	if (!PyArg_ParseTuple(args, "O|IHB:xfer2", &list, &speed_hz, &delay_usecs, &bits_per_word))
		return NULL;

	get_tx_data(list, &pybuf);
	if (!pybuf.buf)
		return NULL;

	txbuf = pybuf.buf;
	len = pybuf.len;
	rxbuf = malloc(sizeof(__u8) * len);

	xfer.tx_buf = (unsigned long)txbuf;
	xfer.rx_buf = (unsigned long)rxbuf;
	xfer.len = len;
	xfer.delay_usecs = delay_usecs;
	xfer.speed_hz = speed_hz ? speed_hz : self->max_speed_hz;
	xfer.bits_per_word = bits_per_word ? bits_per_word : self->bits_per_word;

	Py_BEGIN_ALLOW_THREADS
	status = ioctl(self->fd, SPI_IOC_MESSAGE(1), &xfer);
	Py_END_ALLOW_THREADS
	if (status < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		free_tx_data(&pybuf);
		free(rxbuf);
		return NULL;
	}

	list = PyList_New(len);
	for (ii = 0; ii < len; ii++) {
		PyObject *val = Py_BuildValue("l", (long)rxbuf[ii]);
		PyList_SET_ITEM(list, ii, val);
	}
	// WA:
	// in CS_HIGH mode CS isnt pulled to low after transfer
	// reading 0 bytes doesn't really matter but brings CS down
	// tomdean:
	// Stop generating an extra CS except in mode CS_HOGH
	if (self->mode & SPI_CS_HIGH) status = read(self->fd, &rxbuf[0], 0);

	free_tx_data(&pybuf);
	free(rxbuf);

	return list;
}

static int __spidev_set_mode( int fd, __u8 mode) {
	__u8 test;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return -1;
	}
	if (ioctl(fd, SPI_IOC_RD_MODE, &test) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return -1;
	}
	if (test != mode) {
		return -1;
	}
	return 0;
}

static PyObject *
SpiDev_get_mode(SpiDevObject *self, void *closure)
{
	PyObject *result = Py_BuildValue("i", (self->mode & (SPI_CPHA | SPI_CPOL) ) );
	Py_INCREF(result);
	return result;
}

static PyObject *
SpiDev_get_cshigh(SpiDevObject *self, void *closure)
{
	PyObject *result;

	if (self->mode & SPI_CS_HIGH)
		result = Py_True;
	else
		result = Py_False;

	Py_INCREF(result);
	return result;
}

static PyObject *
SpiDev_get_lsbfirst(SpiDevObject *self, void *closure)
{
	PyObject *result;

	if (self->mode & SPI_LSB_FIRST)
		result = Py_True;
	else
		result = Py_False;

	Py_INCREF(result);
	return result;
}

static PyObject *
SpiDev_get_3wire(SpiDevObject *self, void *closure)
{
	PyObject *result;

	if (self->mode & SPI_3WIRE)
		result = Py_True;
	else
		result = Py_False;

	Py_INCREF(result);
	return result;
}

static PyObject *
SpiDev_get_loop(SpiDevObject *self, void *closure)
{
	PyObject *result;

	if (self->mode & SPI_LOOP)
		result = Py_True;
	else
		result = Py_False;

	Py_INCREF(result);
	return result;
}


static int
SpiDev_set_mode(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t mode, tmp;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyInt_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The mode attribute must be an integer");
		return -1;
	}

	mode = PyInt_AsLong(val);

	if ( mode > 3 ) {
		PyErr_SetString(PyExc_TypeError,
			"The mode attribute must be an integer"
				 "between 0 and 3.");
		return -1;
	}

	// clean and set CPHA and CPOL bits
	tmp = ( self->mode & ~(SPI_CPHA | SPI_CPOL) ) | mode ;

	__spidev_set_mode(self->fd, tmp);

	self->mode = tmp;
	return 0;
}

static int
SpiDev_set_cshigh(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t tmp;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyBool_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The cshigh attribute must be boolean");
		return -1;
	}

	if (val == Py_True)
		tmp = self->mode | SPI_CS_HIGH;
	else
		tmp = self->mode & ~SPI_CS_HIGH;

	__spidev_set_mode(self->fd, tmp);

	self->mode = tmp;
	return 0;
}

static int
SpiDev_set_lsbfirst(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t tmp;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyBool_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The lsbfirst attribute must be boolean");
		return -1;
	}

	if (val == Py_True)
		tmp = self->mode | SPI_LSB_FIRST;
	else
		tmp = self->mode & ~SPI_LSB_FIRST;

	__spidev_set_mode(self->fd, tmp);

	self->mode = tmp;
	return 0;
}

static int
SpiDev_set_3wire(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t tmp;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyBool_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The 3wire attribute must be boolean");
		return -1;
	}

	if (val == Py_True)
		tmp = self->mode | SPI_3WIRE;
	else
		tmp = self->mode & ~SPI_3WIRE;

	__spidev_set_mode(self->fd, tmp);

	self->mode = tmp;
	return 0;
}

static int
SpiDev_set_loop(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t tmp;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyBool_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The loop attribute must be boolean");
		return -1;
	}

	if (val == Py_True)
		tmp = self->mode | SPI_LOOP;
	else
		tmp = self->mode & ~SPI_LOOP;

	__spidev_set_mode(self->fd, tmp);

	self->mode = tmp;
	return 0;
}

static PyObject *
SpiDev_get_bits_per_word(SpiDevObject *self, void *closure)
{
	PyObject *result = Py_BuildValue("i", self->bits_per_word);
	Py_INCREF(result);
	return result;
}

static int
SpiDev_set_bits_per_word(SpiDevObject *self, PyObject *val, void *closure)
{
	uint8_t bits;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyInt_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The bits_per_word attribute must be an integer");
		return -1;
	}

	bits = PyInt_AsLong(val);

        if (bits < 8 || bits > 16) {
		PyErr_SetString(PyExc_TypeError,
                                "invalid bits_per_word (8 to 16)");
		return -1;
	}

	if (self->bits_per_word != bits) {
		if (ioctl(self->fd, SPI_IOC_WR_BITS_PER_WORD, &bits) == -1) {
			PyErr_SetFromErrno(PyExc_IOError);
			return -1;
		}
		self->bits_per_word = bits;
	}
	return 0;
}

static PyObject *
SpiDev_get_max_speed_hz(SpiDevObject *self, void *closure)
{
	PyObject *result = Py_BuildValue("i", self->max_speed_hz);
	Py_INCREF(result);
	return result;
}

static int
SpiDev_set_max_speed_hz(SpiDevObject *self, PyObject *val, void *closure)
{
	uint32_t max_speed_hz;

	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}
	else if (!PyInt_Check(val)) {
		PyErr_SetString(PyExc_TypeError,
			"The max_speed_hz attribute must be an integer");
		return -1;
	}

	max_speed_hz = PyInt_AsLong(val);

	if (self->max_speed_hz != max_speed_hz) {
		if (ioctl(self->fd, SPI_IOC_WR_MAX_SPEED_HZ, &max_speed_hz) == -1) {
			PyErr_SetFromErrno(PyExc_IOError);
			return -1;
		}
		self->max_speed_hz = max_speed_hz;
	}
	return 0;
}

static PyGetSetDef SpiDev_getset[] = {
	{"mode", (getter)SpiDev_get_mode, (setter)SpiDev_set_mode,
			"SPI mode as two bit pattern of \n"
			"Clock Polarity  and Phase [CPOL|CPHA]\n"
			"min: 0b00 = 0 max: 0b11 = 3\n"},
	{"cshigh", (getter)SpiDev_get_cshigh, (setter)SpiDev_set_cshigh,
			"CS active high\n"},
	{"threewire", (getter)SpiDev_get_3wire, (setter)SpiDev_set_3wire,
			"SI/SO signals shared\n"},
	{"lsbfirst", (getter)SpiDev_get_lsbfirst, (setter)SpiDev_set_lsbfirst,
			"LSB first\n"},
	{"loop", (getter)SpiDev_get_loop, (setter)SpiDev_set_loop,
			"loopback configuration\n"},
	{"bits_per_word", (getter)SpiDev_get_bits_per_word, (setter)SpiDev_set_bits_per_word,
			"bits per word\n"},
	{"max_speed_hz", (getter)SpiDev_get_max_speed_hz, (setter)SpiDev_set_max_speed_hz,
			"maximum speed in Hz\n"},
	{NULL},
};

PyDoc_STRVAR(SpiDev_open_doc,
	"open(bus, device)\n\n"
	"Connects the object to the specified SPI device.\n"
	"open(X,Y) will open /dev/spidev-X.Y\n");

static PyObject *
SpiDev_open(SpiDevObject *self, PyObject *args, PyObject *kwds)
{
	int bus, device;
	char path[SPIDEV_MAXPATH];
	uint8_t tmp8;
	uint32_t tmp32;
	static char *kwlist[] = {"bus", "device", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii:open", kwlist, &bus, &device))
		return NULL;
	if (snprintf(path, SPIDEV_MAXPATH, "/dev/spidev%d.%d", bus, device) >= SPIDEV_MAXPATH) {
		PyErr_SetString(PyExc_OverflowError,
			"Bus and/or device number is invalid.");
		return NULL;
	}
	if ((self->fd = open(path, O_RDWR, 0)) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	if (ioctl(self->fd, SPI_IOC_RD_MODE, &tmp8) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->mode = tmp8;
	if (ioctl(self->fd, SPI_IOC_RD_BITS_PER_WORD, &tmp8) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->bits_per_word = tmp8;
	if (ioctl(self->fd, SPI_IOC_RD_MAX_SPEED_HZ, &tmp32) == -1) {
		PyErr_SetFromErrno(PyExc_IOError);
		return NULL;
	}
	self->max_speed_hz = tmp32;

	Py_INCREF(Py_None);
	return Py_None;
}

static int
SpiDev_init(SpiDevObject *self, PyObject *args, PyObject *kwds)
{
	int bus = -1;
	int client = -1;
	static char *kwlist[] = {"bus", "client", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ii:__init__",
			kwlist, &bus, &client))
		return -1;

	if (bus >= 0) {
		SpiDev_open(self, args, kwds);
		if (PyErr_Occurred())
			return -1;
	}

	return 0;
}


PyDoc_STRVAR(SpiDevObjectType_doc,
	"SpiDev([bus],[client]) -> SPI\n\n"
	"Return a new SPI object that is (optionally) connected to the\n"
	"specified SPI device interface.\n");

static PyMethodDef SpiDev_methods[] = {
	{"open", (PyCFunction)SpiDev_open, METH_VARARGS | METH_KEYWORDS,
		SpiDev_open_doc},
	{"close", (PyCFunction)SpiDev_close, METH_NOARGS,
		SpiDev_close_doc},
	{"readbytes", (PyCFunction)SpiDev_readbytes, METH_VARARGS,
		SpiDev_read_doc},
	{"writebytes", (PyCFunction)SpiDev_writebytes, METH_VARARGS,
		SpiDev_write_doc},
	{"xfer", (PyCFunction)SpiDev_xfer, METH_VARARGS,
		SpiDev_xfer_doc},
	{"xfer2", (PyCFunction)SpiDev_xfer2, METH_VARARGS,
		SpiDev_xfer2_doc},
	{NULL},
};

static PyTypeObject SpiDevObjectType = {
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size */
	"SpiDev",			/* tp_name */
	sizeof(SpiDevObject),		/* tp_basicsize */
	0,				/* tp_itemsize */
	(destructor)SpiDev_dealloc,	/* tp_dealloc */
	0,				/* tp_print */
	0,				/* tp_getattr */
	0,				/* tp_setattr */
	0,				/* tp_compare */
	0,				/* tp_repr */
	0,				/* tp_as_number */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping */
	0,				/* tp_hash */
	0,				/* tp_call */
	0,				/* tp_str */
	0,				/* tp_getattro */
	0,				/* tp_setattro */
	0,				/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,		/* tp_flags */
	SpiDevObjectType_doc,		/* tp_doc */
	0,				/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	0,				/* tp_iter */
	0,				/* tp_iternext */
	SpiDev_methods,			/* tp_methods */
	0,				/* tp_members */
	SpiDev_getset,			/* tp_getset */
	0,				/* tp_base */
	0,				/* tp_dict */
	0,				/* tp_descr_get */
	0,				/* tp_descr_set */
	0,				/* tp_dictoffset */
	(initproc)SpiDev_init,		/* tp_init */
	0,				/* tp_alloc */
	SpiDev_new,			/* tp_new */
};

static PyMethodDef SpiDev_module_methods[] = {
	{NULL}
};

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initspidev(void)
{
	PyObject* m;

	if (PyType_Ready(&SpiDevObjectType) < 0)
		return;

	m = Py_InitModule3("spidev", SpiDev_module_methods, SpiDev_module_doc);
	Py_INCREF(&SpiDevObjectType);
	PyModule_AddObject(m, "SpiDev", (PyObject *)&SpiDevObjectType);
}

