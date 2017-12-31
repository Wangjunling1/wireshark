/* packet-quic.c
 * Routines for Quick UDP Internet Connections (IETF) dissection
 * Copyright 2017, Alexis La Goutte <alexis.lagoutte at gmail dot com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * See https://quicwg.github.io/
 * https://tools.ietf.org/html/draft-ietf-quic-transport-05
 */

#include <config.h>

#include <epan/packet.h>
#include <epan/expert.h>
#include "packet-ssl-utils.h"
#include <epan/prefs.h>
#include <wsutil/pint.h>

#if GCRYPT_VERSION_NUMBER >= 0x010600 /* 1.6.0 */
/* Whether to provide support for authentication in addition to decryption. */
#define HAVE_LIBGCRYPT_AEAD
#endif

/* Prototypes */
void proto_reg_handoff_quic(void);
void proto_register_quic(void);

/* Initialize the protocol and registered fields */
static int proto_quic = -1;
static int hf_quic_header_form = -1;
static int hf_quic_long_packet_type = -1;
static int hf_quic_connection_id = -1;
static int hf_quic_packet_number = -1;
static int hf_quic_version = -1;
static int hf_quic_short_cid_flag = -1;
static int hf_quic_short_kp_flag = -1;
static int hf_quic_short_packet_type = -1;
static int hf_quic_cleartext_protected_payload = -1;
static int hf_quic_initial_payload = -1;
static int hf_quic_handshake_payload = -1;
static int hf_quic_protected_payload = -1;

static int hf_quic_frame = -1;
static int hf_quic_frame_type = -1;
static int hf_quic_frame_type_stream = -1;
static int hf_quic_frame_type_stream_f = -1;
static int hf_quic_frame_type_stream_ss = -1;
static int hf_quic_frame_type_stream_oo = -1;
static int hf_quic_frame_type_stream_d = -1;
static int hf_quic_frame_type_stream_fin = -1;
static int hf_quic_frame_type_stream_len = -1;
static int hf_quic_frame_type_stream_off = -1;
static int hf_quic_stream_stream_id = -1;
static int hf_quic_stream_offset = -1;
static int hf_quic_stream_data_len = -1;
static int hf_quic_stream_length = -1;
static int hf_quic_stream_data = -1;

static int hf_quic_frame_type_ack = -1;
static int hf_quic_frame_type_ack_n = -1;
static int hf_quic_frame_type_ack_ll = -1;
static int hf_quic_frame_type_ack_mm = -1;
static int hf_quic_frame_type_ack_num_blocks = -1;
static int hf_quic_frame_type_ack_num_ts = -1;
static int hf_quic_frame_type_ack_largest_acknowledged = -1;
static int hf_quic_frame_type_ack_ack_delay = -1;
static int hf_quic_frame_type_ack_fabl = -1;
static int hf_quic_frame_type_ack_gap2nb = -1;
static int hf_quic_frame_type_ack_ack_block_length = -1;
static int hf_quic_frame_type_ack_dla = -1;
static int hf_quic_frame_type_ack_ft = -1;
static int hf_quic_frame_type_ack_tspt = -1;

static int hf_quic_frame_type_padding_length = -1;
static int hf_quic_frame_type_padding = -1;
static int hf_quic_frame_type_rsts_stream_id = -1;
static int hf_quic_frame_type_rsts_error_code = -1;
static int hf_quic_frame_type_rsts_application_error_code = -1;
static int hf_quic_frame_type_rsts_final_offset = -1;
static int hf_quic_frame_type_cc_old_error_code = -1;
static int hf_quic_frame_type_cc_error_code = -1;
static int hf_quic_frame_type_cc_reason_phrase_length = -1;
static int hf_quic_frame_type_cc_reason_phrase = -1;
static int hf_quic_frame_type_ac_error_code = -1;
static int hf_quic_frame_type_ac_reason_phrase_length = -1;
static int hf_quic_frame_type_ac_reason_phrase = -1;
static int hf_quic_frame_type_md_maximum_data = -1;
static int hf_quic_frame_type_msd_stream_id = -1;
static int hf_quic_frame_type_msd_maximum_stream_data = -1;
static int hf_quic_frame_type_msi_stream_id = -1;
static int hf_quic_frame_type_sb_stream_id = -1;
static int hf_quic_frame_type_nci_sequence = -1;
static int hf_quic_frame_type_nci_connection_id = -1;
static int hf_quic_frame_type_nci_stateless_reset_token = -1;
static int hf_quic_frame_type_ss_stream_id = -1;
static int hf_quic_frame_type_ss_error_code = -1;
static int hf_quic_frame_type_ss_application_error_code = -1;

static int hf_quic_hash = -1;

static expert_field ei_quic_ft_unknown = EI_INIT;
static expert_field ei_quic_decryption_failed = EI_INIT;

static gint ett_quic = -1;
static gint ett_quic_ft = -1;
static gint ett_quic_ftflags = -1;

static dissector_handle_t quic_handle;
static dissector_handle_t ssl_handle;

typedef struct quic_info_data {
    guint32 version;
    guint16 server_port;
    tls13_cipher *client_cleartext_cipher;
    tls13_cipher *server_cleartext_cipher;
} quic_info_data_t;

const value_string quic_version_vals[] = {
    { 0xff000004, "draft-04" },
    { 0xff000005, "draft-05" },
    { 0xff000006, "draft-06" },
    { 0xff000007, "draft-07" },
    { 0xff000008, "draft-08" },
    { 0, NULL }
};

static const value_string quic_short_long_header_vals[] = {
    { 0, "Short Header" },
    { 1, "Long Header" },
    { 0, NULL }
};

#define SH_CID  0x40
#define SH_KP   0x20
#define SH_PT   0x1F

static const value_string quic_short_packet_type_vals[] = {
    { 0x01, "1 octet" },
    { 0x02, "2 octet" },
    { 0x03, "4 octet" },
    { 0x1F, "1 octet" },
    { 0x1E, "2 octet" },
    { 0x1D, "4 octet" },
    { 0, NULL }
};

static const value_string quic_long_packet_type_vals[] = {
    { 0x01, "Version Negotiation" }, /* Removed in draft-08 by a check of Version (=0x00000000)*/
    { 0x02, "Client Initial" }, /* Replaced in draft-08 by 0x7F (Initial) */
    { 0x03, "Server Stateless Retry" }, /* Replaced in draft-08 by 0x7E (Retry) */
    { 0x04, "Server Cleartext" }, /* Replaced in draft-08 by 0x7D (Handshake) */
    { 0x05, "Client Cleartext" }, /* Replaced in draft-08 by 0x7D (Handshake) */
    { 0x06, "0-RTT Protected" },  /* Replaced in draft-08 by 0x7C (0-RTT Protected) */
    { 0x07, "1-RTT Protected (key phase 0)" }, /* Removed in draft-07 */
    { 0x08, "1-RTT Protected (key phase 1)" }, /* Removed in draft-07 */
    { 0x09, "Public Reset" }, /* Removed in draft-06*/
    { 0x7F, "Initial" },
    { 0x7E, "Retry" },
    { 0x7D, "Handshake" },
    { 0x7C, "0-RTT Protected" },
    { 0, NULL }
};

#define FT_PADDING          0x00
#define FT_RST_STREAM       0x01
#define FT_CONNECTION_CLOSE 0x02
#define FT_APPLICATION_CLOSE 0x03 /* Add in draft07 */
#define FT_MAX_DATA         0x04
#define FT_MAX_STREAM_DATA  0x05
#define FT_MAX_STREAM_ID    0x06
#define FT_PING             0x07
#define FT_BLOCKED          0x08
#define FT_STREAM_BLOCKED   0x09
#define FT_STREAM_ID_BLOCKED 0x0a
#define FT_NEW_CONNECTION_ID 0x0b
#define FT_STOP_SENDING     0x0c
#define FT_STREAM_10        0x10
#define FT_STREAM_11        0x11
#define FT_STREAM_12        0x12
#define FT_STREAM_13        0x13
#define FT_STREAM_14        0x14
#define FT_STREAM_15        0x15
#define FT_STREAM_16        0x16
#define FT_STREAM_17        0x17
#define FT_ACK_MIN          0xa0 /* <= draft-07 */
#define FT_ACK_MAX          0xbf /* <= draft-07 */
#define FT_STREAM_MIN       0xc0 /* <= draft-07 */
#define FT_STREAM_MAX       0xff /* <= draft-07 */

