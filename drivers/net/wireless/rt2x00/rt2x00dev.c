/*
	Copyright (C) 2004 - 2007 rt2x00 SourceForge Project
	<http://rt2x00.serialmonkey.com>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the
	Free Software Foundation, Inc.,
	59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
	Module: rt2x00lib
	Abstract: rt2x00 generic device routines.
 */

/*
 * Set enviroment defines for rt2x00.h
 */
#define DRV_NAME "rt2x00lib"

#include <linux/kernel.h>
#include <linux/module.h>

#include "rt2x00.h"
#include "rt2x00lib.h"

/*
 * Ring handler.
 */
struct data_ring *rt2x00lib_get_ring(struct rt2x00_dev *rt2x00dev,
				     const unsigned int queue)
{
	int beacon = test_bit(DRIVER_REQUIRE_BEACON_RING, &rt2x00dev->flags);

	/*
	 * Check if we are requesting a reqular TX ring,
	 * or if we are requesting a Beacon or Atim ring.
	 * For Atim rings, we should check if it is supported.
	 */
	if (queue < rt2x00dev->hw->queues && rt2x00dev->tx)
		return &rt2x00dev->tx[queue];

	if (!rt2x00dev->bcn || !beacon)
		return NULL;

	if (queue == IEEE80211_TX_QUEUE_BEACON)
		return &rt2x00dev->bcn[0];
	else if (queue == IEEE80211_TX_QUEUE_AFTER_BEACON)
		return &rt2x00dev->bcn[1];

	return NULL;
}
EXPORT_SYMBOL_GPL(rt2x00lib_get_ring);

/*
 * Link tuning handlers
 */
static void rt2x00lib_start_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	rt2x00_clear_link(&rt2x00dev->link);

	/*
	 * Reset the link tuner.
	 */
	rt2x00dev->ops->lib->reset_tuner(rt2x00dev);

	queue_delayed_work(rt2x00dev->hw->workqueue,
			   &rt2x00dev->link.work, LINK_TUNE_INTERVAL);
}

static void rt2x00lib_stop_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	cancel_delayed_work_sync(&rt2x00dev->link.work);
}

void rt2x00lib_reset_link_tuner(struct rt2x00_dev *rt2x00dev)
{
	if (!test_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	rt2x00lib_stop_link_tuner(rt2x00dev);
	rt2x00lib_start_link_tuner(rt2x00dev);
}

/*
 * Radio control handlers.
 */
int rt2x00lib_enable_radio(struct rt2x00_dev *rt2x00dev)
{
	int status;

	/*
	 * Don't enable the radio twice.
	 * And check if the hardware button has been disabled.
	 */
	if (test_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags) ||
	    test_bit(DEVICE_DISABLED_RADIO_HW, &rt2x00dev->flags))
		return 0;

	/*
	 * Enable radio.
	 */
	status = rt2x00dev->ops->lib->set_device_state(rt2x00dev,
						       STATE_RADIO_ON);
	if (status)
		return status;

	__set_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags);

	/*
	 * Enable RX.
	 */
	rt2x00lib_toggle_rx(rt2x00dev, STATE_RADIO_RX_ON);

	/*
	 * Start the TX queues.
	 */
	ieee80211_start_queues(rt2x00dev->hw);

	return 0;
}

void rt2x00lib_disable_radio(struct rt2x00_dev *rt2x00dev)
{
	if (!__test_and_clear_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Stop all scheduled work.
	 */
	if (work_pending(&rt2x00dev->beacon_work))
		cancel_work_sync(&rt2x00dev->beacon_work);
	if (work_pending(&rt2x00dev->filter_work))
		cancel_work_sync(&rt2x00dev->filter_work);
	if (work_pending(&rt2x00dev->config_work))
		cancel_work_sync(&rt2x00dev->config_work);

	/*
	 * Stop the TX queues.
	 */
	ieee80211_stop_queues(rt2x00dev->hw);

	/*
	 * Disable RX.
	 */
	rt2x00lib_toggle_rx(rt2x00dev, STATE_RADIO_RX_OFF);

	/*
	 * Disable radio.
	 */
	rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_RADIO_OFF);
}

void rt2x00lib_toggle_rx(struct rt2x00_dev *rt2x00dev, enum dev_state state)
{
	/*
	 * When we are disabling the RX, we should also stop the link tuner.
	 */
	if (state == STATE_RADIO_RX_OFF)
		rt2x00lib_stop_link_tuner(rt2x00dev);

	rt2x00dev->ops->lib->set_device_state(rt2x00dev, state);

	/*
	 * When we are enabling the RX, we should also start the link tuner.
	 */
	if (state == STATE_RADIO_RX_ON &&
	    is_interface_present(&rt2x00dev->interface))
		rt2x00lib_start_link_tuner(rt2x00dev);
}

