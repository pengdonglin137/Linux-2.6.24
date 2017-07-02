/**
  * This file contains definitions and data structures specific
  * to Marvell 802.11 NIC. It contains the Device Information
  * structure wlan_adapter.
  */
#ifndef _WLAN_DEV_H_
#define _WLAN_DEV_H_

#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/ethtool.h>
#include <linux/debugfs.h>
#include <net/ieee80211.h>

#include "defs.h"
#include "scan.h"

extern struct ethtool_ops libertas_ethtool_ops;

#define	MAX_BSSID_PER_CHANNEL		16

#define NR_TX_QUEUE			3

/* For the extended Scan */
#define MAX_EXTENDED_SCAN_BSSID_LIST    MAX_BSSID_PER_CHANNEL * \
						MRVDRV_MAX_CHANNEL_SIZE + 1

#define	MAX_REGION_CHANNEL_NUM	2

/** Chan-freq-TxPower mapping table*/
struct chan_freq_power {
	/** channel Number		*/
	u16 channel;
	/** frequency of this channel	*/
	u32 freq;
	/** Max allowed Tx power level	*/
	u16 maxtxpower;
	/** TRUE:channel unsupported;  FLASE:supported*/
	u8 unsupported;
};

/** region-band mapping table*/
struct region_channel {
	/** TRUE if this entry is valid		     */
	u8 valid;
	/** region code for US, Japan ...	     */
	u8 region;
	/** band B/G/A, used for BAND_CONFIG cmd	     */
	u8 band;
	/** Actual No. of elements in the array below */
	u8 nrcfp;
	/** chan-freq-txpower mapping table*/
	struct chan_freq_power *CFP;
};

struct wlan_802_11_security {
	u8 WPAenabled;
	u8 WPA2enabled;
	u8 wep_enabled;
	u8 auth_mode;
};

/** Current Basic Service Set State Structure */
struct current_bss_params {
	/** bssid */
	u8 bssid[ETH_ALEN];
	/** ssid */
	u8 ssid[IW_ESSID_MAX_SIZE + 1];
	u8 ssid_len;

	/** band */
	u8 band;
	/** channel */
	u8 channel;
	/** zero-terminated array of supported data rates */
	u8 rates[MAX_RATES + 1];
};

/** sleep_params */
struct sleep_params {
	u16 sp_error;
	u16 sp_offset;
	u16 sp_stabletime;
	u8 sp_calcontrol;
	u8 sp_extsleepclk;
	u16 sp_reserved;
};

/* Mesh statistics */
struct wlan_mesh_stats {
	u32	fwd_bcast_cnt;		/* Fwd: Broadcast counter */
	u32	fwd_unicast_cnt;	/* Fwd: Unicast counter */
	u32	fwd_drop_ttl;		/* Fwd: TTL zero */
	u32	fwd_drop_rbt;		/* Fwd: Recently Broadcasted */
	u32	fwd_drop_noroute; 	/* Fwd: No route to Destination */
	u32	fwd_drop_nobuf;		/* Fwd: Run out of internal buffers */
	u32	drop_blind;		/* Rx:  Dropped by blinding table */
	u32	tx_failed_cnt;		/* Tx:  Failed transmissions */
};

/** Private structure for the MV device */
struct _wlan_private {
	int open;
	int mesh_open;
	int infra_open;
	int mesh_autostart_enabled;
	__le16 boot2_version;

	char name[DEV_NAME_LEN];

	void *card;
	wlan_adapter *adapter;
	struct net_device *dev;

	struct net_device_stats stats;
	struct net_device *mesh_dev; /* Virtual device */
	struct net_device *rtap_net_dev;
	struct ieee80211_device *ieee;

	struct iw_statistics wstats;
	struct wlan_mesh_stats mstats;
	struct dentry *debugfs_dir;
	struct dentry *debugfs_debug;
	struct dentry *debugfs_files[6];

	struct dentry *events_dir;
	struct dentry *debugfs_events_files[6];

	struct dentry *regs_dir;
	struct dentry *debugfs_regs_files[6];

	u32 mac_offset;
	u32 bbp_offset;
	u32 rf_offset;

	/** Upload length */
	u32 upld_len;
	/* Upload buffer */
	u8 upld_buf[WLAN_UPLD_SIZE];
	/* Download sent:
	   bit0 1/0=data_sent/data_tx_done,
	   bit1 1/0=cmd_sent/cmd_tx_done,
	   all other bits reserved 0 */
	u8 dnld_sent;

	struct device *hotplug_device;

	/** thread to service interrupts */
	struct task_struct *main_thread;
	wait_queue_head_t waitq;
	struct workqueue_struct *work_thread;

	struct delayed_work scan_work;
	struct delayed_work assoc_work;
	struct work_struct sync_channel;

	/** Hardware access */
	int (*hw_host_to_card) (wlan_private * priv, u8 type, u8 * payload, u16 nb);
	int (*hw_get_int_status) (wlan_private * priv, u8 *);
	int (*hw_read_event_cause) (wlan_private *);
};

/** Association request
 *
 * Encapsulates all the options that describe a specific assocation request
 * or configuration of the wireless card's radio, mode, and security settings.
 */
struct assoc_request {
#define ASSOC_FLAG_SSID			1
#define ASSOC_FLAG_CHANNEL		2
#define ASSOC_FLAG_BAND			3
#define ASSOC_FLAG_MODE			4
#define ASSOC_FLAG_BSSID		5
#define ASSOC_FLAG_WEP_KEYS		6
#define ASSOC_FLAG_WEP_TX_KEYIDX	7
#define ASSOC_FLAG_WPA_MCAST_KEY	8
#define ASSOC_FLAG_WPA_UCAST_KEY	9
#define ASSOC_FLAG_SECINFO		10
#define ASSOC_FLAG_WPA_IE		11
	unsigned long flags;

