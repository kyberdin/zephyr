/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/types.h>


#include <console/console.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/iso.h>
#include <sys/byteorder.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(iso_connected, LOG_LEVEL_DBG);

#define DEVICE_NAME	CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME))

#define ROLE_CENTRAL 0
#define ROLE_PERIPHERAL 1
#define ROLE_QUIT 2

#define DEFAULT_CIS_RTN         0
#define DEFAULT_CIS_INTERVAL_US 7500
#define DEFAULT_CIS_LATENCY_MS  10
#define DEFAULT_CIS_PHY         BT_GAP_LE_PHY_2M
#define DEFAULT_CIS_SDU         CONFIG_BT_ISO_TX_MTU
#define DEFAULT_CIS_PACKING     0
#define DEFAULT_CIS_FRAMING     0
#define DEFAULT_CIS_COUNT       CONFIG_BT_ISO_MAX_CHAN
#define DEFAULT_CIS_SEC_LEVEL   BT_SECURITY_L1

#define ADV_PARAM BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | \
					BT_LE_ADV_OPT_ONE_TIME | \
					BT_LE_ADV_OPT_USE_NAME, \
				  BT_GAP_ADV_FAST_INT_MIN_1, \
				  BT_GAP_ADV_FAST_INT_MAX_1, \
				  NULL)

struct iso_recv_stats {
	uint32_t     iso_recv_count;
	uint32_t     iso_lost_count;
};

static uint8_t role;
static struct bt_conn *default_conn;
static struct k_work_delayable iso_send_work;
static struct bt_iso_chan iso_chans[CONFIG_BT_ISO_MAX_CHAN];
static uint8_t cis_create_count = DEFAULT_CIS_COUNT;
static bool advertiser_found;
static bt_addr_le_t adv_addr;
static uint32_t last_received_counter;
static struct iso_recv_stats stats_current_conn;
static struct iso_recv_stats stats_overall;
static int64_t iso_conn_start_time;
static size_t total_iso_conn_count;
static uint32_t iso_send_count;

NET_BUF_POOL_FIXED_DEFINE(tx_pool, 1, CONFIG_BT_ISO_TX_MTU, NULL);
static uint8_t iso_data[CONFIG_BT_ISO_TX_MTU - BT_ISO_CHAN_SEND_RESERVE];

static K_SEM_DEFINE(sem_adv, 0, 1);
static K_SEM_DEFINE(sem_iso_accept, 0, 1);
static K_SEM_DEFINE(sem_iso_connected, 0, CONFIG_BT_ISO_MAX_CHAN);
static K_SEM_DEFINE(sem_iso_disconnected, 0, CONFIG_BT_ISO_MAX_CHAN);
static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_disconnected, 0, 1);

static struct bt_iso_chan_io_qos iso_tx_qos = {
	.interval = DEFAULT_CIS_INTERVAL_US, /* in microseconds */
	.latency = DEFAULT_CIS_LATENCY_MS, /* milliseconds */
	.sdu = DEFAULT_CIS_SDU, /* bytes */
	.rtn = DEFAULT_CIS_RTN,
	.phy = DEFAULT_CIS_PHY,
};

static struct bt_iso_chan_io_qos iso_rx_qos = {
	.interval = DEFAULT_CIS_INTERVAL_US, /* in microseconds */
	.latency = DEFAULT_CIS_LATENCY_MS, /* milliseconds */
	.sdu = DEFAULT_CIS_SDU, /* bytes */
	.rtn = DEFAULT_CIS_RTN,
	.phy = DEFAULT_CIS_PHY,
};

static struct bt_iso_chan_qos iso_qos = {
	.sca = BT_GAP_SCA_UNKNOWN,
	.packing = DEFAULT_CIS_PACKING,
	.framing = DEFAULT_CIS_FRAMING,
	.tx = &iso_tx_qos,
	.rx = &iso_rx_qos,
};

