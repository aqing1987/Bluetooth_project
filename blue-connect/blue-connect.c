/*
  *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>
#include <getopt.h>
#include <signal.h>
#include <poll.h>
#include <sqlite3.h>
#include <pthread.h>

#include "lib/uuid.h"

#include <sys/ioctl.h>
#include <btio/btio.h>
#include <sys/time.h>

#include <attrib/att.h>
#include <attrib/gattrib.h>
#include <attrib/gatt.h>
#include <attrib/gatttool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

static GIOChannel *iochannel = NULL;
static GAttrib *attrib = NULL;
static GMainLoop *event_loop;

static gchar *opt_src = NULL;
static gchar *opt_dst = NULL;
static gchar *opt_dst_type = NULL;
static gchar *opt_sec_level = NULL;

static const int opt_psm = 0;
static int all, opt_mtu = 0;
static struct hci_dev_info di;

static bt_uuid_t *opt_uuid = NULL;
static int opt_start = 0x0001;
static int opt_end = 0xffff;
static int opt_handle = -1;
static gchar *opt_value = NULL;

static gchar *opt_hci_list = FALSE;
static gchar *opt_hci_reset = FALSE;
static gchar *opt_bt_scan = FALSE;
static gchar *opt_bt_lescan = FALSE;
static gchar *opt_bt_status = FALSE;
static gchar *opt_bt_help = FALSE;
static gchar *opt_interactive = FALSE;
static gboolean opt_write = FALSE;
static gboolean opt_read = FALSE;
static gboolean opt_listen = FALSE;
static gboolean got_error = FALSE;

static GSourceFunc operation;

#define for_each_opt(opt, long, short) while ((opt=getopt_long(argc, argv, short ? short:"+", long, NULL)) != -1)

#define LE_LINK		0x03
#define FLAGS_AD_TYPE 0x01
#define FLAGS_LIMITED_MODE_BIT 0x01
#define FLAGS_GENERAL_MODE_BIT 0x02
#define EIR_FLAGS                   0x01  /* flags */
#define EIR_UUID16_SOME             0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL              0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME             0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL              0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME            0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL             0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT              0x08  /* shortened local name */
#define EIR_NAME_COMPLETE           0x09  /* complete local name */
#define EIR_TX_POWER                0x0A  /* transmit power level */
#define EIR_APPERANCE		    0x19  /* Appearance*/
#define EIR_DEVICE_ID               0x10  /* device ID */
#define EIR_SLAVE_CONN_INTVAL	    0X12  /* Slave connectoin inteval*/
#define SIZE_1024B 1024

#define EIR_MANUFACTURE_SPECIFIC    0xFF

#define BLUETOOTH_DATABASE "devicelist.db"

sqlite3 *bluetooth_db;
typedef struct _le_devices
{
    char name[50];
    char address[50];
}le_devices;

struct characteristic_data {
	GAttrib *attrib;
	//uint16_t orig_start;
	uint16_t start;
	uint16_t end;
	bt_uuid_t uuid;
};
static volatile int signal_received = 0;
static void cmd_help(int parameter,int argc ,char **argvp);

static enum state {
	STATE_DISCONNECTED=0,
	STATE_CONNECTING=1,
	STATE_CONNECTED=2
} conn_state;

static const char 
  *tag_RESPONSE  = "respone",
  *tag_ERRCODE   = "code",
  *tag_HANDLE    = "handle",
  *tag_DATA      = "data",
  *tag_CONNSTATE = "state",
  *tag_SEC_LEVEL = "sec",
  *tag_MTU       = "mtu",
  *tag_DEVICE    = "dst";

static const char
  *rsp_ERROR     = "error",
  *rsp_STATUS    = "status",
  *rsp_NOTIFY    = "ntfy",
  *rsp_IND       = "ind",
  *rsp_WRITE     = "wr";

static const char
  *err_CONN_FAIL = "connect fail",
  *err_COMM_ERR  = "com error",
  *err_PROTO_ERR = "protocol error",
  *err_BAD_CMD   = "can not understand cmd",
  *err_BAD_PARAM = "do not understand parameter",
  *err_BAD_STATE = "badstate";

static const char 
  *st_DISCONNECTED = "disc",
  *st_CONNECTING   = "tryconn",
  *st_CONNECTED    = "conn";

struct cmd_option {

	const char *str ;
	int option_num ;

} cmd_option;

static void resp_begin(const char *rsptype)
{
  printf(" %s:%s", tag_RESPONSE, rsptype);
}

static void send_sym(const char *tag, const char *val)
{
  printf(" %s:%s", tag, val);
}

static void send_uint(const char *tag, unsigned int val)
{
  printf(" %s=h%X", tag, val);
}

static void send_str(const char *tag, const char *val)
{
  //!!FIXME
  printf(" %s='%s", tag, val);
}

static void send_data(const unsigned char *val, size_t len)
{
  printf(" %s=b", tag_DATA);
  while ( len-- > 0 )
    printf("%02X", *val++);
}

static void resp_end()
{
  printf("\n");
  fflush(stdout);
}

static void resp_error(const char *errcode)
{
  resp_begin(rsp_ERROR);
  printf("\n");
  send_sym(tag_ERRCODE, errcode);
  printf("\n");
  resp_end();
}

static void print_dev_hdr(struct hci_dev_info *di)
{
	static int hdr = -1;
	char addr[18];

	if (hdr == di->dev_id)
		return;
	hdr = di->dev_id;

	ba2str(&di->bdaddr, addr);

	printf("%s:\tType: %s  Bus: %s\n", di->name,
					hci_typetostr((di->type & 0x30) >> 4),
					hci_bustostr(di->type & 0x0f));
	printf("\tBD Address: %s  ACL MTU: %d:%d  SCO MTU: %d:%d\n",
					addr, di->acl_mtu, di->acl_pkts,
						di->sco_mtu, di->sco_pkts);
}

static void print_dev_info(int ctl, struct hci_dev_info *di)
{
	struct hci_dev_stats *st = &di->stat;
	char *str;

	print_dev_hdr(di);

	str = hci_dflagstostr(di->flags);
	printf("\t%s\n", str);
	bt_free(str);

	printf("\tRX bytes:%d acl:%d sco:%d events:%d errors:%d\n",
		st->byte_rx, st->acl_rx, st->sco_rx, st->evt_rx, st->err_rx);

	printf("\tTX bytes:%d acl:%d sco:%d commands:%d errors:%d\n",
		st->byte_tx, st->acl_tx, st->sco_tx, st->cmd_tx, st->err_tx);

	// if (all && !hci_test_bit(HCI_RAW, &di->flags)) {
	// 	print_dev_features(di, 0);

	// 	if (((di->type & 0x30) >> 4) == HCI_BREDR) {
	// 		print_pkt_type(di);
	// 		print_link_policy(di);
	// 		print_link_mode(di);

	// 		if (hci_test_bit(HCI_UP, &di->flags)) {
	// 			cmd_name(ctl, di->dev_id, NULL);
	// 			cmd_class(ctl, di->dev_id, NULL);
	// 		}
	// 	}

	// 	if (hci_test_bit(HCI_UP, &di->flags))
	// 		cmd_version(ctl, di->dev_id, NULL);
	// }

	printf("\n");
}

