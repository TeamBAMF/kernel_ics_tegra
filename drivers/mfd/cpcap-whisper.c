/*
 * Copyright (C) 2010 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/switch.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>

#include <linux/regulator/consumer.h>
#include <linux/usb/otg.h>

#include <linux/spi/cpcap.h>
#include <linux/spi/cpcap-regbits.h>
#include <linux/spi/spi.h>

void tegra_cpcap_audio_dock_state(bool connected);

#define SENSE_USB_CLIENT    (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_USB_FLASH     (CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_USB_HOST      (CPCAP_BIT_ID_GROUND_S)
#define SENSE_USB_HOST_MASK (~CPCAP_BIT_SE1_S)

#define SENSE_FACTORY       (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_WHISPER_SPD   (CPCAP_BIT_SE1_S)

#define SENSE_WHISPER_PPD   (0)

#define SENSE_WHISPER_SMART (CPCAP_BIT_ID_GROUND_S | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S)

#define SENSE_CHARGER_FLOAT (CPCAP_BIT_ID_FLOAT_S  | \
			     CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
			     CPCAP_BIT_SE1_S)

#define SENSE_CHARGER       (CPCAP_BIT_VBUSVLD_S   | \
			     CPCAP_BIT_SESSVLD_S   | \
			     CPCAP_BIT_SE1_S)

/* TODO: Update with appropriate value. */
#define ADC_AUDIO_THRES     0x12C

enum cpcap_det_state {
	CONFIG,
	SAMPLE_1,
	SAMPLE_2,
	IDENTIFY,
	IDENTIFY_WHISPER,
	USB,
	USB_POWER,
	WHISPER,
	WHISPER_SMART,
};

enum cpcap_accy {
	CPCAP_ACCY_USB,
	CPCAP_ACCY_USB_HOST,
	CPCAP_ACCY_WHISPER,
	CPCAP_ACCY_WHISPER_SMART,
	CPCAP_ACCY_CHARGER,
	CPCAP_ACCY_NONE,

	/* Used while debouncing the accessory. */
	CPCAP_ACCY_UNKNOWN,
};

enum {
	NO_DOCK,
	DESK_DOCK,
	CAR_DOCK,
	LE_DOCK,
	HE_DOCK,
};

struct cpcap_whisper_data {
	struct cpcap_device *cpcap;
	struct cpcap_whisper_pdata *pdata;
	struct delayed_work work;
	struct workqueue_struct *wq;
	unsigned short sense;
	unsigned short prev_sense;
	enum cpcap_det_state state;
	struct regulator *regulator;
	struct wake_lock wake_lock;
	unsigned char is_vusb_enabled;
	struct switch_dev wsdev; /* Whisper switch */
	struct switch_dev dsdev; /* Dock switch */
	struct switch_dev asdev; /* Audio switch */
	struct switch_dev csdev; /* Invalid charger switch */
	char dock_id[CPCAP_WHISPER_ID_SIZE];
	char dock_prop[CPCAP_WHISPER_PROP_SIZE];
	struct otg_transceiver *otg;
};

static struct cpcap_whisper_data *whisper_di;

static int whisper_debug;
module_param(whisper_debug, int, S_IRUGO | S_IWUSR | S_IWGRP);

static ssize_t print_name(struct switch_dev *dsdev, char *buf)
{
	switch (switch_get_state(dsdev)) {
	case NO_DOCK:
		return sprintf(buf, "None\n");
	case DESK_DOCK:
		return sprintf(buf, "DESK\n");
	case CAR_DOCK:
		return sprintf(buf, "CAR\n");
	case LE_DOCK:
		return sprintf(buf, "LE\n");
	case HE_DOCK:
		return sprintf(buf, "HE\n");
	}

	return -EINVAL;
}

static ssize_t dock_id_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct switch_dev *dsdev = dev_get_drvdata(dev);
	struct cpcap_whisper_data *data =
		container_of(dsdev, struct cpcap_whisper_data, dsdev);

	return snprintf(buf, PAGE_SIZE, "%s\n", data->dock_id);
}
static DEVICE_ATTR(dock_addr, S_IRUGO | S_IWUSR, dock_id_show, NULL);

static ssize_t dock_prop_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct switch_dev *dsdev = dev_get_drvdata(dev);
	struct cpcap_whisper_data *data =
		container_of(dsdev, struct cpcap_whisper_data, dsdev);

	return snprintf(buf, PAGE_SIZE, "%s\n", data->dock_prop);
}
static DEVICE_ATTR(dock_prop, S_IRUGO | S_IWUSR, dock_prop_show, NULL);