static void rt2x00lib_precalculate_link_signal(struct link *link)
{
	if (link->rx_failed || link->rx_success)
		link->rx_percentage =
		    (link->rx_success * 100) /
		    (link->rx_failed + link->rx_success);
	else
		link->rx_percentage = 50;

	if (link->tx_failed || link->tx_success)
		link->tx_percentage =
		    (link->tx_success * 100) /
		    (link->tx_failed + link->tx_success);
	else
		link->tx_percentage = 50;

	link->rx_success = 0;
	link->rx_failed = 0;
	link->tx_success = 0;
	link->tx_failed = 0;
}

static int rt2x00lib_calculate_link_signal(struct rt2x00_dev *rt2x00dev,
					   int rssi)
{
	int rssi_percentage = 0;
	int signal;

	/*
	 * We need a positive value for the RSSI.
	 */
	if (rssi < 0)
		rssi += rt2x00dev->rssi_offset;

	/*
	 * Calculate the different percentages,
	 * which will be used for the signal.
	 */
	if (rt2x00dev->rssi_offset)
		rssi_percentage = (rssi * 100) / rt2x00dev->rssi_offset;

	/*
	 * Add the individual percentages and use the WEIGHT
	 * defines to calculate the current link signal.
	 */
	signal = ((WEIGHT_RSSI * rssi_percentage) +
		  (WEIGHT_TX * rt2x00dev->link.tx_percentage) +
		  (WEIGHT_RX * rt2x00dev->link.rx_percentage)) / 100;

	return (signal > 100) ? 100 : signal;
}

static void rt2x00lib_link_tuner(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, link.work.work);

	/*
	 * When the radio is shutting down we should
	 * immediately cease all link tuning.
	 */
	if (!test_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	/*
	 * Update statistics.
	 */
	rt2x00dev->ops->lib->link_stats(rt2x00dev);

	rt2x00dev->low_level_stats.dot11FCSErrorCount +=
	    rt2x00dev->link.rx_failed;

	/*
	 * Only perform the link tuning when Link tuning
	 * has been enabled (This could have been disabled from the EEPROM).
	 */
	if (!test_bit(CONFIG_DISABLE_LINK_TUNING, &rt2x00dev->flags))
		rt2x00dev->ops->lib->link_tuner(rt2x00dev);

	/*
	 * Precalculate a portion of the link signal which is
	 * in based on the tx/rx success/failure counters.
	 */
	rt2x00lib_precalculate_link_signal(&rt2x00dev->link);

	/*
	 * Increase tuner counter, and reschedule the next link tuner run.
	 */
	rt2x00dev->link.count++;
	queue_delayed_work(rt2x00dev->hw->workqueue, &rt2x00dev->link.work,
			   LINK_TUNE_INTERVAL);
}

static void rt2x00lib_packetfilter_scheduled(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, filter_work);
	unsigned int filter = rt2x00dev->interface.filter;

	/*
	 * Since we had stored the filter inside interface.filter,
	 * we should now clear that field. Otherwise the driver will
	 * assume nothing has changed (*total_flags will be compared
	 * to interface.filter to determine if any action is required).
	 */
	rt2x00dev->interface.filter = 0;

	rt2x00dev->ops->hw->configure_filter(rt2x00dev->hw,
					     filter, &filter, 0, NULL);
}

static void rt2x00lib_configuration_scheduled(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, config_work);
	int preamble = !test_bit(CONFIG_SHORT_PREAMBLE, &rt2x00dev->flags);

	rt2x00mac_erp_ie_changed(rt2x00dev->hw,
				 IEEE80211_ERP_CHANGE_PREAMBLE, 0, preamble);
}

/*
 * Interrupt context handlers.
 */
static void rt2x00lib_beacondone_scheduled(struct work_struct *work)
{
	struct rt2x00_dev *rt2x00dev =
	    container_of(work, struct rt2x00_dev, beacon_work);
	struct data_ring *ring =
	    rt2x00lib_get_ring(rt2x00dev, IEEE80211_TX_QUEUE_BEACON);
	struct data_entry *entry = rt2x00_get_data_entry(ring);
	struct sk_buff *skb;

	skb = ieee80211_beacon_get(rt2x00dev->hw,
				   rt2x00dev->interface.id,
				   &entry->tx_status.control);
	if (!skb)
		return;

	rt2x00dev->ops->hw->beacon_update(rt2x00dev->hw, skb,
					  &entry->tx_status.control);

	dev_kfree_skb(skb);
}

void rt2x00lib_beacondone(struct rt2x00_dev *rt2x00dev)
{
	if (!test_bit(DEVICE_ENABLED_RADIO, &rt2x00dev->flags))
		return;

	queue_work(rt2x00dev->hw->workqueue, &rt2x00dev->beacon_work);
}
EXPORT_SYMBOL_GPL(rt2x00lib_beacondone);

