/*
    tuner-i2c.h - i2c interface for different tuners

    Copyright (C) 2007 Michael Krufky (mkrufky@linuxtv.org)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef __TUNER_I2C_H__
#define __TUNER_I2C_H__

#include <linux/i2c.h>

struct tuner_i2c_props {
	u8 addr;
	struct i2c_adapter *adap;
};

static inline int tuner_i2c_xfer_send(struct tuner_i2c_props *props, char *buf, int len)
{
	struct i2c_msg msg = { .addr = props->addr, .flags = 0,
			       .buf = buf, .len = len };
	int ret = i2c_transfer(props->adap, &msg, 1);

	return (ret == 1) ? len : ret;
}

static inline int tuner_i2c_xfer_recv(struct tuner_i2c_props *props, char *buf, int len)
{
	struct i2c_msg msg = { .addr = props->addr, .flags = I2C_M_RD,
			       .buf = buf, .len = len };
	int ret = i2c_transfer(props->adap, &msg, 1);

	return (ret == 1) ? len : ret;
}

#ifndef __TUNER_DRIVER_H__
#define tuner_warn(fmt, arg...) do {\
	printk(KERN_WARNING PREFIX "%d-%04x: " fmt, \
			i2c_adapter_id(priv->i2c_props.adap), priv->i2c_props.addr , ##arg); } while (0)
#define tuner_info(fmt, arg...) do {\
	printk(KERN_INFO PREFIX "%d-%04x: " fmt, \
			i2c_adapter_id(priv->i2c_props.adap), priv->i2c_props.addr , ##arg); } while (0)
#define tuner_dbg(fmt, arg...) do {\
	if ((debug)) \
		printk(KERN_DEBUG PREFIX "%d-%04x: " fmt, \
			i2c_adapter_id(priv->i2c_props.adap), priv->i2c_props.addr , ##arg); } while (0)
#endif /* __TUNER_DRIVER_H__ */

#endif /* __TUNER_I2C_H__ */

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