static void vusb_enable(struct cpcap_whisper_data *data)
{
	if (!data->is_vusb_enabled) {
		regulator_enable(data->regulator);
		data->is_vusb_enabled = 1;
	}
}

static void vusb_disable(struct cpcap_whisper_data *data)
{
	if (data->is_vusb_enabled) {
		regulator_disable(data->regulator);
		data->is_vusb_enabled = 0;
	}
}

static int get_sense(struct cpcap_whisper_data *data)
{
	int retval = -EFAULT;
	unsigned short value;
	struct cpcap_device *cpcap;

	if (!data)
		return -EFAULT;
	cpcap = data->cpcap;

	retval = cpcap_regacc_read(cpcap, CPCAP_REG_INTS1, &value);
	if (retval)
		return retval;

	/* Clear ASAP after read. */
	retval = cpcap_regacc_write(cpcap, CPCAP_REG_INT1,
				     (CPCAP_BIT_CHRG_DET_I |
				      CPCAP_BIT_ID_FLOAT_I |
				      CPCAP_BIT_ID_GROUND_I),
				     (CPCAP_BIT_CHRG_DET_I |
				      CPCAP_BIT_ID_FLOAT_I |
				      CPCAP_BIT_ID_GROUND_I));
	if (retval)
		return retval;

	data->sense = value & (CPCAP_BIT_ID_FLOAT_S |
			       CPCAP_BIT_ID_GROUND_S);

	retval = cpcap_regacc_read(cpcap, CPCAP_REG_INTS2, &value);
	if (retval)
		return retval;

	/* Clear ASAP after read. */
	retval = cpcap_regacc_write(cpcap, CPCAP_REG_INT2,
				    (CPCAP_BIT_VBUSVLD_I |
				     CPCAP_BIT_SESSVLD_I |
				     CPCAP_BIT_SE1_I),
				    (CPCAP_BIT_VBUSVLD_I |
				     CPCAP_BIT_SESSVLD_I |
				     CPCAP_BIT_SE1_I));
	if (retval)
		return retval;

	data->sense |= value & (CPCAP_BIT_VBUSVLD_S |
				CPCAP_BIT_SESSVLD_S |
				CPCAP_BIT_SE1_S);
	return 0;
}

static int configure_hardware(struct cpcap_whisper_data *data,
			      enum cpcap_accy accy)
{
	int retval;

	retval = cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
				    CPCAP_BIT_DP150KPU,
				    (CPCAP_BIT_DP150KPU | CPCAP_BIT_DP1K5PU |
				     CPCAP_BIT_DM1K5PU | CPCAP_BIT_DPPD |
				     CPCAP_BIT_DMPD));

	switch (accy) {
	case CPCAP_ACCY_USB:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		gpio_set_value(data->pdata->data_gpio, 1);
		if (data->otg)
			raw_notifier_call_chain((void *)&data->otg->notifier.head,
						     USB_EVENT_VBUS, NULL);
		break;

	case CPCAP_ACCY_USB_HOST:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		gpio_set_value(data->pdata->data_gpio, 1);
		if (data->otg)
			raw_notifier_call_chain((void *)&data->otg->notifier.head,
						     USB_EVENT_ID, NULL);
		break;

	case CPCAP_ACCY_WHISPER:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		break;

	case CPCAP_ACCY_WHISPER_SMART:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     CPCAP_BIT_VBUSPD);
		gpio_set_value(data->pdata->data_gpio, 1);
		if (data->otg)
			raw_notifier_call_chain((void *)&data->otg->notifier.head,
						     USB_EVENT_ID, NULL);
		break;

	case CPCAP_ACCY_UNKNOWN:
		gpio_set_value(data->pdata->pwr_gpio, 0);
		gpio_set_value(data->pdata->data_gpio, 0);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1, 0,
					     (CPCAP_BIT_VBUSPD |
					      CPCAP_BIT_ID100KPU));
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC2, 0,
					     (CPCAP_BIT_EMUMODE2 |
					      CPCAP_BIT_EMUMODE1 |
					      CPCAP_BIT_EMUMODE0));
		break;

	case CPCAP_ACCY_CHARGER:
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
					     CPCAP_BIT_VBUSPD,
					     CPCAP_BIT_VBUSPD);
		break;

	case CPCAP_ACCY_NONE:
	default:
		gpio_set_value(data->pdata->pwr_gpio, 0);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC1,
					     CPCAP_BIT_VBUSPD,
					     CPCAP_BIT_VBUSPD);
		retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC2, 0,
					     CPCAP_BIT_USBXCVREN);
		vusb_disable(data);
		if (data->otg)
			raw_notifier_call_chain((void *)&data->otg->notifier.head,
						     USB_EVENT_NONE, NULL);
		break;
	}

	if (retval != 0)
		retval = -EFAULT;

	return retval;
}

