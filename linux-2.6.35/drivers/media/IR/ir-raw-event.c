/* ir-raw-event.c - handle IR Pulse/Space event
 *
 * Copyright (C) 2010 by Mauro Carvalho Chehab <mchehab@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include "ir-core-priv.h"

/* Define the max number of pulse/space transitions to buffer */
#define MAX_IR_EVENT_SIZE      512

/* Used to keep track of IR raw clients, protected by ir_raw_handler_lock */
static LIST_HEAD(ir_raw_client_list);

/* Used to handle IR raw handler extensions */
static DEFINE_SPINLOCK(ir_raw_handler_lock);
static LIST_HEAD(ir_raw_handler_list);
static u64 available_protocols;

#ifdef MODULE
/* Used to load the decoders */
static struct work_struct wq_load;
#endif

static void ir_raw_event_work(struct work_struct *work)
{
	struct ir_raw_event ev;
	struct ir_raw_handler *handler;
	struct ir_raw_event_ctrl *raw =
		container_of(work, struct ir_raw_event_ctrl, rx_work);

	while (kfifo_out(&raw->kfifo, &ev, sizeof(ev)) == sizeof(ev)) {
		spin_lock(&ir_raw_handler_lock);
		list_for_each_entry(handler, &ir_raw_handler_list, list)
			handler->decode(raw->input_dev, ev);
		spin_unlock(&ir_raw_handler_lock);
		raw->prev_ev = ev;
	}
}

/**
 * ir_raw_event_store() - pass a pulse/space duration to the raw ir decoders
 * @input_dev:	the struct input_dev device descriptor
 * @ev:		the struct ir_raw_event descriptor of the pulse/space
 *
 * This routine (which may be called from an interrupt context) stores a
 * pulse/space duration for the raw ir decoding state machines. Pulses are
 * signalled as positive values and spaces as negative values. A zero value
 * will reset the decoding state machines.
 */