static uint8_t device_role_select(void)
{
	char role_char;
	const char central_char = 'c';
	const char peripheral_char = 'p';
	const char quit_char = 'q';

	while (true) {
		printk("Choose device role - type %c (central role) or %c (peripheral role), or %c to quit: ",
			central_char, peripheral_char, quit_char);

		role_char = console_getchar();

		printk("%c\n", role_char);

		if (role_char == central_char) {
			printk("Central role\n");
			return ROLE_CENTRAL;
		} else if (role_char == peripheral_char) {
			printk("Peripheral role\n");
			return ROLE_PERIPHERAL;
		} else if (role_char == quit_char) {
			printk("Quitting\n");
			return ROLE_QUIT;
		} else if (role_char == '\n' || role_char == '\r') {
			continue;
		}

		printk("Invalid role: %c\n", role_char);
	}
}

static void print_stats(char *name, struct iso_recv_stats *stats)
{
	uint32_t total_packets;

	total_packets = stats->iso_recv_count + stats->iso_lost_count;

	LOG_INF("%s: Received %u/%u (%.2f%%) - Total packets lost %u",
		name, stats->iso_recv_count, total_packets,
		(float)stats->iso_recv_count * 100 / total_packets,
		stats->iso_lost_count);

}

static void iso_timer_timeout(struct k_work *work)
{
	int ret;
	struct net_buf *buf;

	/* Reschedule as early as possible to reduce time skewing
	 * Use the ISO interval minus a few microseconds to keep the buffer
	 * full. This might occasionally skip a transmit, i.e. where the host
	 * calls `bt_iso_chan_send` but the controller only sending a single
	 * ISO packet.
	 */
	k_work_reschedule(&iso_send_work, K_USEC(iso_tx_qos.interval - 100));

	for (int i = 0; i < cis_create_count; i++) {
		buf = net_buf_alloc(&tx_pool, K_FOREVER);
		if (buf == NULL) {
			LOG_ERR("Could not allocate buffer");
			return;
		}

		net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
		net_buf_add_mem(buf, iso_data, iso_tx_qos.sdu);
		ret = bt_iso_chan_send(&iso_chans[i], buf);
		if (ret < 0) {
			LOG_ERR("Unable to send data: %d", ret);
			net_buf_unref(buf);
			break;
		}
		iso_send_count++;

		if ((iso_send_count % 100) == 0) {
			LOG_INF("Sending value %u", iso_send_count);
		}
	}
}

static void iso_recv(struct bt_iso_chan *chan,
		     const struct bt_iso_recv_info *info,
		     struct net_buf *buf)
{
	uint32_t total_packets;
	static bool stats_latest_arr[1000];
	static size_t stats_latest_arr_pos;

	/* NOTE: The packets received may be on different BISes */

	if (info->flags == BT_ISO_FLAGS_VALID) {
		stats_current_conn.iso_recv_count++;
		stats_overall.iso_recv_count++;
		stats_latest_arr[stats_latest_arr_pos++] = true;
	} else {
		stats_current_conn.iso_lost_count++;
		stats_overall.iso_lost_count++;
		stats_latest_arr[stats_latest_arr_pos++] = false;
	}

	if (stats_latest_arr_pos == sizeof(stats_latest_arr)) {
		stats_latest_arr_pos = 0;
	}

	total_packets = stats_overall.iso_recv_count + stats_overall.iso_lost_count;

	if ((total_packets % 100) == 0) {
		struct iso_recv_stats stats_latest = { 0 };

		for (int i = 0; i < ARRAY_SIZE(stats_latest_arr); i++) {
			if (i == total_packets) {
				break;
			}

			if (stats_latest_arr[i]) {
				stats_latest.iso_recv_count++;
			} else {
				stats_latest.iso_lost_count++;
			}
		}

		print_stats("Overall     ", &stats_overall);
		print_stats("Current Sync", &stats_current_conn);
		print_stats("Latest 1000 ", &stats_latest);
		LOG_INF(""); /* Empty line to separate the stats */
	}
}