static const char *accy_names[6] = {"USB", "USB host", "whisper",
				    "whisper_smart", "charger", "none"};

static void whisper_notify(struct cpcap_whisper_data *di, enum cpcap_accy accy)
{
	pr_info("%s: accy=%s\n", __func__, accy_names[accy]);

	configure_hardware(di, accy);

	if (accy == CPCAP_ACCY_WHISPER)
		switch_set_state(&di->wsdev, 1);
	else if (accy == CPCAP_ACCY_CHARGER)
		switch_set_state(&di->csdev, 1);
	else {
		switch_set_state(&di->wsdev, 0);
		switch_set_state(&di->dsdev, NO_DOCK);
		switch_set_state(&di->asdev, 0);
		switch_set_state(&di->csdev, 0);
		memset(di->dock_id, 0, CPCAP_WHISPER_ID_SIZE);
		memset(di->dock_prop, 0, CPCAP_WHISPER_PROP_SIZE);
		tegra_cpcap_audio_dock_state(false);
	}

	wake_lock_timeout(&di->wake_lock, HZ / 2);
}

static void whisper_audio_check(struct cpcap_whisper_data *di)
{
	struct cpcap_adc_request req;
	int ret;
	unsigned short value;
	int audio;

	if (!switch_get_state(&di->dsdev))
		return;

	cpcap_regacc_read(di->cpcap, CPCAP_REG_USBC1, &value);
	value &= CPCAP_BIT_ID100KPU;

	cpcap_regacc_write(di->cpcap, CPCAP_REG_USBC1, CPCAP_BIT_IDPUCNTRL,
			   (CPCAP_BIT_ID100KPU | CPCAP_BIT_IDPUCNTRL));

	msleep(200);

	req.format = CPCAP_ADC_FORMAT_RAW;
	req.timing = CPCAP_ADC_TIMING_IMM;
	req.type = CPCAP_ADC_TYPE_BANK_0;

	ret = cpcap_adc_sync_read(di->cpcap, &req);

	cpcap_regacc_write(di->cpcap, CPCAP_REG_USBC1, value,
			   (CPCAP_BIT_ID100KPU | CPCAP_BIT_IDPUCNTRL));

	if (whisper_debug)
		pr_info("%s: ADC result=0x%X (ret=%d, status=%d)\n", __func__,
			req.result[CPCAP_ADC_USB_ID], ret, req.status);

	audio = (req.result[CPCAP_ADC_USB_ID] > ADC_AUDIO_THRES) ? 1 : 0;
	switch_set_state(&di->asdev, audio);
	tegra_cpcap_audio_dock_state(!!audio);

	pr_info("%s: Audio cable %s present\n", __func__,
		(audio ? "is" : "not"));
}

