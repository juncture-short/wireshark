/* packet-goose.c
 * Routines for IEC 61850 GOOSE packet dissection
 * Martin Lutz 2008
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/asn1.h>
#include <epan/etypes.h>
#include <epan/expert.h>

#include "packet-ber.h"
#include "packet-acse.h"

#define PNAME  "GOOSE"
#define PSNAME "GOOSE"
#define PFNAME "goose"

void proto_register_goose(void);
void proto_reg_handoff_goose(void);

/* Initialize the protocol and registered fields */
static int proto_goose = -1;

static int hf_goose_session_header = -1;
static int hf_goose_spdu_id = -1;
static int hf_goose_session_hdr_length = -1;
static int hf_goose_hdr_length = -1;
static int hf_goose_content_id = -1;
static int hf_goose_spdu_lenth = -1;
static int hf_goose_spdu_num = -1;
static int hf_goose_version = -1;
static int hf_goose_security_info = -1;
static int hf_goose_current_key_t = -1;
static int hf_goose_next_key_t = -1;
static int hf_goose_key_id = -1;
static int hf_goose_init_vec_length = -1;
static int hf_goose_init_vec = -1;
static int hf_goose_session_user_info = -1;
static int hf_goose_payload = -1;
static int hf_goose_payload_length = -1;
static int hf_goose_apdu_tag = -1;
static int hf_goose_apdu_simulation = -1;
static int hf_goose_apdu_appid = -1;
static int hf_goose_apdu_length = -1;
static int hf_goose_padding_tag = -1;
static int hf_goose_padding_length = -1;
static int hf_goose_padding = -1;
static int hf_goose_hmac = -1;
static int hf_goose_appid = -1;
static int hf_goose_length = -1;
static int hf_goose_reserve1 = -1;
static int hf_goose_reserve2 = -1;

static expert_field ei_goose_mal_utctime = EI_INIT;
static expert_field ei_goose_zero_pdu = EI_INIT;

#include "packet-goose-hf.c"

/* Initialize the subtree pointers */
static int ett_session_header = -1;
static int ett_security_info = -1;
static int ett_session_user_info = -1;
static int ett_payload = -1;
static int ett_padding = -1;
static int ett_goose = -1;

#include "packet-goose-ett.c"

#include "packet-goose-fn.c"

static dissector_handle_t goose_handle = NULL;
static dissector_handle_t ositp_handle = NULL;


#define OSI_SPDU_TUNNELED 0xA0 /* Tunneled */
#define OSI_SPDU_GOOSE    0xA1 /* GOOSE */
#define OSI_SPDU_SV       0xA2 /* Sample Value */
#define OSI_SPDU_MNGT     0xA3 /* Management */

static const value_string ositp_spdu_id[] = {
	{ OSI_SPDU_TUNNELED, "Tunneled" },
	{ OSI_SPDU_GOOSE,    "GOOSE" },
	{ OSI_SPDU_SV,       "Sample value" },
	{ OSI_SPDU_MNGT,     "Management" },
	{ 0,       NULL }
};

#define OSI_PDU_GOOSE     0x81
#define OSI_PDU_SV        0x82
#define OSI_PDU_TUNNELED  0x83
#define OSI_PDU_MNGT      0x84

static const value_string ositp_pdu_id[] = {
	{ OSI_PDU_GOOSE,     "GOOSE" },
	{ OSI_PDU_SV,        "SV" },
	{ OSI_PDU_TUNNELED,  "Tunnel" },
	{ OSI_PDU_MNGT,      "MNGT" },
	{ 0,       NULL }
};

#define APDU_HEADER_SIZE 6

/*
* Dissect GOOSE PDUs inside a PPDU.
*/
static int
dissect_goose(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree,
			  void* data _U_)
{
	guint32 offset = 0;
	guint32 old_offset;
	guint32 length;
	proto_item *item = NULL;
	proto_tree *tree = NULL;
	asn1_ctx_t asn1_ctx;
	asn1_ctx_init(&asn1_ctx, ASN1_ENC_BER, TRUE, pinfo);

	col_set_str(pinfo->cinfo, COL_PROTOCOL, PNAME);
	col_clear(pinfo->cinfo, COL_INFO);

	item = proto_tree_add_item(parent_tree, proto_goose, tvb, 0, -1, ENC_NA);
	tree = proto_item_add_subtree(item, ett_goose);


	/* APPID */
	proto_tree_add_item(tree, hf_goose_appid, tvb, offset, 2, ENC_BIG_ENDIAN);

	/* Length */
	proto_tree_add_item_ret_uint(tree, hf_goose_length, tvb, offset + 2, 2,
						ENC_BIG_ENDIAN, &length);

	/* Reserved 1 */
	proto_tree_add_item(tree, hf_goose_reserve1, tvb, offset + 4, 2,
						ENC_BIG_ENDIAN);

	/* Reserved 2 */
	proto_tree_add_item(tree, hf_goose_reserve2, tvb, offset + 6, 2,
						ENC_BIG_ENDIAN);

	offset = 8;
	while (offset < length){
		old_offset = offset;
		offset = dissect_goose_GOOSEpdu(FALSE, tvb, offset, &asn1_ctx , tree, -1);
		if (offset == old_offset) {
			proto_tree_add_expert(tree, pinfo, &ei_goose_zero_pdu, tvb, offset, -1);
			break;
		}
	}

	return tvb_captured_length(tvb);
}