static void iso_connected(struct bt_iso_chan *chan)
{
	LOG_INF("ISO Channel %p connected", chan);

	/* If multiple CIS was created, this will be the value of the last
	 * created in the CIG
	 */
	iso_conn_start_time = k_uptime_get();

	k_sem_give(&sem_iso_connected);
}

static void iso_disconnected(struct bt_iso_chan *chan, uint8_t reason)
{
	/* Calculate cumulative moving average - Be aware that this may
	 * cause overflow at sufficiently large counts or durations
	 *
	 * This duration is calculated for each CIS disconnected from the time
	 * of the last created CIS.
	 */
	static int64_t average_duration;
	uint64_t iso_conn_duration;
	uint64_t total_duration;

	if (iso_conn_start_time > 0) {
		iso_conn_duration = k_uptime_get() - iso_conn_start_time;
	} else {
		iso_conn_duration = 0;
	}
	total_duration = iso_conn_duration + (total_iso_conn_count - 1) * average_duration;

	average_duration = total_duration / total_iso_conn_count;

	LOG_INF("ISO Channel %p disconnected with reason 0x%02x after "
		"%llu milliseconds (average duration %llu)",
		chan, reason, iso_conn_duration, average_duration);

	k_sem_give(&sem_iso_disconnected);
}

static struct bt_iso_chan_ops iso_ops = {
	.recv		= iso_recv,
	.connected	= iso_connected,
	.disconnected	= iso_disconnected,
};

static int iso_accept(struct bt_conn *conn, struct bt_iso_chan **chan)
{
	LOG_INF("Incoming ISO request");

	for (int i = 0; i < ARRAY_SIZE(iso_chans); i++) {
		if (iso_chans[i].state == BT_ISO_DISCONNECTED) {
			LOG_INF("Returning instance %d", i);
			*chan = &iso_chans[i];
			cis_create_count++;

			k_sem_give(&sem_iso_accept);
			return 0;
		}
	}

	LOG_ERR("Could not accept any more CIS");

	*chan = NULL;

	return -ENOMEM;
}

static struct bt_iso_server iso_server = {
	.sec_level = DEFAULT_CIS_SEC_LEVEL,
	.accept = iso_accept,
};

static bool data_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;
	uint8_t len;

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		len = MIN(data->data_len, DEVICE_NAME_LEN - 1);
		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	default:
		return true;
	}
}

static int start_scan(void)
{
	int err;

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
	if (err != 0) {
		LOG_ERR("Scan start failed: %d", err);
		return err;
	}

	LOG_INF("Scan started");

	return 0;
}

static int stop_scan(void)
{
	int err;

	err = bt_le_scan_stop();
	if (err != 0) {
		LOG_ERR("Scan stop failed: %d", err);
		return err;
	}
	LOG_INF("Scan stopped");

	return 0;
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *buf)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char name[DEVICE_NAME_LEN];

	if (advertiser_found) {
		return;
	}

	(void)memset(name, 0, sizeof(name));

	bt_data_parse(buf, data_cb, name);

	if (strncmp(DEVICE_NAME, name, strlen(DEVICE_NAME))) {
		return;
	}

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	LOG_INF("Found peripheral with address %s (RSSI %i)",
		le_addr, info->rssi);


	bt_addr_le_copy(&adv_addr, info->addr);
	advertiser_found = true;
	k_sem_give(&sem_adv);
}

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0 && role == ROLE_CENTRAL) {
		LOG_INF("Failed to connect to %s: %d", addr, err);

		bt_conn_unref(default_conn);
		default_conn = NULL;
		return;
	} else if (role == ROLE_PERIPHERAL) {
		default_conn = conn;
	}

	LOG_INF("Connected: %s", addr);

	k_sem_give(&sem_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

	default_conn = NULL;

	k_sem_give(&sem_disconnected);
}

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

static size_t get_chars(char *buffer, size_t max_size)
{
	size_t pos = 0;

	while (pos < max_size) {
		char c = console_getchar();

		if (c == '\n' || c == '\r') {
			break;
		}
		printk("%c", c);
		buffer[pos++] = c;
	}
	printk("\n");
	buffer[pos] = '\0';

	return pos;
}