	u8 ssid[IW_ESSID_MAX_SIZE + 1];
	u8 ssid_len;
	u8 channel;
	u8 band;
	u8 mode;
	u8 bssid[ETH_ALEN];

	/** WEP keys */
	struct enc_key wep_keys[4];
	u16 wep_tx_keyidx;

	/** WPA keys */
	struct enc_key wpa_mcast_key;
	struct enc_key wpa_unicast_key;

	struct wlan_802_11_security secinfo;

	/** WPA Information Elements*/
	u8 wpa_ie[MAX_WPA_IE_LEN];
	u8 wpa_ie_len;

	/* BSS to associate with for infrastructure of Ad-Hoc join */
	struct bss_descriptor bss;
};

/** Wlan adapter data structure*/
struct _wlan_adapter {
	/** STATUS variables */
	u8 fwreleasenumber[4];
	u32 fwcapinfo;
	/* protected with big lock */

	struct mutex lock;

	u8 tmptxbuf[WLAN_UPLD_SIZE];
	/* protected by hard_start_xmit serialization */

	/** command-related variables */
	u16 seqnum;
	/* protected by big lock */

	struct cmd_ctrl_node *cmd_array;
	/** Current command */
	struct cmd_ctrl_node *cur_cmd;
	int cur_cmd_retcode;
	/** command Queues */
	/** Free command buffers */
	struct list_head cmdfreeq;
	/** Pending command buffers */
	struct list_head cmdpendingq;

	wait_queue_head_t cmd_pending;
	u8 nr_cmd_pending;
	/* command related variables protected by adapter->driver_lock */

	/** Async and Sync Event variables */
	u32 intcounter;
	u32 eventcause;
	u8 nodename[16];	/* nickname */

	/** spin locks */
	spinlock_t driver_lock;

	/** Timers */
	struct timer_list command_timer;

	/* TX queue used in PS mode */
	spinlock_t txqueue_lock;
	struct sk_buff *tx_queue_ps[NR_TX_QUEUE];
	unsigned int tx_queue_idx;

	u8 hisregcpy;

	/** current ssid/bssid related parameters*/
	struct current_bss_params curbssparams;

	/* IW_MODE_* */
	u8 mode;

	/* Scan results list */
	struct list_head network_list;
	struct list_head network_free_list;
	struct bss_descriptor *networks;

	u8 adhoccreate;

	/** capability Info used in Association, start, join */
	u16 capability;

	/** MAC address information */
	u8 current_addr[ETH_ALEN];
	u8 multicastlist[MRVDRV_MAX_MULTICAST_LIST_SIZE][ETH_ALEN];
	u32 nr_of_multicastmacaddr;

	/** 802.11 statistics */
//	struct cmd_DS_802_11_GET_STAT wlan802_11Stat;

	u16 enablehwauto;
	u16 ratebitmap;

	u32 fragthsd;
	u32 rtsthsd;

	u8 txretrycount;

	/** Tx-related variables (for single packet tx) */
	struct sk_buff *currenttxskb;
	u16 TxLockFlag;

	/** NIC Operation characteristics */
	u16 currentpacketfilter;
	u32 connect_status;
	u16 regioncode;
	u16 txpowerlevel;

	/** POWER MANAGEMENT AND PnP SUPPORT */
	u8 surpriseremoved;

	u16 psmode;		/* Wlan802_11PowermodeCAM=disable
				   Wlan802_11PowermodeMAX_PSP=enable */
	u32 psstate;
	u8 needtowakeup;

	struct PS_CMD_ConfirmSleep libertas_ps_confirm_sleep;

	struct assoc_request * pending_assoc_req;
	struct assoc_request * in_progress_assoc_req;

	/** Encryption parameter */
	struct wlan_802_11_security secinfo;

	/** WEP keys */
	struct enc_key wep_keys[4];
	u16 wep_tx_keyidx;

	/** WPA keys */
	struct enc_key wpa_mcast_key;
	struct enc_key wpa_unicast_key;

	/** WPA Information Elements*/
	u8 wpa_ie[MAX_WPA_IE_LEN];
	u8 wpa_ie_len;

	/** Requested Signal Strength*/
	u16 SNR[MAX_TYPE_B][MAX_TYPE_AVG];
	u16 NF[MAX_TYPE_B][MAX_TYPE_AVG];
	u8 RSSI[MAX_TYPE_B][MAX_TYPE_AVG];
	u8 rawSNR[DEFAULT_DATA_AVG_FACTOR];
	u8 rawNF[DEFAULT_DATA_AVG_FACTOR];
	u16 nextSNRNF;
	u16 numSNRNF;

	u8 radioon;
	u32 preamble;

	/** data rate stuff */
	u8 cur_rate;
	u8 auto_rate;

	/** sleep_params */
	struct sleep_params sp;

	/** RF calibration data */

#define	MAX_REGION_CHANNEL_NUM	2
	/** region channel data */
	struct region_channel region_channel[MAX_REGION_CHANNEL_NUM];

	struct region_channel universal_channel[MAX_REGION_CHANNEL_NUM];

	/** 11D and Domain Regulatory Data */
	struct wlan_802_11d_domain_reg domainreg;
	struct parsed_region_chan_11d parsed_region_chan;

	/** FSM variable for 11d support */
	u32 enable11d;

	/**	MISCELLANEOUS */
	u8 *prdeeprom;
	struct wlan_offset_value offsetvalue;

	struct cmd_ds_802_11_get_log logmsg;

	u32 monitormode;
	u8 fw_ready;

	u8 last_scanned_channel;
};

#endif				/* _WLAN_DEV_H_ */