/*
* Dissect RGOOSE PDUs inside ISO 8602/X.234 CLTP ConnecteionLess
* Transport Protocol.
*/
static int
dissect_rgoose(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree,
			   void* data _U_)
{
	guint offset = 0, old_offset = 0;
	guint32 init_v_length, payload_tag, padding_length;
	guint32 payload_length, apdu_offset = 0, apdu_length;
	proto_item *item = NULL;
	proto_tree *tree = NULL, *sess_user_info_tree = NULL;
	asn1_ctx_t asn1_ctx;
	asn1_ctx_init(&asn1_ctx, ASN1_ENC_BER, TRUE, pinfo);

	col_set_str(pinfo->cinfo, COL_PROTOCOL, "R-GOOSE");
	col_clear(pinfo->cinfo, COL_INFO);

	/* Session header subtree */
	item = proto_tree_add_item(parent_tree, hf_goose_session_header, tvb, 0,
							   -1, ENC_NA);
	tree = proto_item_add_subtree(item, ett_session_header);

	/* SPDU ID */
	proto_tree_add_item(tree, hf_goose_spdu_id, tvb, offset++, 1,
						ENC_BIG_ENDIAN);
	/* Session header length */
	proto_tree_add_item(tree, hf_goose_session_hdr_length, tvb, offset++, 1,
						ENC_BIG_ENDIAN);
	/* Header content indicator */
	proto_tree_add_item(tree, hf_goose_content_id, tvb, offset++, 1,
						ENC_BIG_ENDIAN);
	/* Length */
	proto_tree_add_item(tree, hf_goose_hdr_length, tvb, offset++, 1,
						ENC_BIG_ENDIAN);
	/* SPDU length */
	proto_tree_add_item(tree, hf_goose_spdu_lenth, tvb, offset, 4,
						ENC_BIG_ENDIAN);
	offset += 4;
	/* SPDU number */
	proto_tree_add_item(tree, hf_goose_spdu_num, tvb, offset, 4,
						ENC_BIG_ENDIAN);
	offset += 4;
	/* Version */
	proto_tree_add_item(tree, hf_goose_version, tvb, offset, 2, ENC_BIG_ENDIAN);
	offset += 2;

	/* Security information subtree */
	item = proto_tree_add_item(tree, hf_goose_security_info, tvb, offset, -1,
							   ENC_NA);
	tree = proto_item_add_subtree(item, ett_security_info);
	/* Time of current key */
	proto_tree_add_item(tree, hf_goose_current_key_t, tvb, offset, 4,
						ENC_BIG_ENDIAN);
	offset += 4;
	/* Time of next key */
	proto_tree_add_item(tree, hf_goose_next_key_t, tvb, offset, 2,
						ENC_BIG_ENDIAN);
	offset += 2;
	/* Key ID */
	proto_tree_add_item(tree, hf_goose_key_id, tvb, offset, 4, ENC_BIG_ENDIAN);
	offset += 4;
	/* Initialization vector length */
	proto_tree_add_item_ret_uint(tree, hf_goose_init_vec_length, tvb, offset++, 1,
						ENC_BIG_ENDIAN, &init_v_length);
	if (init_v_length > 0) {
		/* Initialization vector bytes */
		proto_tree_add_item(tree, hf_goose_init_vec, tvb, offset, init_v_length,
							ENC_NA);
	}
	offset += init_v_length;

	/* Session user information subtree */
	item = proto_tree_add_item(parent_tree, hf_goose_session_user_info, tvb,
							   offset, -1, ENC_NA);
	sess_user_info_tree = proto_item_add_subtree(item, ett_payload);

	/* Payload subtree */
	item = proto_tree_add_item(sess_user_info_tree, hf_goose_payload, tvb,
							   offset, -1, ENC_NA);
	tree = proto_item_add_subtree(item, ett_payload);
	/* Payload length */
	proto_tree_add_item_ret_uint(tree, hf_goose_payload_length, tvb, offset, 4,
						ENC_BIG_ENDIAN, &payload_length);
	offset += 4;

	while (apdu_offset < payload_length){
		/* APDU tag */
		proto_tree_add_item_ret_uint(tree, hf_goose_apdu_tag, tvb, offset++, 1,
							ENC_BIG_ENDIAN, &payload_tag);
		/* Simulation flag */
		proto_tree_add_item(tree, hf_goose_apdu_simulation, tvb, offset++, 1,
							ENC_BIG_ENDIAN);
		/* APPID */
		proto_tree_add_item(tree, hf_goose_apdu_appid, tvb, offset, 2,
							ENC_BIG_ENDIAN);
		offset += 2;

		if (payload_tag != OSI_PDU_GOOSE) {
			return tvb_captured_length(tvb);
		}

		/* APDU length */
		proto_tree_add_item_ret_uint(tree, hf_goose_apdu_length, tvb, offset, 2,
							ENC_BIG_ENDIAN, &apdu_length);

		apdu_offset += (APDU_HEADER_SIZE + apdu_length);
		offset += 2;

		old_offset = offset;
		offset = dissect_goose_GOOSEpdu(FALSE, tvb, offset, &asn1_ctx , tree, -1);
		if (offset == old_offset) {
			proto_tree_add_expert(tree, pinfo, &ei_goose_zero_pdu, tvb, offset, -1);
			break;
		}
	}

	/* Check do we have padding bytes */
	if ((tvb_captured_length(tvb) > offset) &&
		(tvb_get_guint8(tvb, offset) == 0xAF)) {
		/* Padding subtree */
		item = proto_tree_add_item(sess_user_info_tree, hf_goose_padding, tvb,
								   offset, -1, ENC_NA);
		tree = proto_item_add_subtree(item, ett_padding);

		/* Padding tag */
		proto_tree_add_item(tree, hf_goose_padding_tag, tvb, offset++, 1,
							ENC_NA);
		/* Padding length */
		proto_tree_add_item_ret_uint(tree, hf_goose_padding_length, tvb, offset++, 1,
							ENC_BIG_ENDIAN, &padding_length);
		/* Padding bytes */
		proto_tree_add_item(tree, hf_goose_padding, tvb, offset, padding_length,
							ENC_NA);
		offset += padding_length;
	}

	/* Check do we have HMAC bytes */
	if (tvb_captured_length(tvb) > offset) {
		/* HMAC bytes */
		proto_tree_add_item(sess_user_info_tree, hf_goose_hmac, tvb, offset,
			tvb_captured_length(tvb) - offset, ENC_NA);
	}

	return tvb_captured_length(tvb);
}