static int parse_rtn_arg(struct bt_iso_chan_io_qos *qos)
{
	char buffer[4];
	size_t char_count;
	uint64_t rtn;

	printk("Set RTN (current %u, default %u)\n",
	       qos->rtn, DEFAULT_CIS_RTN);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_RTN;
	}

	rtn = strtoul(buffer, NULL, 0);
	if (rtn > 16) {
		printk("Invalid RTN %llu", rtn);
		return -EINVAL;
	}

	return (int)rtn;
}

static int parse_interval_arg(struct bt_iso_chan_io_qos *qos)
{
	char buffer[9];
	size_t char_count;
	uint64_t interval;

	printk("Set interval (us) (current %u, default %u)\n",
	       qos->interval, DEFAULT_CIS_INTERVAL_US);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_INTERVAL_US;
	}

	interval = strtoul(buffer, NULL, 0);
	if (interval < 0x100 || interval > 0xFFFFF) {
		printk("Invalid interval %llu", interval);
		return -EINVAL;
	}

	return (int)interval;
}

static int parse_latency_arg(struct bt_iso_chan_io_qos *qos)
{
	char buffer[6];
	size_t char_count;
	uint64_t latency;

	printk("Set latency (ms) (current %u, default %u)\n",
	       qos->latency, DEFAULT_CIS_LATENCY_MS);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_LATENCY_MS;
	}

	latency = strtoul(buffer, NULL, 0);
	if (latency > 0xFA0) {
		printk("Invalid latency %llu", latency);
		return -EINVAL;
	}

	return (int)latency;
}

static int parse_phy_arg(struct bt_iso_chan_io_qos *qos)
{
	char buffer[3];
	size_t char_count;
	uint64_t phy;

	printk("Set PHY (current %u, default %u) - %u = 1M, %u = 2M, %u = Coded\n",
	       qos->phy, DEFAULT_CIS_PHY, BT_GAP_LE_PHY_1M,
	       BT_GAP_LE_PHY_2M, BT_GAP_LE_PHY_CODED);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_PHY;
	}

	phy = strtoul(buffer, NULL, 0);
	if (phy != BT_GAP_LE_PHY_1M &&
	    phy != BT_GAP_LE_PHY_2M &&
	    phy != BT_GAP_LE_PHY_CODED) {
		printk("Invalid PHY %llu", phy);
		return -EINVAL;
	}

	return (int)phy;
}

static int parse_sdu_arg(struct bt_iso_chan_io_qos *qos)
{
	char buffer[6];
	size_t char_count;
	uint64_t sdu;

	printk("Set SDU (current %u, default %u)\n",
	       qos->sdu, DEFAULT_CIS_SDU);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_SDU;
	}

	sdu = strtoul(buffer, NULL, 0);
	if (sdu > 0xFFF || sdu < sizeof(uint32_t) /* room for the counter */) {
		printk("Invalid SDU %llu", sdu);
		return -EINVAL;
	}

	return (int)sdu;
}

static int parse_cis_count_arg(void)
{
	char buffer[4];
	size_t char_count;
	uint64_t cis_count;

	printk("Set CIS count (current %u, default %u)\n",
	       cis_create_count, DEFAULT_CIS_COUNT);

	char_count = get_chars(buffer, sizeof(buffer) - 1);

	if (char_count == 0) {
		return DEFAULT_CIS_COUNT;
	}

	cis_count = strtoul(buffer, NULL, 0);
	if (cis_count > CONFIG_BT_ISO_MAX_CHAN) {
		printk("Invalid CIS count %llu", cis_count);
		return -EINVAL;
	}

	return (int)cis_count;
}