static const range_string quic_frame_type_vals[] = {
    { 0x00, 0x00,   "PADDING" },
    { 0x01, 0x01,   "RST_STREAM" },
    { 0x02, 0x02,   "CONNECTION_CLOSE" },
    { 0x03, 0x03,   "APPLICATION_CLOSE" },
    { 0x04, 0x04,   "MAX_DATA" },
    { 0x05, 0x05,   "MAX_STREAM_DATA" },
    { 0x06, 0x06,   "MAX_STREAM_ID" },
    { 0x07, 0x07,   "PING" },
    { 0x08, 0x08,   "BLOCKED" },
    { 0x09, 0x09,   "STREAM_BLOCKED" },
    { 0x0a, 0x0a,   "STREAM_ID_BLOCKED" },
    { 0x0b, 0x0b,   "NEW_CONNECTION_ID" },
    { 0x0c, 0x0c,   "STOP_SENDING" },
    { 0x10, 0x17,   "STREAM" },
    { 0xa0, 0xbf,   "ACK" }, /* <= draft-07 */
    { 0xc0, 0xff,   "STREAM" },  /* <= draft-07 */
    { 0,    0,        NULL },
};

/* <= draft-07 */
#define FTFLAGS_STREAM_STREAM 0xC0
#define FTFLAGS_STREAM_F    0x20
#define FTFLAGS_STREAM_SS   0x18
#define FTFLAGS_STREAM_OO   0x06
#define FTFLAGS_STREAM_D    0x01

#define FTFLAGS_ACK_ACK     0xE0
#define FTFLAGS_ACK_N       0x10
#define FTFLAGS_ACK_LL      0x0C
#define FTFLAGS_ACK_MM      0x03

static const value_string len_offset_vals[] = {
    { 0, "0 Byte" },
    { 1, "2 Bytes" },
    { 2, "4 Bytes" },
    { 3, "8 Bytes" },
    { 0, NULL }
};

static const value_string len_stream_vals[] = {
    { 0, "1 Byte" },
    { 1, "2 Bytes" },
    { 2, "3 Bytes" },
    { 3, "4 Bytes" },
    { 0, NULL }
};

static const true_false_string len_data_vals = {
    "2 Bytes",
    "0 Byte"
};

static const value_string len_largest_acknowledged_vals[] = {
    { 0, "1 Byte" },
    { 1, "2 Bytes" },
    { 2, "4 Bytes" },
    { 3, "8 Bytes" },
    { 0, NULL }
};

static const value_string len_ack_block_vals[] = {
    { 0, "1 Byte" },
    { 1, "2 Bytes" },
    { 2, "4 Bytes" },
    { 3, "8 Bytes" },
    { 0, NULL }
};

/* >= draft-08 */
#define FTFLAGS_STREAM_FIN 0x01
#define FTFLAGS_STREAM_LEN 0x02
#define FTFLAGS_STREAM_OFF 0x04

/* draft05-06 */
#define QUIC_OLD_NO_ERROR                   0x80000000
#define QUIC_OLD_INTERNAL_ERROR             0x80000001
#define QUIC_OLD_CANCELLED                  0x80000002
#define QUIC_OLD_FLOW_CONTROL_ERROR         0x80000003
#define QUIC_OLD_STREAM_ID_ERROR            0x80000004
#define QUIC_OLD_STREAM_STATE_ERROR         0x80000005
#define QUIC_OLD_FINAL_OFFSET_ERROR         0x80000006
#define QUIC_OLD_FRAME_FORMAT_ERROR         0x80000007
#define QUIC_OLD_TRANSPORT_PARAMETER_ERROR  0x80000008
#define QUIC_OLD_VERSION_NEGOTIATION_ERROR  0x80000009
#define QUIC_OLD_PROTOCOL_VIOLATION         0x8000000A

/* QUIC TLS Error */
#define QUIC_OLD_TLS_HANDSHAKE_FAILED       0xC000001C
#define QUIC_OLD_TLS_FATAL_ALERT_GENERATED  0xC000001D
#define QUIC_OLD_TLS_FATAL_ALERT_RECEIVED   0xC000001E

static const value_string quic_old_error_code_vals[] = {
    { QUIC_OLD_NO_ERROR, "NO_ERROR (An endpoint uses this with CONNECTION_CLOSE to signal that the connection is being closed abruptly in the absence of any error)" },
    { QUIC_OLD_INTERNAL_ERROR, "INTERNAL_ERROR (The endpoint encountered an internal error and cannot continue with the connection)" },
    { QUIC_OLD_CANCELLED, "CANCELLED (An endpoint sends this with RST_STREAM to indicate that the stream is not wanted and that no application action was taken for the stream)" },
    { QUIC_OLD_FLOW_CONTROL_ERROR, "FLOW_CONTROL_ERROR (An endpoint received more data than An endpoint received more data tha)" },
    { QUIC_OLD_STREAM_ID_ERROR, "STREAM_ID_ERROR (An endpoint received a frame for a stream identifier that exceeded its advertised maximum stream ID)" },
    { QUIC_OLD_STREAM_STATE_ERROR, "STREAM_STATE_ERROR (An endpoint received a frame for a stream that was not in a state that permitted that frame)" },
    { QUIC_OLD_FINAL_OFFSET_ERROR, "FINAL_OFFSET_ERROR (An endpoint received a STREAM frame containing data that exceeded the previously established final offset)" },
    { QUIC_OLD_FRAME_FORMAT_ERROR, "FRAME_FORMAT_ERROR (An endpoint received a frame that was badly formatted)" },
    { QUIC_OLD_TRANSPORT_PARAMETER_ERROR, "TRANSPORT_PARAMETER_ERROR (An endpoint received transport parameters that were badly formatted)" },
    { QUIC_OLD_VERSION_NEGOTIATION_ERROR, "VERSION_NEGOTIATION_ERROR (An endpoint received transport parameters that contained version negotiation parameters that disagreed with the version negotiation that it performed)" },
    { QUIC_OLD_PROTOCOL_VIOLATION, "PROTOCOL_VIOLATION (An endpoint detected an error with protocol compliance that was not covered by more specific error codes)" },
    { QUIC_OLD_TLS_HANDSHAKE_FAILED, "TLS_HANDSHAKE_FAILED (The TLS handshake failed)" },
    { QUIC_OLD_TLS_FATAL_ALERT_GENERATED, "TLS_FATAL_ALERT_GENERATED (A TLS fatal alert was sent, causing the TLS connection to end prematurel)" },
    { QUIC_OLD_TLS_FATAL_ALERT_RECEIVED, "TLS_FATAL_ALERT_RECEIVED (A TLS fatal alert was received, causing the TLS connection to end prematurely)" },
    { 0, NULL }
};
static value_string_ext quic_old_error_code_vals_ext = VALUE_STRING_EXT_INIT(quic_old_error_code_vals);

/* > draft 07 */
#define QUIC_NO_ERROR                   0x0000
#define QUIC_INTERNAL_ERROR             0x0001
#define QUIC_FLOW_CONTROL_ERROR         0x0003
#define QUIC_STREAM_ID_ERROR            0x0004
#define QUIC_STREAM_STATE_ERROR         0x0005
#define QUIC_FINAL_OFFSET_ERROR         0x0006
#define QUIC_FRAME_FORMAT_ERROR         0x0007
#define QUIC_TRANSPORT_PARAMETER_ERROR  0x0008
#define QUIC_VERSION_NEGOTIATION_ERROR  0x0009
#define QUIC_PROTOCOL_VIOLATION         0x000A

static const value_string quic_error_code_vals[] = {
    { QUIC_NO_ERROR, "NO_ERROR (An endpoint uses this with CONNECTION_CLOSE to signal that the connection is being closed abruptly in the absence of any error.)" },
    { QUIC_INTERNAL_ERROR, "INTERNAL_ERROR (The endpoint encountered an internal error and cannot continue with the connection)" },
    { QUIC_FLOW_CONTROL_ERROR, "FLOW_CONTROL_ERROR (An endpoint received more data than An endpoint received more data tha)" },
    { QUIC_STREAM_ID_ERROR, "STREAM_ID_ERROR (An endpoint received a frame for a stream identifier that exceeded its advertised maximum stream ID)" },
    { QUIC_STREAM_STATE_ERROR, "STREAM_STATE_ERROR (An endpoint received a frame for a stream that was not in a state that permitted that frame)" },
    { QUIC_FINAL_OFFSET_ERROR, "FINAL_OFFSET_ERROR (An endpoint received a STREAM frame containing data that exceeded the previously established final offset)" },
    { QUIC_FRAME_FORMAT_ERROR, "FRAME_FORMAT_ERROR (An endpoint received a frame that was badly formatted)" },
    { QUIC_TRANSPORT_PARAMETER_ERROR, "TRANSPORT_PARAMETER_ERROR (An endpoint received transport parameters that were badly formatted)" },
    { QUIC_VERSION_NEGOTIATION_ERROR, "VERSION_NEGOTIATION_ERROR (An endpoint received transport parameters that contained version negotiation parameters that disagreed with the version negotiation that it performed)" },
    { QUIC_PROTOCOL_VIOLATION, "PROTOCOL_VIOLATION (An endpoint detected an error with protocol compliance that was not covered by more specific error codes)" },
    { 0, NULL }
};
static value_string_ext quic_error_code_vals_ext = VALUE_STRING_EXT_INIT(quic_error_code_vals);