static void cmd_print_dev_list(int dev_id, int argc,char **argv)
{
	struct hci_dev_list_req *dl;
	struct hci_dev_req *dr;
	int i, ctl;

	if ((ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
		perror("Can't open HCI socket.");
		exit(1);
	}

	if (!(dl = malloc(HCI_MAX_DEV * sizeof(struct hci_dev_req) +
		sizeof(uint16_t)))) {
		perror("Can't allocate memory");
		exit(1);
	}
	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(ctl, HCIGETDEVLIST, (void *) dl) < 0) {
		perror("Can't get device list");
		free(dl);
		exit(1);
	}

	for (i = 0; i< dl->dev_num; i++) {
		di.dev_id = (dr+i)->dev_id;
		if (ioctl(ctl, HCIGETDEVINFO, (void *) &di) < 0)
			continue;
		print_dev_info(ctl, &di);
	}

	free(dl);
}

static void cmd_up(int ctl, int hdev, char *opt)
{
	/* Start HCI device */
	if (ioctl(ctl, HCIDEVUP, hdev) < 0) {
		if (errno == EALREADY)
			return;
		fprintf(stderr, "Can't init device hci%d: %s (%d)\n",
						hdev, strerror(errno), errno);
		exit(1);
	}
}
static void cmd_down(int ctl, int hdev, char *opt)
{
	/* Stop HCI device */
	printf("hdev: %d \n",hdev);
	if (ioctl(ctl, HCIDEVDOWN, hdev) < 0) {
		fprintf(stderr, "Can't down device hci%d: %s (%d)\n",
						hdev, strerror(errno), errno);
	printf("hdev: %d \n",hdev);	
		exit(1);
	}
}
static void cmd_hci_reset(int dev_id, int argc, char **argv)
{
	printf("reset hci \n");
	fflush(stdout);
	int ctl;
	printf("continue !\n");
	fflush(stdout);
	if ((ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) 
	{
		perror("Can't open HCI socket.");
		exit(1);
	}
	printf("di.dev_id \n");
	di.dev_id = atoi(argv);//atoi(argv[2] + 3);
	//printf("argv[0] + 3 = %s \n",(argv[2] + 3));
	printf("di.dev_id = %d \n",di.dev_id);

	argc--;
	argv++;
	if (ioctl(ctl, HCIGETDEVINFO, (void *) &di)) {
		perror("Can't get device info");
		exit(1);
	}

	if (hci_test_bit(HCI_RAW, &di.flags) &&
			!bacmp(&di.bdaddr, BDADDR_ANY)) {
		int dd = hci_open_dev(di.dev_id);
		hci_read_bd_addr(dd, &di.bdaddr, 1000);
		hci_close_dev(dd);
	}
	/* Reset HCI device */
#if 0
#endif
	cmd_down(ctl, di.dev_id, "down");
	cmd_up(ctl, di.dev_id, "up");
	printf("Reseted..!\n");
}

static void cmd_status(int parameter, int argcp, char **argvp)
{
  resp_begin(rsp_STATUS);
  switch(conn_state)
  {
    case STATE_CONNECTING:
      send_sym(tag_CONNSTATE, st_CONNECTING);
      send_str(tag_DEVICE, opt_dst);
      break;

    case STATE_CONNECTED:
      send_sym(tag_CONNSTATE, st_CONNECTED);
      send_str(tag_DEVICE, opt_dst);
      break;

    default:
      send_sym(tag_CONNSTATE, st_DISCONNECTED);
      break;
  }

  send_uint(tag_MTU, opt_mtu);
  send_str(tag_SEC_LEVEL, opt_sec_level);
  resp_end();
}

static void set_state(enum state st)
{
	conn_state = st;
        cmd_status(0,0, NULL);
}

static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t evt;
	uint16_t handle, olen;
	size_t plen;

	evt = pdu[0];

	if ( evt != ATT_OP_HANDLE_NOTIFY && evt != ATT_OP_HANDLE_IND )
	{
		printf("#Invalid opcode %02X in event handler??\n", evt);
		return;
	}

	assert( len >= 3 );
	handle = att_get_u16(&pdu[1]);

	resp_begin( evt==ATT_OP_HANDLE_NOTIFY ? rsp_NOTIFY : rsp_IND );
	send_uint( tag_HANDLE, handle );
	send_data( pdu+3, len-3 );
	resp_end();

	if (evt == ATT_OP_HANDLE_NOTIFY)
		return;

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_info_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, ending_handle , olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);
	ending_handle = att_get_u16(&pdu[3]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, ending_handle, att_type , olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);
	ending_handle = att_get_u16(&pdu[3]);
	att_type = att_get_u16(&pdu[5]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, ending_handle, att_type, olen;
	size_t plen;

	assert( len == 7 || len == 21 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);
	ending_handle = att_get_u16(&pdu[3]);
	if (len == 7) {
		att_type = att_get_u16(&pdu[5]);
	}

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len == 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_blob_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, offset, olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
	offset = att_get_u16(&pdu[3]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_multi_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle1, handle2, olen; //offset;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle1 = att_get_u16(&pdu[1]);
	handle2 = att_get_u16(&pdu[3]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle1, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_group_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, ending_handle, att_group_type, olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);
	ending_handle = att_get_u16(&pdu[3]);
	att_group_type = att_get_u16(&pdu[5]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len >= 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t opcode;
	uint16_t handle;

	assert( len >= 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
}

static void gatts_signed_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t opcode;
	uint16_t handle;

	assert( len >= 15 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
}

static void gatts_prep_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, offset, olen;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
	offset = att_get_u16(&pdu[3]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_exec_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode, flags;
	uint16_t olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	flags = pdu[1];

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, 0, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
	if (err) {
		set_state(STATE_DISCONNECTED);
		resp_error(err_CONN_FAIL);
		printf("# Connect error: %s\n", err->message);
		return;
	}

	attrib = g_attrib_new(io);
	g_attrib_register(attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_INFO_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_find_info_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_find_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BLOB_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_blob_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_MULTI_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_multi_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_GROUP_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_by_group_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_CMD, GATTRIB_ALL_HANDLES,
	                  gatts_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_SIGNED_WRITE_CMD, GATTRIB_ALL_HANDLES,
	                  gatts_signed_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_PREP_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_prep_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_EXEC_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_exec_write_req, attrib, NULL);

	set_state(STATE_CONNECTED);
	operation(attrib);
}

static void disconnect_io()
{
	if (conn_state == STATE_DISCONNECTED)
		return;

	g_attrib_unref(attrib);
	attrib = NULL;
	opt_mtu = 0;

	g_io_channel_shutdown(iochannel, FALSE, NULL);
	g_io_channel_unref(iochannel);
	iochannel = NULL;

	set_state(STATE_DISCONNECTED);
}
static int read_flags(uint8_t *flags, const uint8_t *data, size_t size)
{
	size_t offset;

	if (!flags || !data)
		return -EINVAL;

	offset = 0;
	while (offset < size) {
		uint8_t len = data[offset];
		uint8_t type;

		/* Check if it is the end of the significant part */
		if (len == 0)
			break;

		if (len + offset > size)
			break;

		type = data[offset + 1];

		if (type == FLAGS_AD_TYPE) {
			*flags = data[offset + 2];
			return 0;
		}

		offset += 1 + len;
	}

	return -ENOENT;
}
static int check_report_filter(uint8_t procedure, le_advertising_info *info)
{
	uint8_t flags;

	/* If no discovery procedure is set, all reports are treat as valid */
	if (procedure == 0)
		return 1;

	/* Read flags AD type value from the advertising report if it exists */
	if (read_flags(&flags, info->data, info->length))
		return 0;

	switch (procedure) {
	case 'l': /* Limited Discovery Procedure */
		if (flags & FLAGS_LIMITED_MODE_BIT)
			return 1;
		break;
	case 'g': /* General Discovery Procedure */
		if (flags & (FLAGS_LIMITED_MODE_BIT | FLAGS_GENERAL_MODE_BIT))
			return 1;
		break;
	default:
		fprintf(stderr, "Unknown discovery procedure\n");
	}

	return 0;
}

static int open_database(char *Database_Location)
{
    char *zErrMsg = 0;
    int  rc;
    char *sql;
    /* Open database */
    rc = sqlite3_open(Database_Location, &bluetooth_db);
    if( rc )
    {
        printf("Can't open database: %s\n", sqlite3_errmsg(bluetooth_db));
        return -1;
    }
    else
    {
        printf("Opened database successfully\n");
    }

    return rc; 
}

static void database_actions(const char* format, ...)
{
    char sql[SIZE_1024B*3];
    memset(sql, 0x00, sizeof(sql));
    char *zErrMsg = 0;

    va_list argptr;
    va_start(argptr, format);
    vsprintf(sql, format, argptr);
    va_end(argptr);

    int rc = sqlite3_exec(bluetooth_db, sql, NULL, 0, &zErrMsg);
    if( rc != SQLITE_OK )
    {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    else
    {
        printf("%s SUCCESS\n", sql);
    }
}

static void insert_database(const char *query, char* data0, char* data1, char* data2)
{
    char *zErrMsg = 0;
    char sql[512];
    memset(sql, 0x00, sizeof(sql));
    sprintf(sql, "%s%s%s%s%s%s%s%s%s", "INSERT INTO ", query, " VALUES ('", data0, "', '", data1, "', '", data2, "');");

    int rc = sqlite3_exec(bluetooth_db, sql, NULL, 0, &zErrMsg);
    if( rc != SQLITE_OK )
    {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    else
    {
        printf("Records BLUETOOTH_DEVICES created successfully\n");
    }

}

static int db_callback(void *data, int argc, char **argv, char **azColName){
   int i;
   for(i=0; i<argc; i++){
      printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
      if(data != NULL) strcpy(data, argv[i]);
   }
   printf("\n");
   return 0;
}

static void searching_database(char* result, const char* format, ...)
{
    char *zErrMsg = 0;
    char sql[512];
    memset(sql, 0x00, sizeof(sql));

    va_list argptr;
    va_start(argptr, format);
    vsprintf(sql, format, argptr);
    va_end(argptr);

    int rc = sqlite3_exec(bluetooth_db, sql, db_callback, (void*)result, &zErrMsg);
    if( rc != SQLITE_OK )
    {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    else
    {
        printf("%s SUCCESS\n", sql);
    }
}

static void update_database(const char* query0, char* data, const char* query1, char* identify)
{
    char *zErrMsg = 0;
    char sql[512];
    memset(sql, 0x00, sizeof(sql));
    sprintf(sql, "%s%s%s%s%s%s%s%s", query0, "'", data, "' ", query1, "'", identify, "';");
    //LOG(LOG_DBG, "update_database with query: %s\n", sql);
    int rc = sqlite3_exec(bluetooth_db, sql, NULL, 0, &zErrMsg);
    if( rc != SQLITE_OK )
    {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    else
    {
        printf("update_database BLUETOOTH_DATABASE successfully\n");
    }
}

static void delete_database(const char* table_name, const char* identify, char* identify_data)
{
    char *zErrMsg = 0;
    char sql[512];
    memset(sql, 0x00, sizeof(sql));
    sprintf(sql, "%s%s%s%s%s%s%s", "DELETE from ", table_name, " where ", identify, "='", identify_data, "';");

    int rc = sqlite3_exec(bluetooth_db, sql, NULL, 0, &zErrMsg);
    if( rc != SQLITE_OK )
    {
        printf("SQL error: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    else
    {
        printf("delete_database BLUETOOTH_DATABASE successfully\n");
    }
}

// static int listdevice_callback(void *devicelist, int argc, char **argv, char **azColName)
// {
//     int i;
//     json_object * jobj = json_object_new_object();

//     for(i=0; i<argc; i++)
//     {
//         json_object_object_add(jobj, azColName[i], json_object_new_string(argv[i] ? argv[i] : "NULL"));
//     }
//     json_object_array_add(devicelist, jobj);
//     return 0;
// }


static void sigint_handler(int sig)
{
	signal_received = sig;
}
static void eir_parse_name(uint8_t *eir, size_t eir_len,
						char *buf, size_t buf_len)
{
	size_t offset;

	offset = 0;
	while (offset < eir_len) {
		uint8_t field_len = eir[0];
		size_t name_len;

		/* Check for the end of EIR */
		if (field_len == 0)
			break;

		if (offset + field_len > eir_len)
			goto failed;

		switch (eir[1]) {
		case EIR_NAME_SHORT:
		case EIR_NAME_COMPLETE:
			name_len = field_len - 1;
			if (name_len > buf_len)
				goto failed;

			memcpy(buf, &eir[2], name_len);
			return;
		}

		offset += field_len + 1;
		eir += field_len + 1;
	}

failed:
	snprintf(buf, buf_len, "(unknown)");
}
#ifndef HEXDUMP_COLS
#define HEXDUMP_COLS 16
#endif
void hexdump(void *mem, unsigned int len)
{
   // if (gLogLevel<3) return;
    unsigned int i, j;
    for(i = 0; i < len + ((len % HEXDUMP_COLS) ? (HEXDUMP_COLS - len % HEXDUMP_COLS) : 0); i++)
    {
        /* print offset */
        // if(i % HEXDUMP_COLS == 0)
        // {
        //     printf("0x%06x: ", i);
        // }
        /* print hex data */
        if(i < len)
        {
            printf("%02x ", 0xFF & ((char*)mem)[i]);
        }
        else /* end of block, just aligning for ASCII dump */
        {
            printf("   ");
        }

        /* print ASCII dump */
        // if(i % HEXDUMP_COLS == (HEXDUMP_COLS - 1))
        // {
        //     for(j = i - (HEXDUMP_COLS - 1); j <= i; j++)
        //     {
        //         if(j >= len) /* end of block, not really printing */
        //         {
        //             putchar(' ');
        //         }
        //         else if(isprint(((char*)mem)[j])) /* printable char */
        //         {
        //             putchar(0xFF & ((char*)mem)[j]);
        //         }
        //         else /* other char */
        //         {
        //             putchar('.');
        //         }
        //     }
           // putchar('\n');
        // }
    }
}

void process_data(uint8_t *data, size_t data_len, le_advertising_info *info)
{
  // printf("Test: %p and %d\n", data, data_len);
  if(data[0] == EIR_NAME_SHORT || data[0] == EIR_NAME_COMPLETE)
  {
    size_t name_len = data_len - 1;
    char *name = malloc(name_len + 1);
    memset(name, 0, name_len + 1);
    memcpy(name, &data[2], name_len);

    char addr[18];
    ba2str(&info->bdaddr, addr);

    printf("MAC = %s \nNAME = %s\n", addr, name);
    printf("type: %02X len: %02X \n",data[0],data_len);
    int i;
    for(i=1; i<data_len; i++)
    {
      printf("\tData: 0x%0X\n", data[i]);
    }

    free(name);
  }
  else if(data[0] == EIR_FLAGS)
  {
    printf("Flag type: len=%02X\n", data_len);
    int i;
    for(i=1; i<data_len; i++)
    {
      printf("\tFlag data: 0x%0X\n", data[i]);
    }
  }
  else if(data[0] == EIR_UUID16_SOME || data[0] ==  EIR_UUID16_ALL
	|| data[0] == EIR_UUID32_SOME || data[0] == EIR_TX_POWER
	|| data[0] == EIR_APPERANCE || data[0] == EIR_SLAVE_CONN_INTVAL)
  {
    //printf("Manufacture specific type: len=%d\n", data_len);
	printf("type: %02X len: %02X \n",data[0],data_len);
    // TODO int company_id = data[current_index + 2] 

    int i;
    for(i=1; i<data_len; i++)
    {
      printf("\tData: 0x%0X\n", data[i]);
    }
  }
  else
  {
    printf("Unknown type: type=%X\n", data[0]);
  }
}
#define BELKIN 0x005C

 void check_configure(char * str_devices_type, char * str_devices_status)
{
	printf("check configure \n");
	int devices_type;
	int devices_status;

	devices_type = strtohandle(str_devices_type);
	devices_status = strtohandle(str_devices_type);
	if(devices_type == 0x5C)
	{
		printf("manufacture: BELKIN \n");
	}
	switch(devices_status)
	{
		case 0x00:
			{
				printf("unconfigure \n");
			break;
			}
		case 0x001:
			{
				printf("configured \n");
				break;
			}
		default:
			{
				printf("revesed");
				break;
			}

	}
}
static int print_advertising_devices(int dd, uint8_t filter_type)
{
	unsigned char buf[HCI_MAX_EVENT_SIZE], *ptr;
	struct hci_filter nf, of;
	struct sigaction sa;
	socklen_t olen;
	int len,to=5000;

	event_loop = g_main_loop_new(NULL,FALSE);

	olen = sizeof(of);
	if (getsockopt(dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
		printf("Could not get socket options\n");
		return -1;
	}

	hci_filter_clear(&nf);
	hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
	hci_filter_set_event(EVT_LE_META_EVENT, &nf);

	if (setsockopt(dd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
		printf("Could not set socket options\n");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);
	
	while(1)
	{
		evt_le_meta_event *meta;
		le_advertising_info *info;
		char addr[18];
		char evt_type[18];

		uint8_t str_data[512];

		 if (to) {
              struct pollfd p;
             int n;
 
             p.fd = dd; p.events = POLLIN;
             while ((n = poll(&p, 1, to)) < 0) {
                 if (errno == EAGAIN || errno == EINTR)
                     continue;
                 goto done;
             }
 
             if (!n) {
                 errno = ETIMEDOUT;
                 goto done;
             }

             to -= 10;
             if (to < 0)
                 to = 0;
         } 

	while ((len = read(dd, buf, sizeof(buf))) < 0)
		{
			if (errno == EINTR && signal_received == SIGINT) {
				len = 0;
				goto done;
			}
			if (errno == EAGAIN || errno == EINTR)
				continue;
			goto done;

		}

		ptr = buf + (1 + HCI_EVENT_HDR_SIZE);
		len -= (1 + HCI_EVENT_HDR_SIZE);

		meta = (void *) ptr;

		if (meta->subevent != 0x02)
			goto done;

		/* Ignoring multiple reports */
		info = (le_advertising_info *) (meta->data + 1);
	      if(info->length == 0)
	      {
	        continue;
	      }

	      int current_index = 0;
	      int data_error = 0;

	      while(!data_error && current_index < info->length)
	      {
	        size_t data_len = info->data[current_index];

	        if(data_len + 1 > info->length)
	        {
	          printf("EIR data length is longer than EIR packet length. %d + 1 > %d", data_len, info->length);
	          data_error = 1;
	        }
	        else
	        {
	          process_data(info->data + current_index + 1, data_len, info);
	          //get_rssi(&info->bdaddr, current_hci_state);
	          current_index += data_len + 1;
	        }
	      }
		printf("+++++++++++++++++++++\n");
//	printf("evt_type: 0x%02x\n", info->evt_type);
//	printf("bdaddr_type: 0x%02x\n", info->bdaddr_type);
//	printf("Length : %02X \n", info->length);
//	if (check_report_filter(filter_type, info)) {
//		char name[30];
//		int counter_tmp;
//
//		memset(name, 0, sizeof(name));
//		ba2str(&info->bdaddr, addr);
//		eir_parse_name(info->data, info->length,
//						name, sizeof(name) - 1);
//
//		printf("mac : %s \nname : %s\n", addr, name);
//		hexdump(info->data,info->length);
//		printf("\n \n");
//
//	 	//check_configure(info->data[9],info->data[10]);
//	 }
	};

done:
	setsockopt(dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));

	if (len < 0)
		return -1;

	return 0;
}
static void cmd_scan(int dev_id,int argc ,char **argvp)
{
	inquiry_info *ii = NULL;
    int max_rsp, num_rsp;
    int  i, dd, len, flags;

    char addr[19] = { 0 };
    char name[248] = { 0 };

    dev_id = hci_get_route(NULL);
    dd = hci_open_dev( dev_id );
    if (dev_id < 0 || dd < 0)
    {
        perror("opening socket");
        exit(1);
    }

    len  = 8;
    max_rsp = 255;
    flags = IREQ_CACHE_FLUSH;
    ii = (inquiry_info*)malloc(max_rsp * sizeof(inquiry_info));

    printf("Scanning ...\n");
    num_rsp = hci_inquiry(dev_id, len, max_rsp, NULL, &ii, flags);

    if( num_rsp < 0 ) perror("hci_inquiry");

    for (i = 0; i < num_rsp; i++) 
    {
        ba2str(&(ii+i)->bdaddr, addr);
        memset(name, 0, sizeof(name));
        if (hci_read_remote_name(dd, &(ii+i)->bdaddr, sizeof(name),
            name, 0) < 0)
        strcpy(name, "[unknown]");
        printf("%s  %s\n", addr, name);
    }

    free( ii );
    hci_close_dev( dd );
    //return 0;
}

static const char *lescan_help =
	"Usage:\n"
	"\tlescan [--privacy] enable privacy\n"
	"\tlescan [--passive] set scan type passive (default active)\n"
	"\tlescan [--whitelist] scan for address in the whitelist only\n"
	"\tlescan [--discovery=g|l] enable general or limited discovery"
		"procedure\n"
	"\tlescan [--duplicates] don't filter duplicates\n";

static struct option lescan_options[] = {
	{ "help",	0, 0, 'h' },
	{ "privacy",	0, 0, 'p' },
	{ "passive",	0, 0, 'P' },
	{ "whitelist",	0, 0, 'w' },
	{ "discovery",	1, 0, 'd' },
	{ "duplicates",	0, 0, 'D' },
	{ 0, 0, 0, 0 }
};
static void helper_arg(int min_num_arg, int max_num_arg, int *argc,
			char ***argv, const char *usage)
{
	*argc -= optind;
	/* too many arguments, but when "max_num_arg < min_num_arg" then no
		 limiting (prefer "max_num_arg=-1" to gen infinity)
	*/
	if ( (*argc > max_num_arg) && (max_num_arg >= min_num_arg ) ) 
	{
		fprintf(stderr, "%s: too many arguments (maximal: %i)\n",
				*argv[1], max_num_arg);
		printf("%s", usage);
		exit(1);
	}

	/* print usage */
	if (*argc < min_num_arg) 
	{
		fprintf(stderr, "%s: too few arguments (minimal: %i)\n",
				*argv[1], min_num_arg);
		printf("%s", usage);
		exit(0);
	}

	*argv += optind;
}

static void * lescan_bt_devices(int dev_id, int argc, char **argv)
{
	int err,opt, dd;
	uint8_t own_type = 0x00;
	uint8_t scan_type = 0x01;
	uint8_t filter_type = 0;
	uint8_t filter_policy = 0x00;
	uint16_t interval = htobs(0x0010);
	uint16_t window = htobs(0x0010);
	uint8_t filter_dup = 1;
	// printf("start lescan \n");
	for_each_opt(opt, lescan_options, NULL) {
		switch (opt) {
		case 'p':
			own_type = 0x01; /* Random */
			break;
		case 'P':
			scan_type = 0x00; /* Passive */
			break;
		case 'w':
			filter_policy = 0x01; /* Whitelist */
			break;
		case 'd':
			filter_type = optarg[0];
			if (filter_type != 'g' && filter_type != 'l') {
				fprintf(stderr, "Unknown discovery procedure\n");
				exit(1);
			}

			interval = htobs(0x0012);
			window = htobs(0x0012);
			break;
		case 'D':
			filter_dup = 0x00;
			break;
		default:
			printf("%s", lescan_help);
			return;
		}
	}

	helper_arg(0, 1, &argc, &argv, lescan_help);

	dev_id = hci_get_route(NULL);
    dd = hci_open_dev( dev_id );
    if (dev_id < 0 || dd < 0)
    {
        perror("opening socket");
        exit(1);
    }
	err = hci_le_set_scan_parameters(dd, scan_type, interval, window,
						own_type, filter_policy, 1000);
	if (err < 0) {
		perror("Set scan parameters failed");
		exit(1);
	}

	err = hci_le_set_scan_enable(dd, 0x01, filter_dup, 1000);
	if (err < 0) {
		perror("Enable scan failed");
		exit(1);
	}

	printf("LE Scan ...\n");

	err = print_advertising_devices(dd, filter_type);

	if (err < 0) {
		perror("Could not receive advertising events");
		exit(1);
	}

	err = hci_le_set_scan_enable(dd, 0x00, filter_dup, 1000);

	if (err < 0) {
		perror("Disable scan failed");
		exit(1);
	}
	printf("LE Scan finish ! \n");
	hci_close_dev(dd);
	
}

static void cmd_lescan (int dev_id,int argc ,char **argvp)
{
	lescan_bt_devices(dev_id,argc,argvp);
}
static void cmd_exit(int parameter, int argcp, char **argvp)
{
	g_main_loop_quit(event_loop);
}

static gboolean channel_watcher(GIOChannel *chan, GIOCondition cond,
				gpointer user_data)
{
	disconnect_io();

	return FALSE;
}

static void cmd_connect(int parameter ,int argcp, char **argvp)
{
	if (conn_state != STATE_DISCONNECTED)
		return;

	if (argcp > 1) {
		g_free(opt_dst);
		opt_dst = g_strdup(argvp[1]);
		printf("opt_dst: %s \n",argvp[2]);

		g_free(opt_dst_type);
		if (argcp > 2)
			opt_dst_type = g_strdup(argvp[3]);
		else
			opt_dst_type = g_strdup("public");
	}

	if (opt_dst == NULL) {
		error("Remote Bluetooth address required\n");
		resp_error(err_BAD_PARAM);
		return;
	}

	set_state(STATE_CONNECTING);
	iochannel = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
						opt_psm, opt_mtu, connect_cb);


	if (iochannel == NULL)
		set_state(STATE_DISCONNECTED);

	else
		g_io_add_watch(iochannel, G_IO_HUP, channel_watcher, NULL);
}

static void cmd_disconnect(int argcp, char **argvp)
{
	disconnect_io();
}

 int strtohandle(const char *src)
{
	char *e;
	int dst;

	errno = 0;
	dst = strtoll(src, &e, 16);
	if (errno != 0 || *e != '\0')
		return -EINVAL;

	return dst;
}

static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		goto done;
	}

	if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen)) {
		resp_error(err_PROTO_ERR);
		goto done;
	}

        resp_begin(rsp_WRITE);
        resp_end();
done:
	if (opt_listen == FALSE)
		g_main_loop_quit(event_loop);
}

// static void cmd_char_write_common(int argcp, char **argvp, int with_response)
// {
// 	uint8_t *value;
// 	size_t plen;
// 	int handle;

// 	if (conn_state != STATE_CONNECTED) {
// 		resp_error(err_BAD_STATE);
// 		return;
// 	}

// 	// if (argcp < 3) {
// 	// 	resp_error(err_BAD_PARAM);
// 	// 	return;
// 	// }

// 	handle = strtohandle(argvp[5]);
// 	if (handle <= 0) {
// 		resp_error(err_BAD_PARAM);
// 		return;
// 	}

// 	plen = gatt_attr_data_from_string(argvp[7], &value);
// 	if (plen == 0) {
// 		resp_error(err_BAD_PARAM);
// 		return;
// 	}

// 	if (with_response)
// 		gatt_write_char(attrib, handle, value, plen,
// 					char_write_req_cb, NULL);
// 	else
//         {
// 		gatt_write_char(attrib, handle, value, plen, NULL, NULL);
//                 resp_begin(rsp_WRITE);
//                 resp_end();
//         }

// 	g_free(value);
// }
static gboolean cmd_char_write_common(gpointer user_data)
{
	GAttrib *attrib = user_data;
	uint8_t *value;
	size_t len;

	if (opt_handle <= 0) {
		g_printerr("A valid handle is required\n");
		goto error;
	}

	if (opt_value == NULL || opt_value[0] == '\0') {
		g_printerr("A value is required\n");
		goto error;
	}

	len = gatt_attr_data_from_string(opt_value, &value);
	if (len == 0) {
		g_printerr("Invalid value\n");
		goto error;
	}

	gatt_write_char(attrib, opt_handle, value, len, char_write_req_cb,
									NULL);
	return FALSE ;

error:
	g_main_loop_quit(event_loop);
	return FALSE ;
}

static void cmd_char_write(int parameter ,int argcp, char **argvp)
{
  operation = cmd_char_write_common ;
}

static void char_read_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	uint8_t value[plen];
	ssize_t vlen;
	int i;

	if (status != 0) {
		g_printerr("Characteristic value/descriptor read failed: %s\n",
							att_ecode2str(status));
		goto done;
	}

	vlen = dec_read_resp(pdu, plen, value, sizeof(value));
	if (vlen < 0) {
		g_printerr("Protocol error\n");
		goto done;
	}
	g_print("Characteristic value/descriptor: ");
	for (i = 0; i < vlen; i++)
		g_print("%02x ", value[i]);
	g_print("\n");

done:
	if (!opt_listen)
		g_main_loop_quit(event_loop);
}

static void char_read_by_uuid_cb(guint8 status, const guint8 *pdu,
					guint16 plen, gpointer user_data)
{
	struct att_data_list *list;
	int i;

	if (status != 0) {
		g_printerr("Read characteristics by UUID failed: %s\n",
							att_ecode2str(status));
		goto done;
	}

	list = dec_read_by_type_resp(pdu, plen);
	if (list == NULL)
		goto done;

	for (i = 0; i < list->num; i++) {
		uint8_t *value = list->data[i];
		int j;

		g_print("handle: 0x%04x \t value: ", att_get_u16(value));
		value += 2;
		for (j = 0; j < list->len - 2; j++, value++)
			g_print("%02x ", *value);
		g_print("\n");
	}

	att_data_list_free(list);

done:
	g_main_loop_quit(event_loop);
}

static gboolean cmd_char_read_common(gpointer user_data)
{
	GAttrib *attrib = user_data;

	if (opt_uuid != NULL) {
		struct characteristic_data *char_data;

		char_data = g_new(struct characteristic_data, 1);
		char_data->attrib = attrib;
		char_data->start = opt_start;
		char_data->end = opt_end;

		gatt_read_char_by_uuid(attrib, opt_start, opt_end, opt_uuid,
						char_read_by_uuid_cb, char_data);

		return FALSE;
	}

	if (opt_handle <= 0) {
		g_printerr("A valid handle is required\n");
		g_main_loop_quit(event_loop);
		return FALSE;
	}

	gatt_read_char(attrib, opt_handle, char_read_cb, attrib);

	return FALSE;
}

static void cmd_char_read(int parameter ,int argcp, char **argvp)
{
  operation = cmd_char_read_common ;
}
static void cmd_sec_level(int argcp, char **argvp)
{
	GError *gerr = NULL;
	BtIOSecLevel sec_level;

	if (argcp < 2) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (strcasecmp(argvp[1], "medium") == 0)
		sec_level = BT_IO_SEC_MEDIUM;
	else if (strcasecmp(argvp[1], "high") == 0)
		sec_level = BT_IO_SEC_HIGH;
	else if (strcasecmp(argvp[1], "low") == 0)
		sec_level = BT_IO_SEC_LOW;
	else {
		resp_error(err_BAD_PARAM);
		return;
	}

	g_free(opt_sec_level);
	opt_sec_level = g_strdup(argvp[1]);

	if (conn_state != STATE_CONNECTED)
		return;

	assert(!opt_psm);

	bt_io_set(iochannel, &gerr,
			BT_IO_OPT_SEC_LEVEL, sec_level,
			BT_IO_OPT_INVALID);
	if (gerr) {
		printf("# Error: %s\n", gerr->message);
                resp_error(err_COMM_ERR);
		g_error_free(gerr);
	}
	else {
		/* Tell bluepy the security level
		 * has been changed successfuly */
		cmd_status(0,0, NULL);
        }
}

static void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	uint16_t mtu;

	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		return;
	}

	if (!dec_mtu_resp(pdu, plen, &mtu)) {
		resp_error(err_PROTO_ERR);
		return;
	}

	mtu = MIN(mtu, opt_mtu);
	/* Set new value for MTU in client */
	if (g_attrib_set_mtu(attrib, mtu))
        {
                opt_mtu = mtu;
		cmd_status(0,0, NULL);
        }
	else
        {
		printf("# Error exchanging MTU\n");
		resp_error(err_COMM_ERR);
        }
}

static void cmd_mtu(int argcp, char **argvp)
{
	if (conn_state != STATE_CONNECTED) {
		resp_error(err_BAD_STATE);
		return;
	}

	assert(!opt_psm);

	if (argcp < 2) {
		resp_error(err_BAD_PARAM);
		return;
	}

	if (opt_mtu) {
		resp_error(err_BAD_STATE);
                /* Can only set once per connection */
		return;
	}

	errno = 0;
	opt_mtu = strtoll(argvp[1], NULL, 16);
	if (errno != 0 || opt_mtu < ATT_DEFAULT_LE_MTU) {
		resp_error(err_BAD_PARAM);
		return;
	}

	gatt_exchange_mtu(attrib, opt_mtu, exchange_mtu_cb, NULL);
}

static void cmd_lecc(int dev_id, int argc, char **argv)
{
	int err, opt, dd;
	bdaddr_t bdaddr;
	uint16_t interval, latency, max_ce_length, max_interval, min_ce_length;
	uint16_t min_interval, supervision_timeout, window, handle;
	uint8_t initiator_filter, own_bdaddr_type, peer_bdaddr_type;

	own_bdaddr_type = LE_PUBLIC_ADDRESS;
	peer_bdaddr_type = LE_PUBLIC_ADDRESS;
	initiator_filter = 0; /* Use peer address */

	// for_each_opt(opt, lecc_options, NULL) {
	// 	switch (opt) {
	// 	case 's':
	// 		own_bdaddr_type = LE_RANDOM_ADDRESS;
	// 		break;
	// 	case 'r':
	// 		peer_bdaddr_type = LE_RANDOM_ADDRESS;
	// 		break;
	// 	case 'w':
	// 		initiator_filter = 0x01; /* Use white list */
	// 		break;
	// 	default:
	// 		printf("%s", lecc_help);
	// 		return;
	// 	}
	// }
	//helper_arg(0, 1, &argc, &argv, lecc_help);

	if (dev_id < 0)
		dev_id = hci_get_route(NULL);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		perror("Could not open device");
		exit(1);
	}

	memset(&bdaddr, 0, sizeof(bdaddr_t));
	if (argv[0])
		str2ba(argv[0], &bdaddr);

	interval = htobs(0x0004);
	window = htobs(0x0004);
	min_interval = htobs(0x000F);
	max_interval = htobs(0x000F);
	latency = htobs(0x0000);
	supervision_timeout = htobs(0x0C80);
	min_ce_length = htobs(0x0001);
	max_ce_length = htobs(0x0001);

	err = hci_le_create_conn(dd, interval, window, initiator_filter,
			peer_bdaddr_type, bdaddr, own_bdaddr_type, min_interval,
			max_interval, latency, supervision_timeout,
			min_ce_length, max_ce_length, &handle, 25000);
	if (err < 0) {
		perror("Could not create connection");
		exit(1);
	}

	printf("Connection handle %d\n", handle);

	hci_close_dev(dd);
}

static void cmd_lecup(int dev_id, int argc, char **argv)
{
	uint16_t handle = 0, min, max, latency, timeout;
	int opt, dd;
	int options = 0;

	/* Aleatory valid values */
	min = 0x0C8;
	max = 0x0960;
	latency = 0x0007;
	timeout = 0x0C80;

	// for_each_opt(opt, lecup_options, NULL) {
	// 	switch (opt) {
	// 	case 'H':
	// 		handle = strtoul(optarg, NULL, 0);
	// 		break;
	// 	case 'm':
	// 		min = strtoul(optarg, NULL, 0);
	// 		break;
	// 	case 'M':
	// 		max = strtoul(optarg, NULL, 0);
	// 		break;
	// 	case 'l':
	// 		latency = strtoul(optarg, NULL, 0);
	// 		break;
	// 	case 't':
	// 		timeout = strtoul(optarg, NULL, 0);
	// 		break;
	// 	default:
	// 		printf("%s", lecup_help);
	// 		return;
	// 	}

	// 	options = 1;
	// }

	if (options == 0) {
		//helper_arg(5, 5, &argc, &argv, lecup_help);

		handle = strtoul(argv[0], NULL, 0);
		min = strtoul(argv[1], NULL, 0);
		max = strtoul(argv[2], NULL, 0);
		latency = strtoul(argv[3], NULL, 0);
		timeout = strtoul(argv[4], NULL, 0);
	}

	if (handle == 0) {
		//printf("%s", lecup_help);
		return;
	}

	if (dev_id < 0)
		dev_id = hci_get_route(NULL);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		fprintf(stderr, "HCI device open failed\n");
		exit(1);
	}

	if (hci_le_conn_update(dd, htobs(handle), htobs(min), htobs(max),
				htobs(latency), htobs(timeout), 5000) < 0) {
		int err = -errno;
		fprintf(stderr, "Could not change connection params: %s(%d)\n",
							strerror(-err), -err);
	}

	hci_close_dev(dd);
}