static int parse_args(struct bt_iso_chan_io_qos *qos)
{
	int rtn;
	int interval;
	int latency;
	int phy;
	int sdu;

	printk("Follow the prompts. Press enter to use default values.\n");

	rtn = parse_rtn_arg(qos);
	if (rtn < 0) {
		return -EINVAL;
	}

	interval = parse_interval_arg(qos);
	if (interval < 0) {
		return -EINVAL;
	}

	latency = parse_latency_arg(qos);
	if (latency < 0) {
		return -EINVAL;
	}

	phy = parse_phy_arg(qos);
	if (phy < 0) {
		return -EINVAL;
	}

	sdu = parse_sdu_arg(qos);
	if (sdu < 0) {
		return -EINVAL;
	}

	qos->rtn = rtn;
	qos->interval = interval;
	qos->latency = latency;
	qos->phy = phy;
	qos->sdu = sdu;

	return 0;
}

static int change_central_settings(void)
{
	char c;
	int err;

	printk("Change TX settings (y/N)? (Current settings: rtn=%u, "
	       "interval=%u, latency=%u, phy=%u, sdu=%u)\n",
	       iso_tx_qos.rtn, iso_tx_qos.interval, iso_tx_qos.latency,
	       iso_tx_qos.phy, iso_tx_qos.sdu);

	c = console_getchar();
	if (c == 'y' || c == 'Y') {
		printk("Disable TX (y/N)?\n");
		c = console_getchar();
		if (c == 'y' || c == 'Y') {
			iso_qos.tx = NULL;
			printk("TX disabled\n");
		} else {
			iso_qos.tx = &iso_tx_qos;

			err = parse_args(&iso_tx_qos);

			if (err != 0) {
				return err;
			}

			printk("New settings: rtn=%u, interval=%u, latency=%u, "
			"phy=%u, sdu=%u\n",
			iso_tx_qos.rtn, iso_tx_qos.interval, iso_tx_qos.latency,
			iso_tx_qos.phy, iso_tx_qos.sdu);
		}
	}

	printk("Change RX settings (y/N)? (Current settings: rtn=%u, "
	       "interval=%u, latency=%u, phy=%u, sdu=%u)\n",
	       iso_rx_qos.rtn, iso_rx_qos.interval, iso_rx_qos.latency,
	       iso_rx_qos.phy, iso_rx_qos.sdu);

	c = console_getchar();
	if (c == 'y' || c == 'Y') {
		printk("Disable RX (y/N)?\n");
		c = console_getchar();
		if (c == 'y' || c == 'Y') {
			if (iso_qos.tx == NULL) {
				LOG_ERR("Cannot disable both TX and RX\n");
				return -EINVAL;
			}

			iso_qos.rx = NULL;
			printk("RX disabled\n");
		} else {

			printk("Set RX settings to TX settings (Y/n)?\n");

			c = console_getchar();
			if (c == 'n' || c == 'N') {
				err = parse_args(&iso_rx_qos);

				if (err != 0) {
					return err;
				}

				printk("New settings: rtn=%u, interval=%u, "
				       "latency=%u, phy=%u, sdu=%u\n",
				       iso_rx_qos.rtn, iso_rx_qos.interval,
				       iso_rx_qos.latency, iso_rx_qos.phy,
				       iso_rx_qos.sdu);
			} else {
				(void)memcpy(&iso_rx_qos, &iso_tx_qos,
					     sizeof(iso_rx_qos));
			}
		}
	}

	printk("Change CIS count (y/N)? (Current: %u)\n", cis_create_count);

	c = console_getchar();
	if (c == 'y' || c == 'Y') {
		int cis_count = parse_cis_count_arg();

		if (cis_count < 0) {
			return -EINVAL;
		}

		cis_create_count = cis_count;
		printk("New CIS count: %u\n", cis_create_count);
	}

	return 0;
}

static int central_create_connection(void)
{
	int err;

	advertiser_found = false;

	err = start_scan();
	if (err != 0) {
		LOG_ERR("Could not start scan: %d", err);
		return err;
	}

	LOG_INF("Waiting for advertiser");
	err = k_sem_take(&sem_adv, K_FOREVER);
	if (err != 0) {
		LOG_ERR("failed to take sem_per_adv: %d", err);
		return err;
	}

	LOG_INF("Stopping scan");
	err = stop_scan();
	if (err != 0) {
		LOG_ERR("Could not stop scan: %d", err);
		return err;
	}

	LOG_INF("Connecting");
	err = bt_conn_le_create(&adv_addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &default_conn);
	if (err != 0) {
		LOG_ERR("Create connection failed: %d", err);
		return err;
	}

	err = k_sem_take(&sem_connected, K_FOREVER);
	if (err != 0) {
		LOG_ERR("failed to take sem_connected: %d", err);
		return err;
	}

	return 0;
}