static guint32 get_len_offset(guint8 frame_type){

    switch((frame_type & FTFLAGS_STREAM_OO) >> 1){
        case 0:
            return 0;
        break;
        case 1:
            return 2;
        break;
        case 2:
            return 4;
        break;
        case 3:
            return 8;
        break;
        default:
        break;
    }
    return 0;
}
static guint32 get_len_stream(guint8 frame_type){

    switch((frame_type & FTFLAGS_STREAM_SS) >> 3){
        case 0:
            return 1;
        break;
        case 1:
            return 2;
        break;
        case 2:
            return 3;
        break;
        case 3:
            return 4;
        break;
        default:
        break;
    }
    return 1;
}

static guint32 get_len_largest_acknowledged(guint8 frame_type){

    switch((frame_type & FTFLAGS_ACK_LL) >> 2){
        case 0:
            return 1;
        break;
        case 1:
            return 2;
        break;
        case 2:
            return 4;
        break;
        case 3:
            return 8;
        break;
        default:
        break;
    }
    return 1;
}
static guint32 get_len_ack_block(guint8 frame_type){

    switch(frame_type & FTFLAGS_ACK_MM){
        case 0:
            return 1;
        break;
        case 1:
            return 2;
        break;
        case 2:
            return 4;
        break;
        case 3:
            return 8;
        break;
        default:
        break;
    }
    return 1;
}

static guint32 get_len_packet_number(guint8 short_packet_type){

    switch(short_packet_type & SH_PT){
        case 1:
        case 0x1F:
            return 1;
        break;
        case 2:
        case 0x1E:
            return 2;
        break;
        case 3:
        case 0x1D:
            return 4;
        break;
        default:
        break;
    }
    return 1;
}