void rt2x00lib_txdone(struct data_entry *entry,
		      const int status, const int retry)
{
	struct rt2x00_dev *rt2x00dev = entry->ring->rt2x00dev;
	struct ieee80211_tx_status *tx_status = &entry->tx_status;
	struct ieee80211_low_level_stats *stats = &rt2x00dev->low_level_stats;
	int success = !!(status == TX_SUCCESS || status == TX_SUCCESS_RETRY);
	int fail = !!(status == TX_FAIL_RETRY || status == TX_FAIL_INVALID ||
		      status == TX_FAIL_OTHER);

	/*
	 * Update TX statistics.
	 */
	tx_status->flags = 0;
	tx_status->ack_signal = 0;
	tx_status->excessive_retries = (status == TX_FAIL_RETRY);
	tx_status->retry_count = retry;
	rt2x00dev->link.tx_success += success;
	rt2x00dev->link.tx_failed += retry + fail;

	if (!(tx_status->control.flags & IEEE80211_TXCTL_NO_ACK)) {
		if (success)
			tx_status->flags |= IEEE80211_TX_STATUS_ACK;
		else
			stats->dot11ACKFailureCount++;
	}

	tx_status->queue_length = entry->ring->stats.limit;
	tx_status->queue_number = tx_status->control.queue;

	if (tx_status->control.flags & IEEE80211_TXCTL_USE_RTS_CTS) {
		if (success)
			stats->dot11RTSSuccessCount++;
		else
			stats->dot11RTSFailureCount++;
	}

	/*
	 * Send the tx_status to mac80211,
	 * that method also cleans up the skb structure.
	 */
	ieee80211_tx_status_irqsafe(rt2x00dev->hw, entry->skb, tx_status);
	entry->skb = NULL;
}
EXPORT_SYMBOL_GPL(rt2x00lib_txdone);

void rt2x00lib_rxdone(struct data_entry *entry, struct sk_buff *skb,
		      struct rxdata_entry_desc *desc)
{
	struct rt2x00_dev *rt2x00dev = entry->ring->rt2x00dev;
	struct ieee80211_rx_status *rx_status = &rt2x00dev->rx_status;
	struct ieee80211_hw_mode *mode;
	struct ieee80211_rate *rate;
	unsigned int i;
	int val = 0;

	/*
	 * Update RX statistics.
	 */
	mode = &rt2x00dev->hwmodes[rt2x00dev->curr_hwmode];
	for (i = 0; i < mode->num_rates; i++) {
		rate = &mode->rates[i];

		/*
		 * When frame was received with an OFDM bitrate,
		 * the signal is the PLCP value. If it was received with
		 * a CCK bitrate the signal is the rate in 0.5kbit/s.
		 */
		if (!desc->ofdm)
			val = DEVICE_GET_RATE_FIELD(rate->val, RATE);
		else
			val = DEVICE_GET_RATE_FIELD(rate->val, PLCP);

		if (val == desc->signal) {
			val = rate->val;
			break;
		}
	}

	rt2x00_update_link_rssi(&rt2x00dev->link, desc->rssi);
	rt2x00dev->link.rx_success++;
	rx_status->rate = val;
	rx_status->signal =
	    rt2x00lib_calculate_link_signal(rt2x00dev, desc->rssi);
	rx_status->ssi = desc->rssi;
	rx_status->flag = desc->flags;

	/*
	 * Send frame to mac80211
	 */
	ieee80211_rx_irqsafe(rt2x00dev->hw, skb, rx_status);
}
EXPORT_SYMBOL_GPL(rt2x00lib_rxdone);

/*
 * TX descriptor initializer
 */
void rt2x00lib_write_tx_desc(struct rt2x00_dev *rt2x00dev,
			     struct data_desc *txd,
			     struct ieee80211_hdr *ieee80211hdr,
			     unsigned int length,
			     struct ieee80211_tx_control *control)
{
	struct txdata_entry_desc desc;
	struct data_ring *ring;
	int tx_rate;
	int bitrate;
	int duration;
	int residual;
	u16 frame_control;
	u16 seq_ctrl;

	/*
	 * Make sure the descriptor is properly cleared.
	 */
	memset(&desc, 0x00, sizeof(desc));

	/*
	 * Get ring pointer, if we fail to obtain the
	 * correct ring, then use the first TX ring.
	 */
	ring = rt2x00lib_get_ring(rt2x00dev, control->queue);
	if (!ring)
		ring = rt2x00lib_get_ring(rt2x00dev, IEEE80211_TX_QUEUE_DATA0);

	desc.cw_min = ring->tx_params.cw_min;
	desc.cw_max = ring->tx_params.cw_max;
	desc.aifs = ring->tx_params.aifs;

