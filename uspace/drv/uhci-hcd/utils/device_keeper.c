/*
 * Copyright (c) 2011 Jan Vesely
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup drvusbuhcihc
 * @{
 */
/** @file
 * @brief UHCI driver
 */
#include <assert.h>
#include <errno.h>
#include <usb/debug.h>

#include "device_keeper.h"

/*----------------------------------------------------------------------------*/
/** Initialize device keeper structure.
 *
 * @param[in] instance Memory place to initialize.
 *
 * Set all values to false/0.
 */
void device_keeper_init(device_keeper_t *instance)
{
	assert(instance);
	fibril_mutex_initialize(&instance->guard);
	fibril_condvar_initialize(&instance->default_address_occupied);
	instance->last_address = 0;
	unsigned i = 0;
	for (; i < USB_ADDRESS_COUNT; ++i) {
		instance->devices[i].occupied = false;
		instance->devices[i].handle = 0;
		instance->devices[i].toggle_status = 0;
	}
}
/*----------------------------------------------------------------------------*/
/** Attempt to obtain address 0, blocks.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] speed Speed of the device requesting default address.
 */
void device_keeper_reserve_default(device_keeper_t *instance, usb_speed_t speed)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	while (instance->devices[USB_ADDRESS_DEFAULT].occupied) {
		fibril_condvar_wait(&instance->default_address_occupied,
		    &instance->guard);
	}
	instance->devices[USB_ADDRESS_DEFAULT].occupied = true;
	instance->devices[USB_ADDRESS_DEFAULT].speed = speed;
	fibril_mutex_unlock(&instance->guard);
}
/*----------------------------------------------------------------------------*/
/** Attempt to obtain address 0, blocks.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] speed Speed of the device requesting default address.
 */
void device_keeper_release_default(device_keeper_t *instance)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	instance->devices[USB_ADDRESS_DEFAULT].occupied = false;
	fibril_mutex_unlock(&instance->guard);
	fibril_condvar_signal(&instance->default_address_occupied);
}
/*----------------------------------------------------------------------------*/
/** Check setup packet data for signs of toggle reset.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] target Device to receive setup packet.
 * @param[in] data Setup packet data.
 *
 * Really ugly one.
 */
void device_keeper_reset_if_need(
    device_keeper_t *instance, usb_target_t target, const unsigned char *data)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	if (target.endpoint > 15 || target.endpoint < 0
	    || target.address >= USB_ADDRESS_COUNT || target.address < 0
	    || !instance->devices[target.address].occupied) {
		fibril_mutex_unlock(&instance->guard);
		usb_log_error("Invalid data when checking for toggle reset.\n");
		return;
	}

	switch (data[1])
	{
	case 0x01: /*clear feature*/
		/* recipient is endpoint, value is zero (ENDPOINT_STALL) */
		if (((data[0] & 0xf) == 1) && ((data[2] | data[3]) == 0)) {
			/* endpoint number is < 16, thus first byte is enough */
			instance->devices[target.address].toggle_status &=
			    ~(1 << data[4]);
		}
	break;

	case 0x9: /* set configuration */
	case 0x11: /* set interface */
		/* target must be device */
		if ((data[0] & 0xf) == 0) {
			instance->devices[target.address].toggle_status = 0;
		}
	break;
	}
	fibril_mutex_unlock(&instance->guard);
}
/*----------------------------------------------------------------------------*/
/** Get current value of endpoint toggle.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] target Device and endpoint used.
 * @return Error code
 */
int device_keeper_get_toggle(device_keeper_t *instance, usb_target_t target)
{
	assert(instance);
	int ret;
	fibril_mutex_lock(&instance->guard);
	if (target.endpoint > 15 || target.endpoint < 0
	    || target.address >= USB_ADDRESS_COUNT || target.address < 0
	    || !instance->devices[target.address].occupied) {
		usb_log_error("Invalid data when asking for toggle value.\n");
		ret = EINVAL;
	} else {
		ret = (instance->devices[target.address].toggle_status
		        >> target.endpoint) & 1;
	}
	fibril_mutex_unlock(&instance->guard);
	return ret;
}
/*----------------------------------------------------------------------------*/
/** Set current value of endpoint toggle.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] target Device and endpoint used.
 * @param[in] toggle Toggle value.
 * @return Error code.
 */