static int central_create_cis(void)
{

	struct bt_conn *conn_pointers[CONFIG_BT_ISO_MAX_CHAN];
	struct bt_iso_chan *chan_pointers[CONFIG_BT_ISO_MAX_CHAN];
	int err;

	iso_conn_start_time = 0;

	for (int i = 0; i < cis_create_count; i++) {
		conn_pointers[i] = default_conn;
		chan_pointers[i] = &iso_chans[i];
	}

	LOG_INF("Binding ISO");
	err = bt_iso_chan_bind(conn_pointers, cis_create_count, chan_pointers);
	if (err != 0) {
		LOG_ERR("Failed to bind iso to connection: %d", err);
		return err;
	}

	LOG_INF("Connecting ISO channels");
	err = bt_iso_chan_connect(chan_pointers, cis_create_count);
	if (err != 0) {
		LOG_ERR("Failed to connect iso: %d", err);
		return err;
	}
	total_iso_conn_count++;

	for (int i = 0; i < cis_create_count; i++) {
		err = k_sem_take(&sem_iso_connected, K_FOREVER);
		if (err != 0) {
			LOG_ERR("failed to take sem_iso_connected: %d", err);
			return err;
		}
	}

	return 0;
}

static void reset_sems(void)
{
	(void)k_sem_reset(&sem_adv);
	(void)k_sem_reset(&sem_iso_accept);
	(void)k_sem_reset(&sem_iso_connected);
	(void)k_sem_reset(&sem_iso_disconnected);
	(void)k_sem_reset(&sem_connected);
	(void)k_sem_reset(&sem_disconnected);
}

static int test_run_central(void)
{
	int err;
	char c;

	iso_conn_start_time = 0;
	last_received_counter = 0;
	memset(&stats_current_conn, 0, sizeof(stats_current_conn));
	reset_sems();

	printk("Change ISO settings (y/N)?\n");
	c = console_getchar();
	if (c == 'y' || c == 'Y') {
		err = change_central_settings();
		if (err != 0) {
			LOG_ERR("Failed to set parameters: %d", err);
			return err;
		}
	}

	err = central_create_connection();
	if (err != 0) {
		LOG_ERR("Failed to create connection: %d", err);
		return err;
	}

	err = central_create_cis();
	if (err != 0) {
		LOG_ERR("Failed to create connection: %d", err);
		bt_conn_unref(default_conn);
		return err;
	}

	k_work_init_delayable(&iso_send_work, iso_timer_timeout);
	k_work_schedule(&iso_send_work, K_MSEC(0));

	err = k_sem_take(&sem_disconnected, K_FOREVER);
	if (err != 0) {
		LOG_ERR("failed to take sem_disconnected: %d", err);
		bt_conn_unref(default_conn);
		return err;
	}

	for (int i = 0; i < cis_create_count; i++) {
		err = k_sem_take(&sem_iso_disconnected, K_FOREVER);
		if (err != 0) {
			LOG_ERR("failed to take sem_iso_disconnected: %d", err);
			bt_conn_unref(default_conn);
			return err;
		}
	}

	LOG_INF("Disconnected - Cleaning up");
	bt_conn_unref(default_conn);
	k_work_cancel_delayable(&iso_send_work);

	return 0;
}

static int peripheral_cleanup(void)
{
	int err;

	err = k_sem_take(&sem_disconnected, K_NO_WAIT);

	if (err != 0) {
		err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		if (err != 0) {
			LOG_ERR("Could not disconnect ACL: %d", err);
			return err;
		}
	} /* else ACL already disconnected */

	for (int i = 0; i < cis_create_count; i++) {
		err = k_sem_take(&sem_iso_disconnected, K_NO_WAIT);
		if (err != 0) {
			err = bt_iso_chan_disconnect(&iso_chans[i]);
			if (err != 0) {
				LOG_ERR("Could not disconnect ISO[%d]: %d", i, err);
				break;
			}
		} /* else ISO already disconnected */
	}

	return err;
}