	/*
	 * Identify queue
	 */
	if (control->queue < rt2x00dev->hw->queues)
		desc.queue = control->queue;
	else if (control->queue == IEEE80211_TX_QUEUE_BEACON ||
		 control->queue == IEEE80211_TX_QUEUE_AFTER_BEACON)
		desc.queue = QUEUE_MGMT;
	else
		desc.queue = QUEUE_OTHER;

	/*
	 * Read required fields from ieee80211 header.
	 */
	frame_control = le16_to_cpu(ieee80211hdr->frame_control);
	seq_ctrl = le16_to_cpu(ieee80211hdr->seq_ctrl);

	tx_rate = control->tx_rate;

	/*
	 * Check if this is a RTS/CTS frame
	 */
	if (is_rts_frame(frame_control) || is_cts_frame(frame_control)) {
		__set_bit(ENTRY_TXD_BURST, &desc.flags);
		if (is_rts_frame(frame_control))
			__set_bit(ENTRY_TXD_RTS_FRAME, &desc.flags);
		if (control->rts_cts_rate)
			tx_rate = control->rts_cts_rate;
	}

	/*
	 * Check for OFDM
	 */
	if (DEVICE_GET_RATE_FIELD(tx_rate, RATEMASK) & DEV_OFDM_RATEMASK)
		__set_bit(ENTRY_TXD_OFDM_RATE, &desc.flags);

	/*
	 * Check if more fragments are pending
	 */
	if (ieee80211_get_morefrag(ieee80211hdr)) {
		__set_bit(ENTRY_TXD_BURST, &desc.flags);
		__set_bit(ENTRY_TXD_MORE_FRAG, &desc.flags);
	}

	/*
	 * Beacons and probe responses require the tsf timestamp
	 * to be inserted into the frame.
	 */
	if (control->queue == IEEE80211_TX_QUEUE_BEACON ||
	    is_probe_resp(frame_control))
		__set_bit(ENTRY_TXD_REQ_TIMESTAMP, &desc.flags);

	/*
	 * Determine with what IFS priority this frame should be send.
	 * Set ifs to IFS_SIFS when the this is not the first fragment,
	 * or this fragment came after RTS/CTS.
	 */
	if ((seq_ctrl & IEEE80211_SCTL_FRAG) > 0 ||
	    test_bit(ENTRY_TXD_RTS_FRAME, &desc.flags))
		desc.ifs = IFS_SIFS;
	else
		desc.ifs = IFS_BACKOFF;

	/*
	 * PLCP setup
	 * Length calculation depends on OFDM/CCK rate.
	 */
	desc.signal = DEVICE_GET_RATE_FIELD(tx_rate, PLCP);
	desc.service = 0x04;

	if (test_bit(ENTRY_TXD_OFDM_RATE, &desc.flags)) {
		desc.length_high = ((length + FCS_LEN) >> 6) & 0x3f;
		desc.length_low = ((length + FCS_LEN) & 0x3f);
	} else {
		bitrate = DEVICE_GET_RATE_FIELD(tx_rate, RATE);

		/*
		 * Convert length to microseconds.
		 */
		residual = get_duration_res(length + FCS_LEN, bitrate);
		duration = get_duration(length + FCS_LEN, bitrate);

		if (residual != 0) {
			duration++;

			/*
			 * Check if we need to set the Length Extension
			 */
			if (bitrate == 110 && residual <= 30)
				desc.service |= 0x80;
		}

		desc.length_high = (duration >> 8) & 0xff;
		desc.length_low = duration & 0xff;

		/*
		 * When preamble is enabled we should set the
		 * preamble bit for the signal.
		 */
		if (DEVICE_GET_RATE_FIELD(tx_rate, PREAMBLE))
			desc.signal |= 0x08;
	}

	rt2x00dev->ops->lib->write_tx_desc(rt2x00dev, txd, &desc,
					   ieee80211hdr, length, control);
}
EXPORT_SYMBOL_GPL(rt2x00lib_write_tx_desc);

/*
 * Driver initialization handlers.
 */
static void rt2x00lib_channel(struct ieee80211_channel *entry,
			      const int channel, const int tx_power,
			      const int value)
{
	entry->chan = channel;
	if (channel <= 14)
		entry->freq = 2407 + (5 * channel);
	else
		entry->freq = 5000 + (5 * channel);
	entry->val = value;
	entry->flag =
	    IEEE80211_CHAN_W_IBSS |
	    IEEE80211_CHAN_W_ACTIVE_SCAN |
	    IEEE80211_CHAN_W_SCAN;
	entry->power_level = tx_power;
	entry->antenna_max = 0xff;
}