enum ENUM_COMMAND{

	ENUM_COMMAND_HELP = 0,
	ENUM_COMMAND_HCI_LIST,
	ENUM_COMMAND_HCI_RESET,
	ENUM_COMMAND_SCAN,
	ENUM_COMMAND_LESCAN,
	ENUM_COMMAND_STATUS,
	ENUM_COMMAND_CONNECT,
	ENUM_COMMAND_WRITE,
	ENUM_COMMAND_DIMMER_COLOR,
	ENUM_COMMAND_READ,
	ENUM_COMMAND_INTERACTIVE,
	ENUM_END
};

static struct {
	char *cmd;
	void (*func)(int parameter, int argcp, char **argvp);
	char *params;
	char *desc;
} commands[] = {
	{ "help",			cmd_help,			"h",			"Show this help"},
	{ "hci_list", 		cmd_print_dev_list,	"i",			"list HCI device"},
	{ "hci_reset", 		cmd_hci_reset,		"u",			"Open and initialize HCI device"},
	{ "scan",			cmd_scan,			"b",			"Scan bluetooth devices" },
	{ "lescan",			cmd_lescan,			"s", 			"Scan LE devices" },
	{ "status",			cmd_status,			"q",			"Status" },
	{ "connect",		cmd_connect,		"c", 			"(-c <address [address type]) Connect to a remote device" },
	{ "write",			cmd_char_write,		"w",			"(-w <handle> <value>) Turn ON/OFF bulb (No response)" },
	{ "dimmer/color",	cmd_char_write,		"d",			"(-d/-l <handle> <value>>) Dimmer/change color (No response)" },
	{ "read",			cmd_char_read,		"r",			"Characteristics Value/Descriptor Read" },
	{ "interactive",                     NULL ,          "t",                    "interactive" },
	{ NULL, NULL, 0}
};