static void whisper_det_work(struct work_struct *work)
{
	struct cpcap_whisper_data *data =
		container_of(work, struct cpcap_whisper_data, work.work);

	switch (data->state) {
	case CONFIG:
		wake_lock(&data->wake_lock);
		vusb_enable(data);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_CHRG_DET);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_IDFLOAT);
		cpcap_irq_mask(data->cpcap, CPCAP_IRQ_IDGND);

		configure_hardware(data, CPCAP_ACCY_UNKNOWN);

		data->state = SAMPLE_1;
		queue_delayed_work(data->wq, &data->work, msecs_to_jiffies(11));
		break;

	case SAMPLE_1:
		get_sense(data);
		data->state = SAMPLE_2;
		queue_delayed_work(data->wq, &data->work,
				   msecs_to_jiffies(100));
		break;

	case SAMPLE_2:
		data->prev_sense = data->sense;
		get_sense(data);

		if (data->prev_sense != data->sense) {
			/* Stay in this state */
			data->state = SAMPLE_2;
		} else
			data->state = IDENTIFY;

		queue_delayed_work(data->wq, &data->work,
				   msecs_to_jiffies(100));
		break;

	case IDENTIFY:
		get_sense(data);
		data->state = CONFIG;

		if (whisper_debug)
			pr_info("%s: sense=0x%04x\n", __func__, data->sense);

		if ((data->sense == SENSE_USB_CLIENT) ||
		    (data->sense == SENSE_USB_FLASH) ||
		    (data->sense == SENSE_FACTORY)) {
			whisper_notify(data, CPCAP_ACCY_USB);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);

			/* Special handling of USB undetect. */
			data->state = USB;
		} else if ((data->sense & SENSE_USB_HOST_MASK) ==
			   SENSE_USB_HOST) {
			whisper_notify(data, CPCAP_ACCY_USB_HOST);

			data->state = USB_POWER;
			queue_delayed_work(data->wq, &data->work,
					   msecs_to_jiffies(200));
		} else if ((data->sense == SENSE_WHISPER_SPD) ||
			   (data->sense == SENSE_WHISPER_PPD)) {
			gpio_set_value(data->pdata->pwr_gpio, 1);

			/* Extra identification step for Whisper. */
			data->state = IDENTIFY_WHISPER;
			queue_delayed_work(data->wq, &data->work,
					   msecs_to_jiffies(47));
		} else if (data->sense == SENSE_WHISPER_SMART) {
			whisper_notify(data, CPCAP_ACCY_WHISPER_SMART);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);

			/* Special handling of Whisper Smart accessories. */
			data->state = WHISPER_SMART;
		} else if ((data->sense == SENSE_CHARGER_FLOAT) ||
			   (data->sense == SENSE_CHARGER)) {
			whisper_notify(data, CPCAP_ACCY_CHARGER);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
		} else {
			whisper_notify(data, CPCAP_ACCY_NONE);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDFLOAT);
		}
		break;

	case IDENTIFY_WHISPER:
		get_sense(data);
		data->state = CONFIG;

		if (whisper_debug)
			pr_info("%s: sense=0x%04x\n", __func__, data->sense);

		if (data->sense & CPCAP_BIT_SE1_S) {
			whisper_notify(data, CPCAP_ACCY_WHISPER);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDFLOAT);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);

			/* Special handling of Whisper undetect. */
			data->state = WHISPER;
		} else {
			whisper_notify(data, CPCAP_ACCY_NONE);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDFLOAT);
		}
		break;

	case USB:
		get_sense(data);

		/* Check if Smart Whisper accessory fully inserted. */
		if (data->sense == SENSE_WHISPER_SMART) {
			data->state = WHISPER_SMART;

			whisper_notify(data, CPCAP_ACCY_NONE);
			whisper_notify(data, CPCAP_ACCY_WHISPER_SMART);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
		} else {
			data->state = CONFIG;
			queue_delayed_work(data->wq, &data->work, 0);
		}

		break;

	case USB_POWER:
		gpio_set_value(data->pdata->pwr_gpio, 1);
		data->state = CONFIG;
		cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDFLOAT);
		break;

	case WHISPER:
		get_sense(data);

		/* The removal of a Whisper accessory can only be detected
		 * if ID is floating.
		 */
		if (data->sense & CPCAP_BIT_ID_FLOAT_S) {
			data->state = CONFIG;
			queue_delayed_work(data->wq, &data->work, 0);
		} else {
			if (!(data->sense & CPCAP_BIT_ID_GROUND_S))
				whisper_audio_check(data);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDFLOAT);
			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_IDGND);
		}
		break;

	case WHISPER_SMART:
		get_sense(data);

		/* The removal of a Whisper Smart accessory can only be detected
		 * when VBUS disappears.
		 */
		if (!(data->sense & CPCAP_BIT_VBUSVLD_S)) {
			data->state = CONFIG;
			queue_delayed_work(data->wq, &data->work, 0);
		} else {
			if (!(data->sense & CPCAP_BIT_ID_GROUND_S))
				pr_info("%s: ID no longer ground\n", __func__);

			cpcap_irq_unmask(data->cpcap, CPCAP_IRQ_CHRG_DET);
		}
		break;

	default:
		/* This shouldn't happen.  Need to reset state machine. */
		vusb_disable(data);
		data->state = CONFIG;
		queue_delayed_work(data->wq, &data->work, 0);
		break;
	}
}

static void whisper_int_handler(enum cpcap_irqs int_event, void *data)
{
	struct cpcap_whisper_data *di = data;

	if (whisper_debug)
		pr_info("%s: irq=%d\n", __func__, int_event);

	queue_delayed_work(di->wq, &(di->work), 0);
}

int cpcap_accy_whisper(struct cpcap_device *cpcap,
		       struct cpcap_whisper_request *req)
{
	struct cpcap_whisper_data *di = cpcap->accydata;
	int retval = -EAGAIN;
	unsigned short value = 0;
	int dock;