static void rt2x00lib_rate(struct ieee80211_rate *entry,
			   const int rate, const int mask,
			   const int plcp, const int flags)
{
	entry->rate = rate;
	entry->val =
	    DEVICE_SET_RATE_FIELD(rate, RATE) |
	    DEVICE_SET_RATE_FIELD(mask, RATEMASK) |
	    DEVICE_SET_RATE_FIELD(plcp, PLCP);
	entry->flags = flags;
	entry->val2 = entry->val;
	if (entry->flags & IEEE80211_RATE_PREAMBLE2)
		entry->val2 |= DEVICE_SET_RATE_FIELD(1, PREAMBLE);
	entry->min_rssi_ack = 0;
	entry->min_rssi_ack_delta = 0;
}

static int rt2x00lib_probe_hw_modes(struct rt2x00_dev *rt2x00dev,
				    struct hw_mode_spec *spec)
{
	struct ieee80211_hw *hw = rt2x00dev->hw;
	struct ieee80211_hw_mode *hwmodes;
	struct ieee80211_channel *channels;
	struct ieee80211_rate *rates;
	unsigned int i;
	unsigned char tx_power;

	hwmodes = kzalloc(sizeof(*hwmodes) * spec->num_modes, GFP_KERNEL);
	if (!hwmodes)
		goto exit;

	channels = kzalloc(sizeof(*channels) * spec->num_channels, GFP_KERNEL);
	if (!channels)
		goto exit_free_modes;

	rates = kzalloc(sizeof(*rates) * spec->num_rates, GFP_KERNEL);
	if (!rates)
		goto exit_free_channels;

	/*
	 * Initialize Rate list.
	 */
	rt2x00lib_rate(&rates[0], 10, DEV_RATEMASK_1MB,
		       0x00, IEEE80211_RATE_CCK);
	rt2x00lib_rate(&rates[1], 20, DEV_RATEMASK_2MB,
		       0x01, IEEE80211_RATE_CCK_2);
	rt2x00lib_rate(&rates[2], 55, DEV_RATEMASK_5_5MB,
		       0x02, IEEE80211_RATE_CCK_2);
	rt2x00lib_rate(&rates[3], 110, DEV_RATEMASK_11MB,
		       0x03, IEEE80211_RATE_CCK_2);

	if (spec->num_rates > 4) {
		rt2x00lib_rate(&rates[4], 60, DEV_RATEMASK_6MB,
			       0x0b, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[5], 90, DEV_RATEMASK_9MB,
			       0x0f, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[6], 120, DEV_RATEMASK_12MB,
			       0x0a, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[7], 180, DEV_RATEMASK_18MB,
			       0x0e, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[8], 240, DEV_RATEMASK_24MB,
			       0x09, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[9], 360, DEV_RATEMASK_36MB,
			       0x0d, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[10], 480, DEV_RATEMASK_48MB,
			       0x08, IEEE80211_RATE_OFDM);
		rt2x00lib_rate(&rates[11], 540, DEV_RATEMASK_54MB,
			       0x0c, IEEE80211_RATE_OFDM);
	}

	/*
	 * Initialize Channel list.
	 */
	for (i = 0; i < spec->num_channels; i++) {
		if (spec->channels[i].channel <= 14)
			tx_power = spec->tx_power_bg[i];
		else if (spec->tx_power_a)
			tx_power = spec->tx_power_a[i];
		else
			tx_power = spec->tx_power_default;

		rt2x00lib_channel(&channels[i],
				  spec->channels[i].channel, tx_power, i);
	}

	/*
	 * Intitialize 802.11b
	 * Rates: CCK.
	 * Channels: OFDM.
	 */
	if (spec->num_modes > HWMODE_B) {
		hwmodes[HWMODE_B].mode = MODE_IEEE80211B;
		hwmodes[HWMODE_B].num_channels = 14;
		hwmodes[HWMODE_B].num_rates = 4;
		hwmodes[HWMODE_B].channels = channels;
		hwmodes[HWMODE_B].rates = rates;
	}

	/*
	 * Intitialize 802.11g
	 * Rates: CCK, OFDM.
	 * Channels: OFDM.
	 */
	if (spec->num_modes > HWMODE_G) {
		hwmodes[HWMODE_G].mode = MODE_IEEE80211G;
		hwmodes[HWMODE_G].num_channels = 14;
		hwmodes[HWMODE_G].num_rates = spec->num_rates;
		hwmodes[HWMODE_G].channels = channels;
		hwmodes[HWMODE_G].rates = rates;
	}

	/*
	 * Intitialize 802.11a
	 * Rates: OFDM.
	 * Channels: OFDM, UNII, HiperLAN2.
	 */
	if (spec->num_modes > HWMODE_A) {
		hwmodes[HWMODE_A].mode = MODE_IEEE80211A;
		hwmodes[HWMODE_A].num_channels = spec->num_channels - 14;
		hwmodes[HWMODE_A].num_rates = spec->num_rates - 4;
		hwmodes[HWMODE_A].channels = &channels[14];
		hwmodes[HWMODE_A].rates = &rates[4];
	}