static gboolean
dissect_rgoose_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree,
					void *data)
{
	guint8 spdu;

	/* Check do we have at least min size of Session header bytes */
	if (tvb_captured_length(tvb) < 27) {
		return FALSE;
	}

	/* Is it R-GOOSE? */
	spdu = tvb_get_guint8(tvb, 0);
	if (spdu != OSI_SPDU_GOOSE) {
		return FALSE;
	}

	dissect_rgoose(tvb, pinfo, parent_tree, data);
	return TRUE;
}

static gboolean
dissect_cltp_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree,
				  void *data _U_)
{
	guint8 li, tpdu, spdu;

	/* First, check do we have at least 2 bytes (length + tpdu) */
	if (tvb_captured_length(tvb) < 2) {
		return FALSE;
	}

	li = tvb_get_guint8(tvb, 0);

	/* Is it OSI on top of the UDP? */
	tpdu = (tvb_get_guint8(tvb, 1) & 0xF0) >> 4;
	if (tpdu != 0x4) {
		return FALSE;
	}

	/* Check do we have SPDU ID byte, too */
	if (tvb_captured_length(tvb) < (guint) (li + 2)) {
		return FALSE;
	}

	/* And let's see if it is GOOSE SPDU */
	spdu = tvb_get_guint8(tvb, li + 1);
	if (spdu != OSI_SPDU_GOOSE) {
		return FALSE;
	}

	call_dissector(ositp_handle, tvb, pinfo, parent_tree);
	return TRUE;
}