static int
dissect_quic_frame_type(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *quic_tree, guint offset, quic_info_data_t *quic_info){
    proto_item *ti_ft, *ti_ftflags;
    proto_tree *ft_tree, *ftflags_tree;
    guint32 frame_type;

    ti_ft = proto_tree_add_item(quic_tree, hf_quic_frame, tvb, offset, 1, ENC_NA);
    ft_tree = proto_item_add_subtree(ti_ft, ett_quic_ft);

    ti_ftflags = proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type, tvb, offset, 1, ENC_NA, &frame_type);
    proto_item_set_text(ti_ft, "%s", rval_to_str(frame_type, quic_frame_type_vals, "Unknown"));

    if(frame_type >= FT_STREAM_MIN && frame_type <= FT_STREAM_MAX) {
        guint32 len_stream = 0, len_offset = 0, len_data = 0, data_len = 0;
        guint64 stream_id;
        proto_item *ti_stream;

        ftflags_tree = proto_item_add_subtree(ti_ftflags, ett_quic_ftflags);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream, tvb, offset, 1, ENC_NA);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_f, tvb, offset, 1, ENC_NA);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_ss, tvb, offset, 1, ENC_NA);
        len_offset = get_len_offset(frame_type);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_oo, tvb, offset, 1, ENC_NA);
        len_stream = get_len_stream(frame_type);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_d, tvb, offset, 1, ENC_NA);
        if(frame_type & FTFLAGS_STREAM_D){
            len_data = 2;
        }
        offset += 1;

        ti_stream = proto_tree_add_item_ret_uint64(ft_tree, hf_quic_stream_stream_id, tvb, offset, len_stream, ENC_BIG_ENDIAN, &stream_id);
        offset += len_stream;

        proto_item_append_text(ti_ft, " Stream ID: %" G_GINT64_MODIFIER "u", stream_id);

        if (len_offset) {
            proto_tree_add_item(ft_tree, hf_quic_stream_offset, tvb, offset, len_offset, ENC_BIG_ENDIAN);
            offset += len_offset;
        }

        if (len_data) {
            proto_tree_add_item_ret_uint(ft_tree, hf_quic_stream_data_len, tvb, offset, len_data, ENC_BIG_ENDIAN, &data_len);
            offset += len_data;
        } else {
           data_len = tvb_reported_length_remaining(tvb, offset);
        }

        proto_tree_add_item(ft_tree, hf_quic_stream_data, tvb, offset, data_len, ENC_NA);

        if (stream_id == 0) { /* Special Stream */
            tvbuff_t *next_tvb;

            proto_item_append_text(ti_stream, " (Cryptographic handshake)");
            col_set_writable(pinfo->cinfo, -1, FALSE);
            next_tvb = tvb_new_subset_length(tvb, offset, data_len);
            call_dissector(ssl_handle, next_tvb, pinfo, ft_tree);
            col_set_writable(pinfo->cinfo, -1, TRUE);
        }
        offset += data_len;

    } else if (frame_type >= FT_ACK_MIN && frame_type <= FT_ACK_MAX ){
        guint32 len_largest_acknowledged = 0, len_ack_block = 0;
        guint8 num_blocks = 0, num_ts = 0;

        ftflags_tree = proto_item_add_subtree(ti_ftflags, ett_quic_ftflags);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_ack, tvb, offset, 1, ENC_NA);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_ack_n, tvb, offset, 1, ENC_NA);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_ack_ll, tvb, offset, 1, ENC_NA);
        len_largest_acknowledged = get_len_largest_acknowledged(frame_type);
        proto_tree_add_item(ftflags_tree, hf_quic_frame_type_ack_mm, tvb, offset, 1, ENC_NA);
        len_ack_block = get_len_ack_block(frame_type);
        offset += 1;

        if(frame_type & FTFLAGS_ACK_N){
            proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_num_blocks, tvb, offset, 1, ENC_NA);
            num_blocks = tvb_get_guint8(tvb, offset);
            offset += 1;
        }

        /* No longer Timestamps Block with draft07 */
        if(quic_info->version == 0xFF000005 || quic_info->version == 0xFF000006) {
            proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_num_ts, tvb, offset, 1, ENC_NA);
            num_ts = tvb_get_guint8(tvb , offset);
            offset += 1;
        }
        proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_largest_acknowledged, tvb, offset, len_largest_acknowledged, ENC_BIG_ENDIAN);
        offset += len_largest_acknowledged;

        proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_ack_delay, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        /* ACK Block Section */
        /* First ACK Block Length */
        proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_fabl, tvb, offset, len_ack_block, ENC_BIG_ENDIAN);
        offset += len_ack_block;

        /* Repeated "Num Blocks" */
        while(num_blocks){

            /* Gap To Next Block */
            proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_gap2nb, tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;

            proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_ack_block_length, tvb, offset, len_ack_block, ENC_BIG_ENDIAN);
            offset += len_ack_block;

            num_blocks--;
        }

        if(quic_info->version == 0xFF000005 || quic_info->version == 0xFF000006) {
            /* Timestamp Section */
            if(num_ts){

                /* Delta Largest Acknowledged */
                proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_dla, tvb, offset, 1, ENC_BIG_ENDIAN);
                offset += 1;

                /* First Timestamp */
                proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_ft, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;

                num_ts--;
                /* Repeated "Num Timestamps - 1" */
                while(num_ts){

                    /* Delta Largest Acknowledged */
                    proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_dla, tvb, offset, 1, ENC_BIG_ENDIAN);
                    offset += 1;

                    /* Time Since Previous Timestamp*/
                    proto_tree_add_item(ft_tree, hf_quic_frame_type_ack_tspt, tvb, offset, 2, ENC_BIG_ENDIAN);
                    offset += 2;

                    num_ts--;
                }
            }
        }

    } else { /* it is not STREAM or ACK Frame (<= draft-07)*/
        offset += 1;
        switch(frame_type){
            case FT_PADDING:{
                proto_item *ti_pad_len;
                guint32 pad_len = tvb_reported_length_remaining(tvb, offset);

                ti_pad_len = proto_tree_add_uint(ft_tree, hf_quic_frame_type_padding_length, tvb, offset, 0, pad_len);
                PROTO_ITEM_SET_GENERATED(ti_pad_len);
                proto_item_append_text(ti_ft, " Length: %u", pad_len);
                //TODO: Add check if always 0 ?
                proto_tree_add_item(ft_tree, hf_quic_frame_type_padding, tvb, offset, -1, ENC_NA);
                offset += pad_len;
                proto_item_set_len(ti_ft, 1+pad_len);
            }
            break;
            case FT_RST_STREAM:{
                guint32 stream_id, error_code;
                proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_rsts_stream_id, tvb, offset, 4, ENC_BIG_ENDIAN, &stream_id);
                offset += 4;
                if(quic_info->version == 0xFF000005 || quic_info->version == 0xFF000006) {
                    proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_rsts_error_code, tvb, offset, 4, ENC_BIG_ENDIAN, &error_code);
                    offset += 4;
                } else {
                    proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_rsts_application_error_code, tvb, offset, 2, ENC_BIG_ENDIAN, &error_code);
                    offset += 2;
                }
                proto_tree_add_item(ft_tree, hf_quic_frame_type_rsts_final_offset, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;

                proto_item_append_text(ti_ft, " Stream ID: %u, Error code: %s", stream_id, val_to_str_ext(error_code, &quic_error_code_vals_ext, "Unknown (%d)"));
                if(quic_info->version == 0xFF000005 || quic_info->version == 0xFF000006) {
                    proto_item_set_len(ti_ft, 1 + 4 + 4 + 8);
                } else {
                    proto_item_set_len(ti_ft, 1 + 4 + 2 + 8);
                }

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "RST STREAM, ");

            }
            break;
            case FT_CONNECTION_CLOSE:{
                guint32 len_reason, error_code;

                if(quic_info->version == 0xff000005 || quic_info->version == 0xff000006) {
                    proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_cc_old_error_code, tvb, offset, 4, ENC_BIG_ENDIAN, &error_code);
                    offset += 4;
                } else { /* Only 2 bytes with draft07 */
                    proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_cc_error_code, tvb, offset, 2, ENC_BIG_ENDIAN, &error_code);
                    offset += 2;
                }
                proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_cc_reason_phrase_length, tvb, offset, 2, ENC_BIG_ENDIAN, &len_reason);
                offset += 2;
                proto_tree_add_item(ft_tree, hf_quic_frame_type_cc_reason_phrase, tvb, offset, len_reason, ENC_ASCII|ENC_NA);
                offset += len_reason;

                proto_item_append_text(ti_ft, " Error code: %s", val_to_str_ext(error_code, &quic_old_error_code_vals_ext, "Unknown (%d)"));
                if(quic_info->version == 0xff000005 || quic_info->version == 0xff000006) {
                    proto_item_set_len(ti_ft, 1 + 4 + 2 + len_reason);
                } else {
                    proto_item_set_len(ti_ft, 1 + 2 + 2 + len_reason);
                }
                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Connection Close");

            }
            break;
            case FT_APPLICATION_CLOSE:{
                guint32 len_reason, error_code;

                proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_ac_error_code, tvb, offset, 2, ENC_BIG_ENDIAN, &error_code);
                offset += 2;
                proto_tree_add_item_ret_uint(ft_tree, hf_quic_frame_type_ac_reason_phrase_length, tvb, offset, 2, ENC_BIG_ENDIAN, &len_reason);
                offset += 2;
                proto_tree_add_item(ft_tree, hf_quic_frame_type_ac_reason_phrase, tvb, offset, len_reason, ENC_ASCII|ENC_NA);
                offset += len_reason;

                proto_item_append_text(ti_ft, " Error code: %s", val_to_str_ext(error_code, &quic_error_code_vals_ext, "Unknown (%d)"));
                proto_item_set_len(ti_ft, 1 + 2 + 2 + len_reason);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Application Close");

            }
            break;
            case FT_MAX_DATA:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_md_maximum_data, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;

                proto_item_set_len(ti_ft, 1 + 8);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Max Data");

            }
            break;
            case FT_MAX_STREAM_DATA:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_msd_stream_id, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;
                proto_tree_add_item(ft_tree, hf_quic_frame_type_msd_maximum_stream_data, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;

                proto_item_set_len(ti_ft, 1 + 4 + 8);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Max Stream Data");

            }
            break;
            case FT_MAX_STREAM_ID:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_msi_stream_id, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;

                proto_item_set_len(ti_ft, 1 + 4);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Max Stream ID");

            }
            break;
            case FT_PING:{

                /* No Payload */

                proto_item_set_len(ti_ft, 1);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "PING");
            }
            break;
            case FT_BLOCKED:{

                /* No Payload */

                proto_item_set_len(ti_ft, 1);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Blocked");
            }
            break;
            case FT_STREAM_BLOCKED:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_sb_stream_id, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;

                proto_item_set_len(ti_ft, 1 + 4);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Stream Blocked");

            }
            break;
            case FT_STREAM_ID_BLOCKED:{

                /* No Payload */

                proto_item_set_len(ti_ft, 1);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Stream ID Blocked");
            }
            break;
            case FT_NEW_CONNECTION_ID:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_nci_sequence, tvb, offset, 2, ENC_BIG_ENDIAN);
                offset += 2;

                proto_tree_add_item(ft_tree, hf_quic_frame_type_nci_connection_id, tvb, offset, 8, ENC_BIG_ENDIAN);
                offset += 8;

                proto_tree_add_item(ft_tree, hf_quic_frame_type_nci_stateless_reset_token, tvb, offset, 16, ENC_NA);
                offset += 16;

                proto_item_set_len(ti_ft, 1 + 2 + 8 + 16);

                col_prepend_fstr(pinfo->cinfo, COL_INFO, "New Connection ID");

            }
            break;
            case FT_STOP_SENDING:{

                proto_tree_add_item(ft_tree, hf_quic_frame_type_ss_stream_id, tvb, offset, 4, ENC_BIG_ENDIAN);
                offset += 4;

                if(quic_info->version == 0xff000005 || quic_info->version == 0xff000006) {
                    proto_tree_add_item(ft_tree, hf_quic_frame_type_ss_error_code, tvb, offset, 4, ENC_BIG_ENDIAN);
                    offset += 4;

                    proto_item_set_len(ti_ft, 1 + 4 + 4);
                } else {
                    proto_tree_add_item(ft_tree, hf_quic_frame_type_ss_application_error_code, tvb, offset, 2, ENC_BIG_ENDIAN);
                    offset += 2;

                    proto_item_set_len(ti_ft, 1 + 4 + 2);
                }
                col_prepend_fstr(pinfo->cinfo, COL_INFO, "Stop Sending");

            }
            break;
            case FT_STREAM_10:
            case FT_STREAM_11:
            case FT_STREAM_12:
            case FT_STREAM_13:
            case FT_STREAM_14:
            case FT_STREAM_15:
            case FT_STREAM_16:
            case FT_STREAM_17: {
                guint64 stream_id, length;
                guint32 lenvar;
                proto_item *ti_stream;

                offset -= 1;

                ftflags_tree = proto_item_add_subtree(ti_ftflags, ett_quic_ftflags);
                proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_fin, tvb, offset, 1, ENC_NA);
                proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_len, tvb, offset, 1, ENC_NA);
                proto_tree_add_item(ftflags_tree, hf_quic_frame_type_stream_off, tvb, offset, 1, ENC_NA);
                offset += 1;

                ti_stream = proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_stream_id, tvb, offset, -1, ENC_VARINT_QUIC, &stream_id, &lenvar);
                offset += lenvar;

                proto_item_append_text(ti_ft, " Stream ID: %" G_GINT64_MODIFIER "u", stream_id);

                if (frame_type & FTFLAGS_STREAM_OFF) {
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_offset, tvb, offset, -1, ENC_VARINT_QUIC, NULL, &lenvar);
                    offset += lenvar;
                }

                if (frame_type & FTFLAGS_STREAM_LEN) {
                    proto_tree_add_item_ret_varint(ft_tree, hf_quic_stream_length, tvb, offset, -1, ENC_VARINT_QUIC, &length, &lenvar);
                    offset += lenvar;
                } else {
                   length = tvb_reported_length_remaining(tvb, offset);
                }

                proto_tree_add_item(ft_tree, hf_quic_stream_data, tvb, offset, (int)length, ENC_NA);

                if (stream_id == 0) { /* Special Stream */
                    tvbuff_t *next_tvb;

                    proto_item_append_text(ti_stream, " (Cryptographic handshake)");
                    col_set_writable(pinfo->cinfo, -1, FALSE);
                    next_tvb = tvb_new_subset_length(tvb, offset, (int)length);
                    call_dissector(ssl_handle, next_tvb, pinfo, ft_tree);
                    col_set_writable(pinfo->cinfo, -1, TRUE);
                }
                offset += (int)length;
            }
            break;
            default:
                expert_add_info_format(pinfo, ti_ft, &ei_quic_ft_unknown, "Unknown Frame Type %u", frame_type);
            break;
        }
    }

    return offset;
}