static int test_run_peripheral(void)
{
	int err;
	static bool initialized;

	/* Reset */
	cis_create_count = 0;
	iso_conn_start_time = 0;
	last_received_counter = 0;
	memset(&stats_current_conn, 0, sizeof(stats_current_conn));
	reset_sems();

	if (!initialized) {
		LOG_INF("Registering ISO server");
		err = bt_iso_server_register(&iso_server);
		if (err != 0) {
			LOG_ERR("ISO server register failed: %d", err);
			return err;
		}
		initialized = true;
	}

	LOG_INF("Starting advertising");
	err = bt_le_adv_start(ADV_PARAM, NULL, 0, NULL, 0);
	if (err != 0) {
		LOG_ERR("Advertising failed to start: %d", err);
		return err;
	}

	LOG_INF("Waiting for ACL connection");
	err = k_sem_take(&sem_connected, K_FOREVER);
	if (err != 0) {
		LOG_ERR("failed to take sem_connected: %d", err);
		return err;
	}

	LOG_INF("Waiting for ISO connection");

	err = k_sem_take(&sem_iso_accept, K_SECONDS(2));
	if (err != 0) {
		err = peripheral_cleanup();
		if (err != 0) {
			LOG_ERR("Could not clean up peripheral");
		}
		return err;
	}

	for (int i = 0; i < cis_create_count; i++) {
		err = k_sem_take(&sem_iso_connected, K_FOREVER);
		if (err != 0) {
			LOG_ERR("failed to take sem_iso_connected: %d", err);
			return err;
		}
	}
	total_iso_conn_count++;

	k_work_init_delayable(&iso_send_work, iso_timer_timeout);
	k_work_schedule(&iso_send_work, K_MSEC(0));

	/* Wait for disconnect */
	err = k_sem_take(&sem_disconnected, K_FOREVER);
	if (err != 0) {
		LOG_ERR("failed to take sem_disconnected: %d", err);
		return err;
	}

	for (int i = 0; i < cis_create_count; i++) {
		err = k_sem_take(&sem_iso_disconnected, K_FOREVER);
		if (err != 0) {
			LOG_ERR("failed to take sem_iso_disconnected: %d", err);
			return err;
		}
	}

	LOG_INF("Disconnected - Cleaning up");
	k_work_cancel_delayable(&iso_send_work);

	return 0;
}

void main(void)
{
	int err;
	static bool data_initialized;

	LOG_INF("Starting Bluetooth Throughput example");

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_INF("Bluetooth init failed: %d", err);
		return;
	}

	bt_conn_cb_register(&conn_callbacks);
	bt_le_scan_cb_register(&scan_callbacks);

	err = console_init();
	if (err != 0) {
		LOG_INF("Console init failed: %d", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	for (int i = 0; i < ARRAY_SIZE(iso_chans); i++) {
		iso_chans->ops = &iso_ops;
		iso_chans->qos = &iso_qos;
	}

	if (!data_initialized) {
		/* Init data */
		for (int i = 0; i < iso_tx_qos.sdu; i++) {
			if (i < sizeof(iso_send_count)) {
				continue;
			}
			iso_data[i] = (uint8_t)i;
		}

		data_initialized = true;
	}

	while (true) {
		role = device_role_select();

		if (role == ROLE_CENTRAL) {
			err = test_run_central();
		} else if (role == ROLE_PERIPHERAL) {
			err = test_run_peripheral();
		} else {
			if (role != ROLE_QUIT) {
				LOG_INF("Invalid role %u", role);
				continue;
			} else {
				err = 0;
				break;
			}
		}

		LOG_INF("Test complete: %d", err);
	}

	LOG_INF("Exiting");
}