/*--- proto_register_goose -------------------------------------------*/
void proto_register_goose(void) {

	/* List of fields */
	static hf_register_info hf[] =
	{
		{ &hf_goose_session_header,
		{ "Session header", "rgoose.session_hdr",
		  FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_spdu_id,
		{ "Session identifier", "rgoose.spdu_id",
		  FT_UINT8, BASE_HEX_DEC, VALS(ositp_spdu_id), 0x0, NULL, HFILL }},

		{ &hf_goose_session_hdr_length,
		{ "Session header length", "rgoose.session_hdr_len",
		  FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_content_id,
		{ "Common session header identifier", "rgoose.common_session_id",
		  FT_UINT8, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_hdr_length,
		{ "Header length", "rgoose.hdr_len",
		  FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_spdu_lenth,
		{ "SPDU length", "rgoose.spdu_len",
		  FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_spdu_num,
		{ "SPDU number", "rgoose.spdu_num",
		  FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_version,
		{ "Version", "rgoose.version",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_security_info,
		{ "Security information", "rgoose.sec_info",
		  FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_current_key_t,
		{ "Time of current key", "rgoose.curr_key_t",
		   FT_UINT32, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_next_key_t,
		{ "Time of next key", "rgoose.next_key_t",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_key_id,
		{ "Key ID", "rgoose.key_id",
		  FT_UINT32, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_init_vec_length,
		{ "Initialization vector length", "rgoose.init_v_len",
		  FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_init_vec,
		{ "Initialization vector", "rgoose.init_v",
		  FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_session_user_info,
		{ "Session user information", "rgoose.session_user_info",
		  FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_payload,
		{ "Payload", "rgoose.payload",
		  FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_payload_length,
		{ "Payload length", "rgoose.payload_len",
		  FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_apdu_tag,
		{ "Payload type tag", "rgoose.pdu_tag",
		  FT_UINT8, BASE_HEX_DEC, VALS(ositp_pdu_id), 0x0, NULL, HFILL }},

		{ &hf_goose_apdu_simulation,
		{ "Simulation flag", "rgoose.simulation",
		  FT_UINT8, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_apdu_appid,
		{ "APPID", "rgoose.appid",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_apdu_length,
		{ "APDU length", "rgoose.apdu_len",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_padding_tag,
		{ "Padding", "rgoose.padding_tag",
		  FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_padding_length,
		{ "Padding length", "rgoose.padding_len",
		  FT_UINT8, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_padding,
		{ "Padding", "rgoose.padding",
		  FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_hmac,
		{ "HMAC", "rgoose.hmac",
		  FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_appid,
		{ "APPID", "goose.appid",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_length,
		{ "Length", "goose.length",
		  FT_UINT16, BASE_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_reserve1,
		{ "Reserved 1", "goose.reserve1",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		{ &hf_goose_reserve2,
		{ "Reserved 2", "goose.reserve2",
		  FT_UINT16, BASE_HEX_DEC, NULL, 0x0, NULL, HFILL }},

		#include "packet-goose-hfarr.c"
	};

	/* List of subtrees */
	static gint *ett[] = {
		&ett_session_header,
		&ett_security_info,
		&ett_session_user_info,
		&ett_payload,
		&ett_padding,
		&ett_goose,
		#include "packet-goose-ettarr.c"
	};

	static ei_register_info ei[] = {
		{ &ei_goose_mal_utctime,
		{ "goose.malformed.utctime", PI_MALFORMED, PI_WARN,
		  "BER Error: malformed UTCTime encoding", EXPFILL }},
		{ &ei_goose_zero_pdu,
		{ "goose.zero_pdu", PI_PROTOCOL, PI_ERROR,
		  "Internal error, zero-byte GOOSE PDU", EXPFILL }},
	};

	expert_module_t* expert_goose;

	/* Register protocol */
	proto_goose = proto_register_protocol(PNAME, PSNAME, PFNAME);
	goose_handle = register_dissector("goose", dissect_goose, proto_goose);

	/* Register fields and subtrees */
	proto_register_field_array(proto_goose, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
	expert_goose = expert_register_protocol(proto_goose);
	expert_register_field_array(expert_goose, ei, array_length(ei));
}

/*--- proto_reg_handoff_goose --- */
void proto_reg_handoff_goose(void) {

	dissector_add_uint("ethertype", ETHERTYPE_IEC61850_GOOSE, goose_handle);

	ositp_handle = find_dissector_add_dependency("ositp", proto_goose);

	heur_dissector_add("udp", dissect_cltp_heur,
		"CLTP over UDP", "cltp_udp", proto_goose, HEURISTIC_ENABLE);
	heur_dissector_add("cltp", dissect_rgoose_heur,
		"R-GOOSE (GOOSE over CLTP)", "rgoose_cltp", proto_goose, HEURISTIC_ENABLE);
}