/* TLS 1.3 draft used by the draft-ietf-quic-tls-07 */
#define QUIC_TLS13_VERSION          21
#define QUIC_LONG_HEADER_LENGTH     17

#ifdef HAVE_LIBGCRYPT_AEAD
/**
 * Given a QUIC message (header + non-empty payload), the actual packet number,
 * try to decrypt it using the cipher.
 *
 * The actual packet number must be constructed according to
 * https://tools.ietf.org/html/draft-ietf-quic-transport-07#section-5.7
 *
 * If decryption succeeds, the decrypted buffer is added as data source and
 * returned. Otherwise NULL is returned and an error message is set.
 */
static tvbuff_t *
quic_decrypt_message(tls13_cipher *cipher, tvbuff_t *head, packet_info *pinfo, guint header_length, guint64 packet_number, const gchar **error)
{
    gcry_error_t    err;
    guint8          header[QUIC_LONG_HEADER_LENGTH];
    guint8          nonce[TLS13_AEAD_NONCE_LENGTH];
    guint8         *buffer;
    guint8         *atag[16];
    guint           buffer_length;
    tvbuff_t       *decrypted;

    DISSECTOR_ASSERT(cipher != NULL);
    DISSECTOR_ASSERT(header_length <= sizeof(header));
    tvb_memcpy(head, header, 0, header_length);

    /* Input is "header || ciphertext (buffer) || auth tag (16 bytes)" */
    buffer_length = tvb_captured_length_remaining(head, header_length + 16);
    if (buffer_length == 0) {
        *error = "Decryption not possible, ciphertext is too short";
        return NULL;
    }
    buffer = (guint8 *)tvb_memdup(pinfo->pool, head, header_length, buffer_length);
    tvb_memcpy(head, atag, header_length + buffer_length, 16);

    memcpy(nonce, cipher->iv, TLS13_AEAD_NONCE_LENGTH);
    /* Packet number is left-padded with zeroes and XORed with write_iv */
    phton64(nonce + sizeof(nonce) - 8, pntoh64(nonce + sizeof(nonce) - 8) ^ packet_number);

    gcry_cipher_reset(cipher->hd);
    err = gcry_cipher_setiv(cipher->hd, nonce, TLS13_AEAD_NONCE_LENGTH);
    if (err) {
        *error = wmem_strdup_printf(wmem_packet_scope(), "Decryption (setiv) failed: %s", gcry_strerror(err));
        return NULL;
    }

    /* associated data (A) is the contents of QUIC header */
    err = gcry_cipher_authenticate(cipher->hd, header, header_length);
    if (err) {
        *error = wmem_strdup_printf(wmem_packet_scope(), "Decryption (authenticate) failed: %s", gcry_strerror(err));
        return NULL;
    }

    /* Output ciphertext (C) */
    err = gcry_cipher_decrypt(cipher->hd, buffer, buffer_length, NULL, 0);
    if (err) {
        *error = wmem_strdup_printf(wmem_packet_scope(), "Decryption (decrypt) failed: %s", gcry_strerror(err));
        return NULL;
    }

    err = gcry_cipher_checktag(cipher->hd, atag, 16);
    if (err) {
        *error = wmem_strdup_printf(wmem_packet_scope(), "Decryption (checktag) failed: %s", gcry_strerror(err));
        return NULL;
    }

    decrypted = tvb_new_child_real_data(head, buffer, buffer_length, buffer_length);
    add_new_data_source(pinfo, decrypted, "Decrypted QUIC");

    *error = NULL;
    return decrypted;
}

/**
 * Compute the client and server cleartext secrets given Connection ID "cid".
 *
 * On success TRUE is returned and the two cleartext secrets are returned (these
 * must be freed with wmem_free(NULL, ...)). FALSE is returned on error.
 */
static gboolean
quic_derive_cleartext_secrets(guint64 cid,
                              guint8 **client_cleartext_secret,
                              guint8 **server_cleartext_secret,
                              quic_info_data_t *quic_info,
                              const gchar **error)
{

    /*
     * https://tools.ietf.org/html/draft-ietf-quic-tls-08#section-5.2.1
     *
     * quic_version_1_salt = afc824ec5fc77eca1e9d36f37fb2d46518c36639
     *
     * cleartext_secret = HKDF-Extract(quic_version_1_salt,
     *                                 client_connection_id)
     *
     * client_cleartext_secret =
     *                    HKDF-Expand-Label(cleartext_secret,
     *                                      "QUIC client handshake secret",
     *                                      "", Hash.length)
     * server_cleartext_secret =
     *                    HKDF-Expand-Label(cleartext_secret,
     *                                      "QUIC server handshake secret",
     *                                      "", Hash.length)
     * Hash for cleartext packets is SHA-256 (output size 32).
     */
    static const guint8 quic_version_1_salt[20] = {
        0xaf, 0xc8, 0x24, 0xec, 0x5f, 0xc7, 0x7e, 0xca, 0x1e, 0x9d,
        0x36, 0xf3, 0x7f, 0xb2, 0xd4, 0x65, 0x18, 0xc3, 0x66, 0x39
    };
    guint           tls13_draft_version = QUIC_TLS13_VERSION;
    gcry_error_t    err;
    guint8          secret_bytes[HASH_SHA2_256_LENGTH];
    StringInfo      secret = { (guchar *) &secret_bytes, HASH_SHA2_256_LENGTH };
    guint8          cid_bytes[8];
    const gchar     *client_label = "QUIC client handshake secret";
    const gchar     *server_label = "QUIC server handshake secret";

    /* draft-07 uses "cleartext Secret" instead of "handshake secret" in labels */
    if(quic_info->version == 0xFF000007) {
       client_label = "QUIC client cleartext Secret";
       server_label = "QUIC server cleartext Secret";
    }

    phton64(cid_bytes, cid);
    err = hkdf_extract(GCRY_MD_SHA256, quic_version_1_salt, sizeof(quic_version_1_salt),
                       cid_bytes, sizeof(cid_bytes), secret.data);
    if (err) {
        *error = wmem_strdup_printf(wmem_packet_scope(), "Failed to extract secrets: %s", gcry_strerror(err));
        return FALSE;
    }

    if (!tls13_hkdf_expand_label(tls13_draft_version, GCRY_MD_SHA256, &secret, client_label,
                                 "", HASH_SHA2_256_LENGTH, client_cleartext_secret)) {
        *error = "Key expansion (client) failed";
        return FALSE;
    }

    if (!tls13_hkdf_expand_label(tls13_draft_version, GCRY_MD_SHA256, &secret, server_label,
                                 "", HASH_SHA2_256_LENGTH, server_cleartext_secret)) {
        wmem_free(NULL, client_cleartext_secret);
        *client_cleartext_secret = NULL;
        *error = "Key expansion (server) failed";
        return FALSE;
    }

    *error = NULL;
    return TRUE;
}

static gboolean
quic_create_cleartext_decoders(guint64 cid, const gchar **error, quic_info_data_t *quic_info)
{
    tls13_cipher   *client_cipher, *server_cipher;
    StringInfo      client_secret = { NULL, HASH_SHA2_256_LENGTH };
    StringInfo      server_secret = { NULL, HASH_SHA2_256_LENGTH };

    /* TODO extract from packet/conversation */
    if (!quic_derive_cleartext_secrets(cid, &client_secret.data, &server_secret.data, quic_info, error)) {
        /* TODO handle error (expert info) */
        return FALSE;
    }

    /* Cleartext packets are protected with AEAD_AES_128_GCM */
    client_cipher = tls13_cipher_create(QUIC_TLS13_VERSION, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_GCM, GCRY_MD_SHA256, &client_secret, error);
    server_cipher = tls13_cipher_create(QUIC_TLS13_VERSION, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_GCM, GCRY_MD_SHA256, &server_secret, error);
    if (!client_cipher || !server_cipher) {
        return FALSE;
    }

    quic_info->client_cleartext_cipher = client_cipher;
    quic_info->server_cleartext_cipher = server_cipher;

    return TRUE;
}
#endif /* HAVE_LIBGCRYPT_AEAD */