	if (spec->num_modes > HWMODE_G &&
	    ieee80211_register_hwmode(hw, &hwmodes[HWMODE_G]))
		goto exit_free_rates;

	if (spec->num_modes > HWMODE_B &&
	    ieee80211_register_hwmode(hw, &hwmodes[HWMODE_B]))
		goto exit_free_rates;

	if (spec->num_modes > HWMODE_A &&
	    ieee80211_register_hwmode(hw, &hwmodes[HWMODE_A]))
		goto exit_free_rates;

	rt2x00dev->hwmodes = hwmodes;

	return 0;

exit_free_rates:
	kfree(rates);

exit_free_channels:
	kfree(channels);

exit_free_modes:
	kfree(hwmodes);

exit:
	ERROR(rt2x00dev, "Allocation ieee80211 modes failed.\n");
	return -ENOMEM;
}

static void rt2x00lib_remove_hw(struct rt2x00_dev *rt2x00dev)
{
	if (test_bit(DEVICE_REGISTERED_HW, &rt2x00dev->flags))
		ieee80211_unregister_hw(rt2x00dev->hw);

	if (likely(rt2x00dev->hwmodes)) {
		kfree(rt2x00dev->hwmodes->channels);
		kfree(rt2x00dev->hwmodes->rates);
		kfree(rt2x00dev->hwmodes);
		rt2x00dev->hwmodes = NULL;
	}
}

static int rt2x00lib_probe_hw(struct rt2x00_dev *rt2x00dev)
{
	struct hw_mode_spec *spec = &rt2x00dev->spec;
	int status;

	/*
	 * Initialize HW modes.
	 */
	status = rt2x00lib_probe_hw_modes(rt2x00dev, spec);
	if (status)
		return status;

	/*
	 * Register HW.
	 */
	status = ieee80211_register_hw(rt2x00dev->hw);
	if (status) {
		rt2x00lib_remove_hw(rt2x00dev);
		return status;
	}

	__set_bit(DEVICE_REGISTERED_HW, &rt2x00dev->flags);

	return 0;
}

/*
 * Initialization/uninitialization handlers.
 */