	if (!di)
		return -ENODEV;

	/* Can only change settings if not debouncing and whisper device
	 * is present. */
	if (di->state == WHISPER) {
		if (req->cmd & CPCAP_WHISPER_ENABLE_UART)
			value = CPCAP_BIT_EMUMODE0;
		retval = cpcap_regacc_write(cpcap, CPCAP_REG_USBC2, value,
					    (CPCAP_BIT_EMUMODE2 |
					     CPCAP_BIT_EMUMODE1 |
					     CPCAP_BIT_EMUMODE0));

		value = (req->cmd & CPCAP_WHISPER_MODE_PU) ?
			CPCAP_BIT_ID100KPU : 0;
		retval |= cpcap_regacc_write(cpcap, CPCAP_REG_USBC1,
					     value, CPCAP_BIT_ID100KPU);
		if (value) {
			retval |= cpcap_regacc_write(cpcap, CPCAP_REG_USBC2,
						     (CPCAP_BIT_EMUMODE2 |
						      CPCAP_BIT_EMUMODE0),
						     (CPCAP_BIT_EMUMODE2 |
						      CPCAP_BIT_EMUMODE1 |
						      CPCAP_BIT_EMUMODE0));
		}

		/* Report dock type to system. */
		dock = (req->cmd & CPCAP_WHISPER_ACCY_MASK) >>
			CPCAP_WHISPER_ACCY_SHFT;
		if (dock && (strlen(req->dock_id) < CPCAP_WHISPER_ID_SIZE))
			strncpy(di->dock_id, req->dock_id,
				CPCAP_WHISPER_ID_SIZE);
		if (dock && (strlen(req->dock_prop) < CPCAP_WHISPER_PROP_SIZE))
			strncpy(di->dock_prop, req->dock_prop,
				CPCAP_WHISPER_PROP_SIZE);
		switch_set_state(&di->dsdev, dock);

		whisper_audio_check(di);
	} else if (di->state == WHISPER_SMART) {
		/* Report dock type to system. */
		dock = (req->cmd & CPCAP_WHISPER_ACCY_MASK) >>
			CPCAP_WHISPER_ACCY_SHFT;
		if (dock && (strlen(req->dock_id) < CPCAP_WHISPER_ID_SIZE))
			strncpy(di->dock_id, req->dock_id,
				CPCAP_WHISPER_ID_SIZE);
		if (dock && (strlen(req->dock_prop) < CPCAP_WHISPER_PROP_SIZE))
			strncpy(di->dock_prop, req->dock_prop,
				CPCAP_WHISPER_PROP_SIZE);
		switch_set_state(&di->dsdev, dock);
		retval = 0;
	}

	return retval;
}

void cpcap_accy_whisper_spdif_set_state(int state)
{
	if (!whisper_di)
		return;

	if (!switch_get_state(&whisper_di->dsdev))
		return;

	state = ((state > 0) ? 2 : 0);
	switch_set_state(&whisper_di->asdev, state);

	pr_info("%s: Audio cable %s present\n", __func__,
		(state ? "is" : "not"));
}