static void cmd_help(int parameter,int argc ,char **argvp)
{
	printf("bthandeler_cli version 1.0 \n");
	int count;
	for (count = 0; commands[count].cmd; count++)
		printf("-%-15s %-30s %s\n", commands[count].cmd,
				commands[count].params, commands[count].desc);
        
        // cmd_status(0,0, NULL);
}

static GOptionEntry bt_options[] = {
	{ "help", 0 , 0, G_OPTION_ARG_NONE, &opt_bt_help,
		"list HCI interface)",NULL },
	{ "hci-list", 'i', 0, G_OPTION_ARG_NONE, &opt_hci_list,
		"list HCI interface)",NULL },
	{ "hci-reset", 'u', 0, G_OPTION_ARG_STRING, &opt_hci_reset,
		"reset HCI interface)",NULL },
	{ "scan", 'b', 0, G_OPTION_ARG_NONE, &opt_bt_scan,
		"scan bluetooth devices",NULL },
	{ "lescan", 's' , 0, G_OPTION_ARG_NONE, &opt_bt_lescan,
		"scan LE bluetooth devices",NULL },
	{ "status", 'q' , 0, G_OPTION_ARG_NONE, &opt_bt_status,
		"status bluetooth devices",NULL },
	{ "char-write-req", 'w', 0, G_OPTION_ARG_NONE, &opt_write,
		"Characteristics Value Write (Write Request)", NULL },
	{ "interactive", 't', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &opt_interactive,
		 "Use interactive mode", NULL },
	{ "device", 'c', 0, G_OPTION_ARG_STRING, &opt_dst,
		"Specify remote Bluetooth address", "MAC" },
	{ "handle", 'a' , 0, G_OPTION_ARG_INT, &opt_handle,
		"Read/Write characteristic by handle(required)", "0x0001" },
	{ "value", 'v' , 0, G_OPTION_ARG_STRING, &opt_value,
		"Write characteristic value (required for write operation)",
		"0x0001" },
	{ NULL },
};
// gboolean timeout_callback(gpointer data)
// {
//     static int i = 0;
//     i++;