static int
dissect_quic_long_header(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree, guint offset, quic_info_data_t *quic_info){
    guint32 long_packet_type, pkn;
    guint64 cid;
    tvbuff_t *payload_tvb;

    proto_tree_add_item_ret_uint(quic_tree, hf_quic_long_packet_type, tvb, offset, 1, ENC_NA, &long_packet_type);
    offset += 1;

    proto_tree_add_item_ret_uint64(quic_tree, hf_quic_connection_id, tvb, offset, 8, ENC_BIG_ENDIAN, &cid);
    offset += 8;

    if(long_packet_type < 0x09) { /*Before draft-08 */
        proto_tree_add_item_ret_uint(quic_tree, hf_quic_packet_number, tvb, offset, 4, ENC_BIG_ENDIAN, &pkn);
        offset += 4;

        proto_tree_add_item_ret_uint(quic_tree, hf_quic_version, tvb, offset, 4, ENC_BIG_ENDIAN, &quic_info->version);
        offset += 4;
    } else {
        proto_tree_add_item_ret_uint(quic_tree, hf_quic_version, tvb, offset, 4, ENC_BIG_ENDIAN, &quic_info->version);
        offset += 4;
        /* Add version check (0x00000000) it is not a Negociation Packet */

        proto_tree_add_item_ret_uint(quic_tree, hf_quic_packet_number, tvb, offset, 4, ENC_BIG_ENDIAN, &pkn);
        offset += 4;
    }

    col_append_fstr(pinfo->cinfo, COL_INFO, "%s, PKN: %u, CID: 0x%" G_GINT64_MODIFIER "x", val_to_str(long_packet_type, quic_long_packet_type_vals, "Unknown Packet Type"), pkn, cid);

    /* Payload */
    /* Version Negociation (0x01)*/
    if(long_packet_type == 0x01){
        payload_tvb = tvb_new_subset_length(tvb, 0, tvb_reported_length(tvb) - 8);

        while(tvb_reported_length_remaining(payload_tvb, offset) > 0){
            proto_tree_add_item(quic_tree, hf_quic_version, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }

        /* FNV-1a hash, TODO: Add check and expert info ? */
        proto_tree_add_item(quic_tree, hf_quic_hash, tvb, offset, 8, ENC_NA);
        offset += 8;

    /*  Client Initial (0x02), Server Stateless Retry (0x03), Server ClearText (0x04),
        Client ClearText (0x05) and Public Reset (0x09) */
    } else if(long_packet_type <= 0x05 || long_packet_type == 0x09) {
        if(quic_info->version == 0xff000005 || quic_info->version == 0xff000006) { /* draft05 and draft06 use FNV Protection */
            /* All Unprotected have 8 bytes with FNV-1a has (See QUIC-TLS) */
            payload_tvb = tvb_new_subset_length(tvb, 0, tvb_reported_length(tvb) - 8);

            while(tvb_reported_length_remaining(payload_tvb, offset) > 0){
                offset = dissect_quic_frame_type(payload_tvb, pinfo, quic_tree, offset, quic_info);
            }

            /* FNV-1a hash, TODO: Add check and expert info ? */
            proto_tree_add_item(quic_tree, hf_quic_hash, tvb, offset, 8, ENC_NA);
            offset += 8;
        } else { /* Now use Clear Text AEAD */
            proto_item *ti;

            /* quic_decrypt_message expects exactly one header + ciphertext as tvb. */
            DISSECTOR_ASSERT(offset == QUIC_LONG_HEADER_LENGTH);

            ti = proto_tree_add_item(quic_tree, hf_quic_cleartext_protected_payload, tvb, offset, -1, ENC_NA);
            offset += tvb_reported_length_remaining(tvb, offset);

#ifdef HAVE_LIBGCRYPT_AEAD
            tls13_cipher *cipher = NULL;
            const gchar *error = NULL;
            tvbuff_t *decrypted_tvb;

            switch (long_packet_type) {
            case 0x02: /* Client Initial Packet */
                /* Create new decryption context based on the Client Connection
                 * ID from the Client Initial packet. */
                if (!quic_create_cleartext_decoders(cid, &error, quic_info)) {
                    expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Failed to create decryption context: %s", error);
                    return offset;
                }
                /* FALLTHROUGH */
            case 0x05: /* Client Cleartext Packet */
                cipher = quic_info->client_cleartext_cipher;
                break;
            case 0x03: /* Server Stateless Retry Packet */
            case 0x04: /* Server Cleartext Packet */
                cipher = quic_info->server_cleartext_cipher;
                break;
            }

            if (cipher) {
                decrypted_tvb = quic_decrypt_message(cipher, tvb, pinfo, QUIC_LONG_HEADER_LENGTH, pkn, &error);
                if (decrypted_tvb) {
                    guint decrypted_offset = 0;
                    while (tvb_reported_length_remaining(decrypted_tvb, decrypted_offset) > 0){
                        decrypted_offset = dissect_quic_frame_type(decrypted_tvb, pinfo, quic_tree, decrypted_offset, quic_info);
                    }
                } else {
                    expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Failed to decrypt handshake: %s", error);
                }
            }
#else /* !HAVE_LIBGCRYPT_AEAD */
            expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Libgcrypt >= 1.6.0 is required for QUIC decryption");
#endif /* !HAVE_LIBGCRYPT_AEAD */
        }
    /* Initial (>= draft-08) */
    } else if (long_packet_type == 0x7F) {
        proto_item *ti;

        ti = proto_tree_add_item(quic_tree, hf_quic_initial_payload, tvb, offset, -1, ENC_NA);

        /* Initial Packet is always send by client */
        if(pinfo->destport != 443) {
            quic_info->server_port = pinfo->destport;
        }

#ifdef HAVE_LIBGCRYPT_AEAD
        tls13_cipher *cipher = NULL;
        const gchar *error = NULL;
        tvbuff_t *decrypted_tvb;

        cipher = quic_info->client_cleartext_cipher;

        /* Create new decryption context based on the Client Connection
         * ID from the Client Initial packet. */
        if (!quic_create_cleartext_decoders(cid, &error, quic_info)) {
            expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Failed to create decryption context: %s", error);
             return offset;
        }

        if (cipher) {
            /* quic_decrypt_message expects exactly one header + ciphertext as tvb. */
            DISSECTOR_ASSERT(offset == QUIC_LONG_HEADER_LENGTH);

            decrypted_tvb = quic_decrypt_message(cipher, tvb, pinfo, QUIC_LONG_HEADER_LENGTH, pkn, &error);
            if (decrypted_tvb) {
                guint decrypted_offset = 0;
                while (tvb_reported_length_remaining(decrypted_tvb, decrypted_offset) > 0){
                    decrypted_offset = dissect_quic_frame_type(decrypted_tvb, pinfo, quic_tree, decrypted_offset, quic_info);
                }
            } else {
                expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Failed to decrypt handshake: %s", error);
            }
        }
#else /* !HAVE_LIBGCRYPT_AEAD */
            expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Libgcrypt >= 1.6.0 is required for QUIC decryption");
#endif /* !HAVE_LIBGCRYPT_AEAD */
        offset += tvb_reported_length_remaining(tvb, offset);

    /* Handshake (>= draft-08) */
    } else if  (long_packet_type == 0x7D ) {
        proto_item *ti;

        ti = proto_tree_add_item(quic_tree, hf_quic_handshake_payload, tvb, offset, -1, ENC_NA);

#ifdef HAVE_LIBGCRYPT_AEAD
        tls13_cipher *cipher = NULL;
        const gchar *error = NULL;
        tvbuff_t *decrypted_tvb;

        if(pinfo->destport == quic_info->server_port) {
            cipher = quic_info->client_cleartext_cipher;
        } else {
            cipher = quic_info->server_cleartext_cipher;
        }

        if (cipher) {
            /* quic_decrypt_message expects exactly one header + ciphertext as tvb. */
            DISSECTOR_ASSERT(offset == QUIC_LONG_HEADER_LENGTH);

            decrypted_tvb = quic_decrypt_message(cipher, tvb, pinfo, QUIC_LONG_HEADER_LENGTH, pkn, &error);
            if (decrypted_tvb) {
                guint decrypted_offset = 0;
                while (tvb_reported_length_remaining(decrypted_tvb, decrypted_offset) > 0){
                    decrypted_offset = dissect_quic_frame_type(decrypted_tvb, pinfo, quic_tree, decrypted_offset, quic_info);
                }
            } else {
                expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Failed to decrypt handshake: %s", error);
            }
        }
#else /* !HAVE_LIBGCRYPT_AEAD */
        expert_add_info_format(pinfo, ti, &ei_quic_decryption_failed, "Libgcrypt >= 1.6.0 is required for QUIC decryption");
#endif /* !HAVE_LIBGCRYPT_AEAD */
        offset += tvb_reported_length_remaining(tvb, offset);

    } else {
        /* 0-RTT (0x06)/ 1-RTT Key Phase 0 (0x07), 1-RTT Key Phase 1 (0x08) Protected Payload */
        proto_tree_add_item(quic_tree, hf_quic_protected_payload, tvb, offset, -1, ENC_NA);
        offset += tvb_reported_length_remaining(tvb, offset);

    }

    return offset;
}

static int
dissect_quic_short_header(tvbuff_t *tvb, packet_info *pinfo, proto_tree *quic_tree, guint offset, quic_info_data_t *quic_info _U_){
    guint8 short_flags;
    guint64 cid = 0;
    guint32 pkn_len, pkn;

    short_flags = tvb_get_guint8(tvb, offset);
    proto_tree_add_item(quic_tree, hf_quic_short_cid_flag, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(quic_tree, hf_quic_short_kp_flag, tvb, offset, 1, ENC_NA);
    proto_tree_add_item(quic_tree, hf_quic_short_packet_type, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* Connection ID */
    if (short_flags & SH_CID){
        proto_tree_add_item_ret_uint64(quic_tree, hf_quic_connection_id, tvb, offset, 8, ENC_BIG_ENDIAN, &cid);
        offset += 8;
    }
    /* Packet Number */
    pkn_len = get_len_packet_number(short_flags);
    proto_tree_add_item_ret_uint(quic_tree, hf_quic_packet_number, tvb, offset, pkn_len, ENC_BIG_ENDIAN, &pkn);
    offset += pkn_len;

    /* Protected Payload */
    proto_tree_add_item(quic_tree, hf_quic_protected_payload, tvb, offset, -1, ENC_NA);
    offset += tvb_reported_length_remaining(tvb, offset);

    col_append_fstr(pinfo->cinfo, COL_INFO, "Protected Payload (KP%u), PKN: %u", short_flags & SH_KP, pkn);

    if(cid){
        col_append_fstr(pinfo->cinfo, COL_INFO, ", CID: %" G_GINT64_MODIFIER "u", cid);
    }

    return offset;
}


static int
dissect_quic(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
        void *data _U_)
{
    proto_item *ti;
    proto_tree *quic_tree;
    guint       offset = 0;
    guint32     header_form;
    conversation_t  *conv;
    quic_info_data_t  *quic_info;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "QUIC");

    /* get conversation, create if necessary*/
    conv = find_or_create_conversation(pinfo);

    /* get associated state information, create if necessary */
    quic_info = (quic_info_data_t *)conversation_get_proto_data(conv, proto_quic);

    if (!quic_info) {
        quic_info = wmem_new0(wmem_file_scope(), quic_info_data_t);
        quic_info->version = 0;
        quic_info->server_port = 443;
        conversation_add_proto_data(conv, proto_quic, quic_info);
    }

    ti = proto_tree_add_item(tree, proto_quic, tvb, 0, -1, ENC_NA);

    quic_tree = proto_item_add_subtree(ti, ett_quic);

    proto_tree_add_item_ret_uint(quic_tree, hf_quic_header_form, tvb, offset, 1, ENC_NA, &header_form);
    if(header_form) {
        col_set_str(pinfo->cinfo, COL_INFO, "LH, ");
        offset = dissect_quic_long_header(tvb, pinfo, quic_tree, offset, quic_info);
    } else {
        col_set_str(pinfo->cinfo, COL_INFO, "SH, ");
        offset = dissect_quic_short_header(tvb, pinfo, quic_tree, offset, quic_info);
    }

    return offset;
}


void
proto_register_quic(void)
{
    expert_module_t *expert_quic;

    static hf_register_info hf[] = {
        { &hf_quic_header_form,
          { "Header Form", "quic.header_form",
            FT_UINT8, BASE_DEC, VALS(quic_short_long_header_vals), 0x80,
            "The most significant bit (0x80) of the first octet is set to 1 for long headers and 0 for short headers.", HFILL }
        },

        { &hf_quic_long_packet_type,
          { "Packet Type", "quic.long.packet_type",
            FT_UINT8, BASE_DEC, VALS(quic_long_packet_type_vals), 0x7F,
            "Long Header Packet Type", HFILL }
        },
        { &hf_quic_connection_id,
          { "Connection ID", "quic.connection_id",
            FT_UINT64, BASE_HEX, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_packet_number,
          { "Packet Number", "quic.packet_number",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_version,
          { "Version", "quic.version",
            FT_UINT32, BASE_HEX, VALS(quic_version_vals), 0x0,
            NULL, HFILL }
        },
        { &hf_quic_short_cid_flag,
          { "Connection ID Flag", "quic.short.cid_flag",
            FT_BOOLEAN, 8, NULL, SH_CID,
            NULL, HFILL }
        },
        { &hf_quic_short_kp_flag,
          { "Key Phase Bit", "quic.short.kp_flag",
            FT_BOOLEAN, 8, NULL, SH_KP,
            NULL, HFILL }
        },
        { &hf_quic_short_packet_type,
          { "Packet Type", "quic.short.packet_type",
            FT_UINT8, BASE_DEC, VALS(quic_short_packet_type_vals), SH_PT,
            "Short Header Packet Type", HFILL }
        },
        { &hf_quic_cleartext_protected_payload,
          { "Cleartext Protected Payload", "quic.cleartext_protected_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_initial_payload,
          { "Initial Payload", "quic.initial_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_handshake_payload,
          { "Handshake Payload", "quic.handshake_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

        { &hf_quic_protected_payload,
          { "Protected Payload", "quic.protected_payload",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

        { &hf_quic_frame,
          { "Frame", "quic.frame",
            FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_frame_type,
          { "Frame Type", "quic.frame_type",
            FT_UINT8, BASE_RANGE_STRING | BASE_HEX, RVALS(quic_frame_type_vals), 0x0,
            NULL, HFILL }
        },

        /* >= draft-07 */
        { &hf_quic_frame_type_stream,
          { "Stream", "quic.frame_type.stream",
            FT_UINT8, BASE_HEX, NULL, FTFLAGS_STREAM_STREAM,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_f,
          { "Fin(F)", "quic.frame_type.stream.f",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_F,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_ss,
          { "Stream Length (SS)", "quic.frame_type.stream.ss",
            FT_UINT8, BASE_DEC, VALS(len_stream_vals), FTFLAGS_STREAM_SS,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_oo,
          { "Offset Length (OO)", "quic.frame_type.stream.oo",
            FT_UINT8, BASE_DEC, VALS(len_offset_vals), FTFLAGS_STREAM_OO,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_d,
          { "Data Length (D)", "quic.frame_type.stream.d",
            FT_BOOLEAN, 8, TFS(&len_data_vals), FTFLAGS_STREAM_D,
            NULL, HFILL }
        },

        /* >= draft-08*/
        { &hf_quic_frame_type_stream_fin,
          { "Fin", "quic.frame_type.stream.fin",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_FIN,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_len,
          { "Len(gth)", "quic.frame_type.stream.len",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_LEN,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_stream_off,
          { "Off(set)", "quic.frame_type.stream.off",
            FT_BOOLEAN, 8, NULL, FTFLAGS_STREAM_OFF,
            NULL, HFILL }
        },

        { &hf_quic_stream_stream_id,
          { "Stream ID", "quic.stream.stream_id",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_offset,
          { "Offset", "quic.stream.offset",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_data_len,
          { "Data Length", "quic.stream.data_len",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_length,
          { "Length", "quic.stream.length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_stream_data,
          { "Stream Data", "quic.stream_data",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

        { &hf_quic_frame_type_ack,
          { "ACK", "quic.frame_type.ack",
            FT_UINT8, BASE_HEX, NULL, FTFLAGS_ACK_ACK,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_ack_n,
          { "NACK", "quic.frame_type.ack.n",
            FT_BOOLEAN, 8, NULL, FTFLAGS_ACK_N,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_ack_ll,
          { "Largest Acknowledged Length", "quic.frame_type.ack.ll",
            FT_UINT8, BASE_DEC, VALS(len_largest_acknowledged_vals), FTFLAGS_ACK_LL,
            "Length of the Largest Observed field as 1, 2, 4, or 8 bytes long", HFILL }
        },
        { &hf_quic_frame_type_ack_mm,
          { "ACK Block Length", "quic.frame_type.ack.mm",
            FT_UINT8, BASE_DEC, VALS(len_ack_block_vals), FTFLAGS_ACK_MM,
            "Length of the ACK Block Length field as 1, 2, 4, or 8 bytes long", HFILL }
        },
        { &hf_quic_frame_type_ack_num_blocks,
          { "Num blocks", "quic.frame_type.ack.num_blocks",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "Specifying the number of additional ACK blocks (besides the required First ACK Block)", HFILL }
        },
        { &hf_quic_frame_type_ack_num_ts,
          { "Num Timestamps", "quic.frame_type.ack.num_ts",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "Specifying the total number of <packet number, timestamp> pairs in the Timestamp Section", HFILL }
        },
        { &hf_quic_frame_type_ack_largest_acknowledged,
          { "Largest Acknowledged", "quic.frame_type.ack.largest_acknowledged",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Representing the largest packet number the peer is acknowledging in this packet", HFILL }
        },
        { &hf_quic_frame_type_ack_ack_delay,
          { "Ack Delay", "quic.frame_type.ack.ack_delay",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "The time from when the largest acknowledged packet, as indicated in the Largest Acknowledged field, was received by this peer to when this ACK was sent", HFILL }
        },
        { &hf_quic_frame_type_ack_fabl,
          { "First ACK Block Length", "quic.frame_type.ack.fabl",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Indicates the number of contiguous additional packets being acknowledged starting at the Largest Acknowledged", HFILL }
        },
        { &hf_quic_frame_type_ack_gap2nb,
          { "Gap To Next Block", "quic.frame_type.ack.gap2nb",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "Specifying the number of contiguous missing packets from the end of the previous ACK block to the start of the next", HFILL }
        },
        { &hf_quic_frame_type_ack_ack_block_length,
          { "ACK Block Length", "quic.frame_type.ack.ack_block_length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Indicates the number of contiguous packets being acknowledged starting after the end of the previous gap", HFILL }
        },
        { &hf_quic_frame_type_ack_dla,
          { "Delta Largest Acknowledged", "quic.frame_type.ack.dla",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "Specifying the delta between the largest acknowledged and the first packet whose timestamp is being reported", HFILL }
        },
        { &hf_quic_frame_type_ack_ft,
          { "First Timestamp", "quic.frame_type.ack.ft",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Specifying the time delta in microseconds, from the beginning of the connection to the arrival of the packet indicated by Delta Largest Acknowledged", HFILL }
        },
        { &hf_quic_frame_type_ack_tspt,
          { "Time Since Previous Timestamp", "quic.frame_type.ack.tspt",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "Specifying time delta from the previous reported timestamp. It is encoded in the same format as the ACK Delay", HFILL }
        },
        /* PADDING */
        { &hf_quic_frame_type_padding_length,
          { "Padding Length", "quic.frame_type.padding.length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_quic_frame_type_padding,
          { "Padding", "quic.frame_type.padding",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            "Must be zero", HFILL }
        },
        /* RST_STREAM */
        { &hf_quic_frame_type_rsts_stream_id,
            { "Stream ID", "quic.frame_type.rsts.stream_id",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "Stream ID of the stream being terminated", HFILL }
        },
        { &hf_quic_frame_type_rsts_error_code,
            { "Error code", "quic.frame_type.rsts.error_code",
              FT_UINT32, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates why the stream is being closed", HFILL }
        },
        { &hf_quic_frame_type_rsts_application_error_code,
            { "Application Error code", "quic.frame_type.rsts.application_error_code",
              FT_UINT16, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates why the stream is being closed", HFILL }
        },
        { &hf_quic_frame_type_rsts_final_offset,
            { "Final offset", "quic.frame_type.rsts.byte_offset",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the absolute byte offset of the end of data written on this stream", HFILL }
        },
        /* CONNECTION_CLOSE */
        { &hf_quic_frame_type_cc_old_error_code, /* draft05-06 */
            { "Error code", "quic.frame_type.cc.error_code",
              FT_UINT32, BASE_DEC|BASE_EXT_STRING, &quic_old_error_code_vals_ext, 0x0,
              "Indicates the reason for closing this connection", HFILL }
        },
        { &hf_quic_frame_type_cc_error_code, /* >= draft07 */
            { "Error code", "quic.frame_type.cc.error_code",
              FT_UINT16, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates the reason for closing this connection", HFILL }
        },
        { &hf_quic_frame_type_cc_reason_phrase_length,
            { "Reason phrase Length", "quic.frame_type.cc.reason_phrase.length",
              FT_UINT16, BASE_DEC, NULL, 0x0,
              "Specifying the length of the reason phrase", HFILL }
        },
        { &hf_quic_frame_type_cc_reason_phrase,
            { "Reason phrase", "quic.frame_type.cc.reason_phrase",
              FT_STRING, BASE_NONE, NULL, 0x0,
              "A human-readable explanation for why the connection was closed", HFILL }
        },
        /* APPLICATION_CLOSE */
        { &hf_quic_frame_type_ac_error_code,
            { "Application Error code", "quic.frame_type.ac.error_code",
              FT_UINT16, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates the reason for closing this application", HFILL }
        },
        { &hf_quic_frame_type_ac_reason_phrase_length,
            { "Reason phrase Length", "quic.frame_type.ac.reason_phrase.length",
              FT_UINT16, BASE_DEC, NULL, 0x0,
              "Specifying the length of the reason phrase", HFILL }
        },
        { &hf_quic_frame_type_ac_reason_phrase,
            { "Reason phrase", "quic.frame_type.ac.reason_phrase",
              FT_STRING, BASE_NONE, NULL, 0x0,
              "A human-readable explanation for why the application was closed", HFILL }
        },
        /* MAX_DATA */
        { &hf_quic_frame_type_md_maximum_data,
            { "Maximum Data", "quic.frame_type.md.maximum_data",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the maximum amount of data that can be sent on the entire connection, in units of 1024 octets", HFILL }
        },
        /* MAX_STREAM_DATA */
        { &hf_quic_frame_type_msd_stream_id,
            { "Stream ID", "quic.frame_type.msd.stream_id",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "The stream ID of the stream that is affected", HFILL }
        },
        { &hf_quic_frame_type_msd_maximum_stream_data,
            { "Maximum Stream Data", "quic.frame_type.msd.maximum_stream_data",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              "Indicating the maximum amount of data that can be sent on the identified stream, in units of octets", HFILL }
        },
        /* MAX_STREAM_ID */
        { &hf_quic_frame_type_msi_stream_id,
            { "Stream ID", "quic.frame_type.msi.stream_id",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "ID of the maximum peer-initiated stream ID for the connection", HFILL }
        },
        /* STREAM_BLOCKED */
        { &hf_quic_frame_type_sb_stream_id,
            { "Stream ID", "quic.frame_type.sb.stream_id",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "Indicating the stream which is flow control blocked", HFILL }
        },
        /* NEW_CONNECTION_ID */
        { &hf_quic_frame_type_nci_sequence,
            { "Sequence", "quic.frame_type.nci.sequence",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "Increases by 1 for each connection ID that is provided by the server", HFILL }
        },
        { &hf_quic_frame_type_nci_connection_id,
            { "Connection ID", "quic.frame_type.nci.connection_id",
              FT_UINT64, BASE_DEC, NULL, 0x0,
              NULL, HFILL }
        },
        { &hf_quic_frame_type_nci_stateless_reset_token,
            { "Stateless Reset Token", "quic.frame_type.stateless_reset_token",
              FT_BYTES, BASE_NONE, NULL, 0x0,
              NULL, HFILL }
        },
        /* STOP_SENDING */
        { &hf_quic_frame_type_ss_stream_id,
            { "Stream ID", "quic.frame_type.ss.stream_id",
              FT_UINT32, BASE_DEC, NULL, 0x0,
              "Stream ID of the stream being ignored", HFILL }
        },
        { &hf_quic_frame_type_ss_error_code,
            { "Error code", "quic.frame_type.ss.error_code",
              FT_UINT32, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates why the sender is ignoring the stream", HFILL }
        },
        { &hf_quic_frame_type_ss_application_error_code,
            { "Application Error code", "quic.frame_type.ss.application_error_code",
              FT_UINT16, BASE_DEC|BASE_EXT_STRING, &quic_error_code_vals_ext, 0x0,
              "Indicates why the sender is ignoring the stream", HFILL }
        },

        { &hf_quic_hash,
          { "Hash", "quic.hash",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL }
        },

    };

    static gint *ett[] = {
        &ett_quic,
        &ett_quic_ft,
        &ett_quic_ftflags
    };

    static ei_register_info ei[] = {
        { &ei_quic_ft_unknown,
          { "quic.ft.unknown", PI_UNDECODED, PI_NOTE,
            "Unknown Frame Type", EXPFILL }
        },
        { &ei_quic_decryption_failed,
          { "quic.decryption_failed", PI_DECRYPTION, PI_WARN,
            "Failed to decrypt handshake", EXPFILL }
        },
    };

    proto_quic = proto_register_protocol("QUIC (Quick UDP Internet Connections) IETF", "QUIC", "quic");

    proto_register_field_array(proto_quic, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_quic = expert_register_protocol(proto_quic);
    expert_register_field_array(expert_quic, ei, array_length(ei));

    quic_handle = register_dissector("quic", dissect_quic, proto_quic);

}

void
proto_reg_handoff_quic(void)
{
    ssl_handle = find_dissector("ssl");
    dissector_add_uint_with_preference("udp.port", 443, quic_handle);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