int device_keeper_set_toggle(
    device_keeper_t *instance, usb_target_t target, bool toggle)
{
	assert(instance);
	int ret;
	fibril_mutex_lock(&instance->guard);
	if (target.endpoint > 15 || target.endpoint < 0
	    || target.address >= USB_ADDRESS_COUNT || target.address < 0
	    || !instance->devices[target.address].occupied) {
		usb_log_error("Invalid data when setting toggle value.\n");
		ret = EINVAL;
	} else {
		if (toggle) {
			instance->devices[target.address].toggle_status |=
			    (1 << target.endpoint);
		} else {
			instance->devices[target.address].toggle_status &=
			    ~(1 << target.endpoint);
		}
		ret = EOK;
	}
	fibril_mutex_unlock(&instance->guard);
	return ret;
}
/*----------------------------------------------------------------------------*/
/** Get a free USB address
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] speed Speed of the device requiring address.
 * @return Free address, or error code.
 */
usb_address_t device_keeper_request(
    device_keeper_t *instance, usb_speed_t speed)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);

	usb_address_t new_address = instance->last_address;
	do {
		++new_address;
		if (new_address > USB11_ADDRESS_MAX)
			new_address = 1;
		if (new_address == instance->last_address) {
			fibril_mutex_unlock(&instance->guard);
			return ENOSPC;
		}
	} while (instance->devices[new_address].occupied);

	assert(new_address != USB_ADDRESS_DEFAULT);
	assert(instance->devices[new_address].occupied == false);
	instance->devices[new_address].occupied = true;
	instance->devices[new_address].speed = speed;
	instance->devices[new_address].toggle_status = 0;
	instance->last_address = new_address;
	fibril_mutex_unlock(&instance->guard);
	return new_address;
}
/*----------------------------------------------------------------------------*/
/** Bind USB address to devman handle.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] address Device address
 * @param[in] handle Devman handle of the device.
 */
void device_keeper_bind(
    device_keeper_t *instance, usb_address_t address, devman_handle_t handle)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	assert(address > 0);
	assert(address <= USB11_ADDRESS_MAX);
	assert(instance->devices[address].occupied);
	instance->devices[address].handle = handle;
	fibril_mutex_unlock(&instance->guard);
}
/*----------------------------------------------------------------------------*/
/** Release used USB address.
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] address Device address
 */
void device_keeper_release(device_keeper_t *instance, usb_address_t address)
{
	assert(instance);
	assert(address > 0);
	assert(address <= USB11_ADDRESS_MAX);

	fibril_mutex_lock(&instance->guard);
	assert(instance->devices[address].occupied);
	instance->devices[address].occupied = false;
	fibril_mutex_unlock(&instance->guard);
}
/*----------------------------------------------------------------------------*/
/** Find USB address associated with the device
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] handle Devman handle of the device seeking its address.
 * @return USB Address, or error code.
 */
usb_address_t device_keeper_find(
    device_keeper_t *instance, devman_handle_t handle)
{
	assert(instance);
	fibril_mutex_lock(&instance->guard);
	usb_address_t address = 1;
	while (address <= USB11_ADDRESS_MAX) {
		if (instance->devices[address].handle == handle) {
			fibril_mutex_unlock(&instance->guard);
			return address;
		}
		++address;
	}
	fibril_mutex_unlock(&instance->guard);
	return ENOENT;
}
/*----------------------------------------------------------------------------*/
/** Get speed associated with the address
 *
 * @param[in] instance Device keeper structure to use.
 * @param[in] address Address of the device.
 * @return USB speed.
 */
usb_speed_t device_keeper_speed(
    device_keeper_t *instance, usb_address_t address)
{
	assert(instance);
	assert(address >= 0);
	assert(address <= USB11_ADDRESS_MAX);
	return instance->devices[address].speed;
}
/**
 * @}
 */