static int rt2x00lib_alloc_entries(struct data_ring *ring,
				   const u16 max_entries, const u16 data_size,
				   const u16 desc_size)
{
	struct data_entry *entry;
	unsigned int i;

	ring->stats.limit = max_entries;
	ring->data_size = data_size;
	ring->desc_size = desc_size;

	/*
	 * Allocate all ring entries.
	 */
	entry = kzalloc(ring->stats.limit * sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	for (i = 0; i < ring->stats.limit; i++) {
		entry[i].flags = 0;
		entry[i].ring = ring;
		entry[i].skb = NULL;
	}

	ring->entry = entry;

	return 0;
}

static int rt2x00lib_alloc_ring_entries(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;

	/*
	 * Allocate the RX ring.
	 */
	if (rt2x00lib_alloc_entries(rt2x00dev->rx, RX_ENTRIES, DATA_FRAME_SIZE,
				    rt2x00dev->ops->rxd_size))
		return -ENOMEM;

	/*
	 * First allocate the TX rings.
	 */
	txring_for_each(rt2x00dev, ring) {
		if (rt2x00lib_alloc_entries(ring, TX_ENTRIES, DATA_FRAME_SIZE,
					    rt2x00dev->ops->txd_size))
			return -ENOMEM;
	}

	if (!test_bit(DRIVER_REQUIRE_BEACON_RING, &rt2x00dev->flags))
		return 0;

	/*
	 * Allocate the BEACON ring.
	 */
	if (rt2x00lib_alloc_entries(&rt2x00dev->bcn[0], BEACON_ENTRIES,
				    MGMT_FRAME_SIZE, rt2x00dev->ops->txd_size))
		return -ENOMEM;

	/*
	 * Allocate the Atim ring.
	 */
	if (rt2x00lib_alloc_entries(&rt2x00dev->bcn[1], ATIM_ENTRIES,
				    DATA_FRAME_SIZE, rt2x00dev->ops->txd_size))
		return -ENOMEM;

	return 0;
}

static void rt2x00lib_free_ring_entries(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;

	ring_for_each(rt2x00dev, ring) {
		kfree(ring->entry);
		ring->entry = NULL;
	}
}

void rt2x00lib_uninitialize(struct rt2x00_dev *rt2x00dev)
{
	if (!__test_and_clear_bit(DEVICE_INITIALIZED, &rt2x00dev->flags))
		return;

	/*
	 * Unregister rfkill.
	 */
	rt2x00rfkill_unregister(rt2x00dev);

	/*
	 * Allow the HW to uninitialize.
	 */
	rt2x00dev->ops->lib->uninitialize(rt2x00dev);

	/*
	 * Free allocated ring entries.
	 */
	rt2x00lib_free_ring_entries(rt2x00dev);
}

int rt2x00lib_initialize(struct rt2x00_dev *rt2x00dev)
{
	int status;

	if (test_bit(DEVICE_INITIALIZED, &rt2x00dev->flags))
		return 0;

	/*
	 * Allocate all ring entries.
	 */
	status = rt2x00lib_alloc_ring_entries(rt2x00dev);
	if (status) {
		ERROR(rt2x00dev, "Ring entries allocation failed.\n");
		return status;
	}

	/*
	 * Initialize the device.
	 */
	status = rt2x00dev->ops->lib->initialize(rt2x00dev);
	if (status)
		goto exit;

	__set_bit(DEVICE_INITIALIZED, &rt2x00dev->flags);

	/*
	 * Register the rfkill handler.
	 */
	status = rt2x00rfkill_register(rt2x00dev);
	if (status)
		goto exit_unitialize;

	return 0;

exit_unitialize:
	rt2x00lib_uninitialize(rt2x00dev);

exit:
	rt2x00lib_free_ring_entries(rt2x00dev);

	return status;
}

/*
 * driver allocation handlers.
 */
static int rt2x00lib_alloc_rings(struct rt2x00_dev *rt2x00dev)
{
	struct data_ring *ring;

	/*
	 * We need the following rings:
	 * RX: 1
	 * TX: hw->queues
	 * Beacon: 1 (if required)
	 * Atim: 1 (if required)
	 */
	rt2x00dev->data_rings = 1 + rt2x00dev->hw->queues +
	    (2 * test_bit(DRIVER_REQUIRE_BEACON_RING, &rt2x00dev->flags));

	ring = kzalloc(rt2x00dev->data_rings * sizeof(*ring), GFP_KERNEL);
	if (!ring) {
		ERROR(rt2x00dev, "Ring allocation failed.\n");
		return -ENOMEM;
	}

	/*
	 * Initialize pointers
	 */
	rt2x00dev->rx = ring;
	rt2x00dev->tx = &rt2x00dev->rx[1];
	if (test_bit(DRIVER_REQUIRE_BEACON_RING, &rt2x00dev->flags))
		rt2x00dev->bcn = &rt2x00dev->tx[rt2x00dev->hw->queues];

	/*
	 * Initialize ring parameters.
	 * cw_min: 2^5 = 32.
	 * cw_max: 2^10 = 1024.
	 */
	ring_for_each(rt2x00dev, ring) {
		ring->rt2x00dev = rt2x00dev;
		ring->tx_params.aifs = 2;
		ring->tx_params.cw_min = 5;
		ring->tx_params.cw_max = 10;
	}

	return 0;
}

static void rt2x00lib_free_rings(struct rt2x00_dev *rt2x00dev)
{
	kfree(rt2x00dev->rx);
	rt2x00dev->rx = NULL;
	rt2x00dev->tx = NULL;
	rt2x00dev->bcn = NULL;
}

int rt2x00lib_probe_dev(struct rt2x00_dev *rt2x00dev)
{
	int retval = -ENOMEM;

	/*
	 * Let the driver probe the device to detect the capabilities.
	 */
	retval = rt2x00dev->ops->lib->probe_hw(rt2x00dev);
	if (retval) {
		ERROR(rt2x00dev, "Failed to allocate device.\n");
		goto exit;
	}

	/*
	 * Initialize configuration work.
	 */
	INIT_WORK(&rt2x00dev->beacon_work, rt2x00lib_beacondone_scheduled);
	INIT_WORK(&rt2x00dev->filter_work, rt2x00lib_packetfilter_scheduled);
	INIT_WORK(&rt2x00dev->config_work, rt2x00lib_configuration_scheduled);
	INIT_DELAYED_WORK(&rt2x00dev->link.work, rt2x00lib_link_tuner);

	/*
	 * Reset current working type.
	 */
	rt2x00dev->interface.type = INVALID_INTERFACE;

	/*
	 * Allocate ring array.
	 */
	retval = rt2x00lib_alloc_rings(rt2x00dev);
	if (retval)
		goto exit;

	/*
	 * Initialize ieee80211 structure.
	 */
	retval = rt2x00lib_probe_hw(rt2x00dev);
	if (retval) {
		ERROR(rt2x00dev, "Failed to initialize hw.\n");
		goto exit;
	}

	/*
	 * Allocatie rfkill.
	 */
	retval = rt2x00rfkill_allocate(rt2x00dev);
	if (retval)
		goto exit;

	/*
	 * Open the debugfs entry.
	 */
	rt2x00debug_register(rt2x00dev);

	__set_bit(DEVICE_PRESENT, &rt2x00dev->flags);

	return 0;

exit:
	rt2x00lib_remove_dev(rt2x00dev);

	return retval;
}
EXPORT_SYMBOL_GPL(rt2x00lib_probe_dev);

void rt2x00lib_remove_dev(struct rt2x00_dev *rt2x00dev)
{
	__clear_bit(DEVICE_PRESENT, &rt2x00dev->flags);

	/*
	 * Disable radio.
	 */
	rt2x00lib_disable_radio(rt2x00dev);

	/*
	 * Uninitialize device.
	 */
	rt2x00lib_uninitialize(rt2x00dev);

	/*
	 * Close debugfs entry.
	 */
	rt2x00debug_deregister(rt2x00dev);

	/*
	 * Free rfkill
	 */
	rt2x00rfkill_free(rt2x00dev);

	/*
	 * Free ieee80211_hw memory.
	 */
	rt2x00lib_remove_hw(rt2x00dev);

	/*
	 * Free firmware image.
	 */
	rt2x00lib_free_firmware(rt2x00dev);

	/*
	 * Free ring structures.
	 */
	rt2x00lib_free_rings(rt2x00dev);
}
EXPORT_SYMBOL_GPL(rt2x00lib_remove_dev);

/*
 * Device state handlers
 */
#ifdef CONFIG_PM
int rt2x00lib_suspend(struct rt2x00_dev *rt2x00dev, pm_message_t state)
{
	int retval;

	NOTICE(rt2x00dev, "Going to sleep.\n");
	__clear_bit(DEVICE_PRESENT, &rt2x00dev->flags);

	/*
	 * Only continue if mac80211 has open interfaces.
	 */
	if (!test_bit(DEVICE_STARTED, &rt2x00dev->flags))
		goto exit;
	__set_bit(DEVICE_STARTED_SUSPEND, &rt2x00dev->flags);

	/*
	 * Disable radio and unitialize all items
	 * that must be recreated on resume.
	 */
	rt2x00mac_stop(rt2x00dev->hw);
	rt2x00lib_uninitialize(rt2x00dev);
	rt2x00debug_deregister(rt2x00dev);

exit:
	/*
	 * Set device mode to sleep for power management.
	 */
	retval = rt2x00dev->ops->lib->set_device_state(rt2x00dev, STATE_SLEEP);
	if (retval)
		return retval;

	return 0;
}
EXPORT_SYMBOL_GPL(rt2x00lib_suspend);

int rt2x00lib_resume(struct rt2x00_dev *rt2x00dev)
{
	struct interface *intf = &rt2x00dev->interface;
	int retval;

	NOTICE(rt2x00dev, "Waking up.\n");
	__set_bit(DEVICE_PRESENT, &rt2x00dev->flags);

	/*
	 * Open the debugfs entry.
	 */
	rt2x00debug_register(rt2x00dev);

	/*
	 * Only continue if mac80211 had open interfaces.
	 */
	if (!__test_and_clear_bit(DEVICE_STARTED_SUSPEND, &rt2x00dev->flags))
		return 0;

	/*
	 * Reinitialize device and all active interfaces.
	 */
	retval = rt2x00mac_start(rt2x00dev->hw);
	if (retval)
		goto exit;

	/*
	 * Reconfigure device.
	 */
	rt2x00lib_config(rt2x00dev, &rt2x00dev->hw->conf, 1);
	if (!rt2x00dev->hw->conf.radio_enabled)
		rt2x00lib_disable_radio(rt2x00dev);

	rt2x00lib_config_mac_addr(rt2x00dev, intf->mac);
	rt2x00lib_config_bssid(rt2x00dev, intf->bssid);
	rt2x00lib_config_type(rt2x00dev, intf->type);

	/*
	 * It is possible that during that mac80211 has attempted
	 * to send frames while we were suspending or resuming.
	 * In that case we have disabled the TX queue and should
	 * now enable it again
	 */
	ieee80211_start_queues(rt2x00dev->hw);

	/*
	 * When in Master or Ad-hoc mode,
	 * restart Beacon transmitting by faking a beacondone event.
	 */
	if (intf->type == IEEE80211_IF_TYPE_AP ||
	    intf->type == IEEE80211_IF_TYPE_IBSS)
		rt2x00lib_beacondone(rt2x00dev);

	return 0;

exit:
	rt2x00lib_disable_radio(rt2x00dev);
	rt2x00lib_uninitialize(rt2x00dev);
	rt2x00debug_deregister(rt2x00dev);

	return retval;
}
EXPORT_SYMBOL_GPL(rt2x00lib_resume);
#endif /* CONFIG_PM */

/*
 * rt2x00lib module information.
 */
MODULE_AUTHOR(DRV_PROJECT);
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("rt2x00 library");
MODULE_LICENSE("GPL");