int ir_raw_event_store(struct input_dev *input_dev, struct ir_raw_event *ev)
{
	struct ir_input_dev *ir = input_get_drvdata(input_dev);

	if (!ir->raw)
		return -EINVAL;

	if (kfifo_in(&ir->raw->kfifo, ev, sizeof(*ev)) != sizeof(*ev))
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(ir_raw_event_store);

/**
 * ir_raw_event_store_edge() - notify raw ir decoders of the start of a pulse/space
 * @input_dev:	the struct input_dev device descriptor
 * @type:	the type of the event that has occurred
 *
 * This routine (which may be called from an interrupt context) is used to
 * store the beginning of an ir pulse or space (or the start/end of ir
 * reception) for the raw ir decoding state machines. This is used by
 * hardware which does not provide durations directly but only interrupts
 * (or similar events) on state change.
 */
int ir_raw_event_store_edge(struct input_dev *input_dev, enum raw_event_type type)
{
	struct ir_input_dev	*ir = input_get_drvdata(input_dev);
	ktime_t			now;
	s64			delta; /* ns */
	struct ir_raw_event	ev;
	int			rc = 0;

	if (!ir->raw)
		return -EINVAL;

	now = ktime_get();
	delta = ktime_to_ns(ktime_sub(now, ir->raw->last_event));

	/* Check for a long duration since last event or if we're
	 * being called for the first time, note that delta can't
	 * possibly be negative.
	 */
	ev.duration = 0;
	if (delta > IR_MAX_DURATION || !ir->raw->last_type)
		type |= IR_START_EVENT;
	else
		ev.duration = delta;

	if (type & IR_START_EVENT)
		ir_raw_event_reset(input_dev);
	else if (ir->raw->last_type & IR_SPACE) {
		ev.pulse = false;
		rc = ir_raw_event_store(input_dev, &ev);
	} else if (ir->raw->last_type & IR_PULSE) {
		ev.pulse = true;
		rc = ir_raw_event_store(input_dev, &ev);
	} else
		return 0;

	ir->raw->last_event = now;
	ir->raw->last_type = type;
	return rc;
}
EXPORT_SYMBOL_GPL(ir_raw_event_store_edge);

/**
 * ir_raw_event_handle() - schedules the decoding of stored ir data
 * @input_dev:	the struct input_dev device descriptor
 *
 * This routine will signal the workqueue to start decoding stored ir data.
 */
void ir_raw_event_handle(struct input_dev *input_dev)
{
	struct ir_input_dev *ir = input_get_drvdata(input_dev);

	if (!ir->raw)
		return;

	schedule_work(&ir->raw->rx_work);
}
EXPORT_SYMBOL_GPL(ir_raw_event_handle);

/* used internally by the sysfs interface */
u64
ir_raw_get_allowed_protocols()
{
	u64 protocols;
	spin_lock(&ir_raw_handler_lock);
	protocols = available_protocols;
	spin_unlock(&ir_raw_handler_lock);
	return protocols;
}

/*
 * Used to (un)register raw event clients
 */
int ir_raw_event_register(struct input_dev *input_dev)
{
	struct ir_input_dev *ir = input_get_drvdata(input_dev);
	int rc;
	struct ir_raw_handler *handler;

	ir->raw = kzalloc(sizeof(*ir->raw), GFP_KERNEL);
	if (!ir->raw)
		return -ENOMEM;

	ir->raw->input_dev = input_dev;
	INIT_WORK(&ir->raw->rx_work, ir_raw_event_work);
	ir->raw->enabled_protocols = ~0;
	rc = kfifo_alloc(&ir->raw->kfifo, sizeof(s64) * MAX_IR_EVENT_SIZE,
			 GFP_KERNEL);
	if (rc < 0) {
		kfree(ir->raw);
		ir->raw = NULL;
		return rc;
	}

	spin_lock(&ir_raw_handler_lock);
	list_add_tail(&ir->raw->list, &ir_raw_client_list);
	list_for_each_entry(handler, &ir_raw_handler_list, list)
		if (handler->raw_register)
			handler->raw_register(ir->raw->input_dev);
	spin_unlock(&ir_raw_handler_lock);

	return 0;
}

void ir_raw_event_unregister(struct input_dev *input_dev)
{
	struct ir_input_dev *ir = input_get_drvdata(input_dev);
	struct ir_raw_handler *handler;

	if (!ir->raw)
		return;

	cancel_work_sync(&ir->raw->rx_work);

	spin_lock(&ir_raw_handler_lock);
	list_del(&ir->raw->list);
	list_for_each_entry(handler, &ir_raw_handler_list, list)
		if (handler->raw_unregister)
			handler->raw_unregister(ir->raw->input_dev);
	spin_unlock(&ir_raw_handler_lock);

	kfifo_free(&ir->raw->kfifo);
	kfree(ir->raw);
	ir->raw = NULL;
}

/*
 * Extension interface - used to register the IR decoders
 */

int ir_raw_handler_register(struct ir_raw_handler *ir_raw_handler)
{
	struct ir_raw_event_ctrl *raw;

	spin_lock(&ir_raw_handler_lock);
	list_add_tail(&ir_raw_handler->list, &ir_raw_handler_list);
	if (ir_raw_handler->raw_register)
		list_for_each_entry(raw, &ir_raw_client_list, list)
			ir_raw_handler->raw_register(raw->input_dev);
	available_protocols |= ir_raw_handler->protocols;
	spin_unlock(&ir_raw_handler_lock);

	return 0;
}
EXPORT_SYMBOL(ir_raw_handler_register);

void ir_raw_handler_unregister(struct ir_raw_handler *ir_raw_handler)
{
	struct ir_raw_event_ctrl *raw;

	spin_lock(&ir_raw_handler_lock);
	list_del(&ir_raw_handler->list);
	if (ir_raw_handler->raw_unregister)
		list_for_each_entry(raw, &ir_raw_client_list, list)
			ir_raw_handler->raw_unregister(raw->input_dev);
	available_protocols &= ~ir_raw_handler->protocols;
	spin_unlock(&ir_raw_handler_lock);
}
EXPORT_SYMBOL(ir_raw_handler_unregister);

#ifdef MODULE
static void init_decoders(struct work_struct *work)
{
	/* Load the decoder modules */

	load_nec_decode();
	load_rc5_decode();
	load_rc6_decode();
	load_jvc_decode();
	load_sony_decode();
	load_lirc_codec();

	/* If needed, we may later add some init code. In this case,
	   it is needed to change the CONFIG_MODULE test at ir-core.h
	 */
}
#endif

void ir_raw_init(void)
{
#ifdef MODULE
	INIT_WORK(&wq_load, init_decoders);
	schedule_work(&wq_load);
#endif
}