static int cpcap_whisper_probe(struct platform_device *pdev)
{
	int retval;
	struct cpcap_whisper_data *data;

	if (pdev->dev.platform_data == NULL) {
		dev_err(&pdev->dev, "no platform_data\n");
		return -EINVAL;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->pdata = pdev->dev.platform_data;
	data->cpcap = platform_get_drvdata(pdev);
	data->state = CONFIG;
	data->wq = create_singlethread_workqueue("cpcap_whisper");
	INIT_DELAYED_WORK(&data->work, whisper_det_work);
	wake_lock_init(&data->wake_lock, WAKE_LOCK_SUSPEND, "whisper");

	data->wsdev.name = "whisper";
	switch_dev_register(&data->wsdev);

	data->asdev.name = "usb_audio";
	switch_dev_register(&data->asdev);

	data->csdev.name = "invalid_charger";
	switch_dev_register(&data->csdev);

	data->dsdev.name = "dock";
	data->dsdev.print_name = print_name;
	switch_dev_register(&data->dsdev);
	retval = device_create_file(data->dsdev.dev, &dev_attr_dock_addr);
	if (retval < 0) {
		dev_err(&pdev->dev, "Failed to create dock_addr file\n");
		goto free_mem;
	}
	retval = device_create_file(data->dsdev.dev, &dev_attr_dock_prop);
	if (retval < 0) {
		dev_err(&pdev->dev, "Failed to create dock_prop file\n");
		goto free_dock_id;
	}

	platform_set_drvdata(pdev, data);

	data->regulator = regulator_get(&pdev->dev, "vusb");
	if (IS_ERR(data->regulator)) {
		dev_err(&pdev->dev,
			"Could not get regulator for cpcap_whisper\n");
		retval = PTR_ERR(data->regulator);
		goto free_dock_prop;
	}
	regulator_set_voltage(data->regulator, 3300000, 3300000);

	retval = cpcap_irq_clear(data->cpcap, CPCAP_IRQ_CHRG_DET);
	retval |= cpcap_irq_clear(data->cpcap, CPCAP_IRQ_IDFLOAT);
	retval |= cpcap_irq_clear(data->cpcap, CPCAP_IRQ_IDGND);

	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_CHRG_DET,
				     whisper_int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_IDFLOAT,
				     whisper_int_handler, data);
	retval |= cpcap_irq_register(data->cpcap, CPCAP_IRQ_IDGND,
				     whisper_int_handler, data);

	retval |= cpcap_regacc_write(data->cpcap, CPCAP_REG_USBC2,
				     (data->pdata->uartmux << 8),
				     (CPCAP_BIT_UARTMUX1 | CPCAP_BIT_UARTMUX0));

	if (retval != 0) {
		dev_err(&pdev->dev, "Initialization Error\n");
		retval = -ENODEV;
		goto free_irqs;
	}

#ifdef CONFIG_USB_CPCAP_OTG
	data->otg = otg_get_transceiver();
#endif

	data->cpcap->accydata = data;
	whisper_di = data;
	dev_info(&pdev->dev, "CPCAP Whisper detection probed\n");

	/* Perform initial detection */
	whisper_det_work(&(data->work.work));

	return 0;

free_irqs:
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDGND);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDFLOAT);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_DET);
	regulator_put(data->regulator);
free_dock_prop:
	device_remove_file(data->dsdev.dev, &dev_attr_dock_prop);
free_dock_id:
	device_remove_file(data->dsdev.dev, &dev_attr_dock_addr);
free_mem:
	switch_dev_unregister(&data->wsdev);
	switch_dev_unregister(&data->dsdev);
	switch_dev_unregister(&data->asdev);
	switch_dev_unregister(&data->csdev);
	wake_lock_destroy(&data->wake_lock);
	kfree(data);

	return retval;
}

static int __exit cpcap_whisper_remove(struct platform_device *pdev)
{
	struct cpcap_whisper_data *data = platform_get_drvdata(pdev);

	cpcap_irq_free(data->cpcap, CPCAP_IRQ_CHRG_DET);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDFLOAT);
	cpcap_irq_free(data->cpcap, CPCAP_IRQ_IDGND);

	configure_hardware(data, CPCAP_ACCY_NONE);
	cancel_delayed_work_sync(&data->work);
	destroy_workqueue(data->wq);

	device_remove_file(data->dsdev.dev, &dev_attr_dock_prop);
	device_remove_file(data->dsdev.dev, &dev_attr_dock_addr);
	switch_dev_unregister(&data->wsdev);
	switch_dev_unregister(&data->dsdev);
	switch_dev_unregister(&data->asdev);
	switch_dev_unregister(&data->csdev);

	gpio_set_value(data->pdata->data_gpio, 1);

	vusb_disable(data);
	regulator_put(data->regulator);

#ifdef CONFIG_USB_CPCAP_OTG
	if (data->otg)
		otg_put_transceiver(data->otg);
#endif

	wake_lock_destroy(&data->wake_lock);

	data->cpcap->accydata = NULL;
	kfree(data);

	return 0;
}

static struct platform_driver cpcap_whisper_driver = {
	.probe		= cpcap_whisper_probe,
	.remove		= __exit_p(cpcap_whisper_remove),
	.driver		= {
		.name	= "cpcap_whisper",
		.owner	= THIS_MODULE,
	},
};

static int __init cpcap_whisper_init(void)
{
	return cpcap_driver_register(&cpcap_whisper_driver);
}
late_initcall(cpcap_whisper_init);

static void __exit cpcap_whisper_exit(void)
{
	cpcap_driver_unregister(&cpcap_whisper_driver);
}
module_exit(cpcap_whisper_exit);

MODULE_ALIAS("platform:cpcap_whisper");
MODULE_DESCRIPTION("CPCAP Whisper detection driver");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