//     if (10 == i)
//     {
//         g_main_loop_quit( (GMainLoop*)data );
//         return FALSE;
//     }
//     if(i > 9)
//     {
// 		exit(1);
// 	}

//     return TRUE;
// }
int main(int argc, char *argv[])
{
	// printf("START BTHANDLER-CLI TEST \n");

	GOptionContext *context;
	GOptionGroup *bt_group;
	GError *gerr = NULL;
	GIOChannel *chan,*pchan;
	gint events;

	opt_sec_level = g_strdup("high");
	opt_dst_type = g_strdup("public");
	// int count_argv_command ;
	char *argvp,*temp;
	char char_syntax[] = "56";
	char char_syntax_dim[]= "0faa";
	char char_syntax_color[] = "00f0aa";
	 int i , cmd = 0;

	if(argc <= 1)
	{
		cmd_help(0,0,NULL);
		return 1;
	}

	// event_loop = g_main_loop_new(NULL, FALSE);
	// chan = g_io_channel_unix_new(fileno(stdin));
	// g_io_channel_set_close_on_unref(chan, TRUE);
	// events = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	//int c;

    //        while (1) {
    //            int this_option_optind = optind ? optind : 1;
    //            int option_index = 0;
    //            static struct option long_options[] = {
    //                {"help",  	1, 0,  'h' },
    //                {"up",  		1, 0,  'u' },
    //                {"scan",  	1, 0,  'b' },
    //                {"lescan",  	1, 0,  's' },
    //                {"status",  	1, 0,  't' },
    //                {"connect",  1, 0,  'c' },
    //                {"write",    1, 0,  'w' },
    //                {"dimmer",   1, 0,  'd' },
    //                {"color",    1, 0,  'l' },
    //                {0,  0,  0,  0 }
    //            };

    //            c = getopt_long(argc, argv, "hubstcwdl",
    //                     long_options, &option_index);
    //            if (c == -1)
    //                break;
    //            for(count_argv_command =0;count_argv_command<argc;count_argv_command ++)
    //            {
    //            switch (c)
    //            	{
				//    case 0:
				//    break;
				//    case 'h':
				// 	   	printf("option 'help' %s \n",argv[1]);
				// 	   	commands[ENUM_COMMAND_HELP].func(0,0,NULL);
				// 	   break;
				//    case 'u':
				// 	   	printf("option 'hci_up': \n");
				// 	   	commands[ENUM_COMMAND_HCI_RESET].func(dev_id,argc,argv);
				// 	   	g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
				//    case 'b':
				// 	   	printf("option 'scan': \n");
				// 	   	commands[ENUM_COMMAND_SCAN].func(dev_id,argc,argv);
				// 	   	g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				//    	   break;
				//    case 's':
				// 	   	printf("option 'lescan': \n");
				// 	   	commands[ENUM_COMMAND_LESCAN].func(dev_id,argc,argv);
				// 	   	g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
				//    case 't':
				// 		printf("option 'quit' \n");
				// 		commands[ENUM_COMMAND_STATUS].func(0,argc, argv);
				// 		exit(1);
				// 	   break;
				//    case 'c':
				// 		printf("option connect \n");
				// 		printf("opt_dest: %s \n",argv[2]);
				// 		commands[ENUM_COMMAND_CONNECT].func(0,argc, argv);
				// 		g_timeout_add(100,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
				//    case 'w':
				// 		printf(" sending command to turn ON/OFF... \n");
				// 		commands[ENUM_COMMAND_WRITE].func(0,argc, argv);
				// 		g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
				//    case 'd':
				// 		printf(" sending command to dim... \n");
				// 		temp = argv[5];
				// 		temp = strcat(temp,char_syntax_dim);
				// 		argv[5] = strcat(char_syntax,temp );
				// 		commands[ENUM_COMMAND_DIMMER_COLOR].func(0,argc, argv);
				// 		g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
				//    case 'l':
				// 		printf(" sending command to change color... \n");
				// 		temp = argv[5];
				// 		temp = strcat(temp,char_syntax_color);
				// 		argv[5] = strcat(char_syntax,temp );
				// 		commands[ENUM_COMMAND_DIMMER_COLOR].func(0,argc, argv);
				// 		g_timeout_add(50,timeout_callback,event_loop);
				// 		g_main_loop_run(event_loop);
				// 	   break;
			 //   	}
			 //   break;
			 //   }
    // }
    // printf("argc : %d \n", argc);
    // printf("argv[%d]: %s \n",0,argv[0]);
    // printf("argv[%d]: %s \n",1,argv[1]);
    // printf("argv[%d]: %s \n",2,argv[2]);
    // printf("argv[%d]: %s \n",3,argv[3]);

    argvp = *argv ;

    context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, bt_options, NULL);

	bt_group = g_option_group_new("char-read-write",
		"Characteristics Value/Descriptor Read/Write arguments",
		"Show all Characteristics Value/Descriptor Read/Write "
		"arguments",
		NULL, NULL);

	g_option_context_add_group(context, bt_group);
	g_option_group_add_entries(bt_group, bt_options);
	
	 if (opt_interactive) {
                interactive(opt_src, opt_dst, opt_dst_type, opt_psm);
		goto finish;
        }

	if (g_option_context_parse(context, &argc, &argv, &gerr) == FALSE) 
	{
		g_printerr("%s\n", gerr->message);
		g_error_free(gerr);
	}

	if(opt_bt_help)
	{
		commands[ENUM_COMMAND_HELP].func(0,0,NULL);
		exit(1);
	}
	else if(opt_hci_list)
	{
		commands[ENUM_COMMAND_HCI_LIST].func(di.dev_id, argc, argv);
		exit(1);
	}
 	else if(opt_hci_reset)
 	{
		commands[ENUM_COMMAND_HCI_RESET].func(di.dev_id, argc, &argvp);
		exit(1);
 	}
	else if(opt_bt_scan)
	{
		commands[ENUM_COMMAND_SCAN].func(di.dev_id, argc, argv);
		exit(1);
	}
	else if(opt_bt_lescan)
	{
		commands[ENUM_COMMAND_LESCAN].func(di.dev_id, argc, argv);
		exit(1);
	}
	else if(opt_bt_status)
	{
		commands[ENUM_COMMAND_STATUS].func(di.dev_id, argc, argv);
		exit(1);
	}

	else if(opt_write)
	{
		commands[ENUM_COMMAND_WRITE].func(di.dev_id,argc, argv);
	}
	else if(opt_read)
	{
		commands[ENUM_COMMAND_READ].func(di.dev_id,argc, argv);
	}
	else
	{
		got_error = TRUE;;
		goto finish;
	}
 	// while (argc >0)
 	// {
 	// 	for (i = 0; commands[i].cmd; i++) 
 	// 	{
		// 	if (strncmp(commands[i].cmd,
		// 			*argv, strlen(commands[i].cmd)))
		// 		continue;

		// 	if (commands[i].cmd) {
		// 		argc--; argv++;
		// 	}

		// 	commands[i].func(di.dev_id, argc, argv);
		// 	cmd = 1;
		// 	break;
		// }

		// // if (strcasecmp(commands[i].cmd, argv[0]) == 0)
		// // {
		// // 	printf(" commands[%d].params : %s \n",i,*argv);
		// // 	fprintf(stderr, "Warning: unknown command - \"%s\"\n",
		// // 			*argv);
		// // }

		// argc--; argv++;
 	// }

	// cmd_disconnect(0, NULL);
 //    fflush(stdout);
	// g_io_channel_unref(chan);
	if (opt_dst == NULL) 
	{
		g_print("Remote Bluetooth address required\n");
		got_error = TRUE;
		goto finish;
	}

	printf("opt_dst: %s",opt_dst);
	pchan = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
					opt_psm, opt_mtu, connect_cb);
	if (pchan == NULL) 
	{
		got_error = TRUE;
		goto finish;
	}
	// else
	// {
	// 	g_io_add_watch(chan, G_IO_HUP, channel_watcher, NULL);
	// 	printf("close io \n");
	// }
	event_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(event_loop);
	g_main_loop_unref(event_loop);

finish:
	g_option_context_free(context);
	g_free(opt_src);
	g_free(opt_dst);
	g_free(opt_sec_level);

	if (got_error)
		exit(EXIT_FAILURE);
	else
		exit(EXIT_SUCCESS);
}
