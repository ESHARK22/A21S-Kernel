/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Samsung SoC DisplayPort EDID driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/fb.h>
#include "displayport.h"

#define EDID_SEGMENT_ADDR	(0x60 >> 1)
#define EDID_ADDR		(0xA0 >> 1)
#define EDID_SEGMENT_IGNORE	(2)
#define EDID_BLOCK_SIZE		128
#define EDID_SEGMENT(x)		((x) >> 1)
#define EDID_OFFSET(x)		(((x) & 1) * EDID_BLOCK_SIZE)
#define EDID_EXTENSION_FLAG	0x7E
#define EDID_NATIVE_FORMAT	0x83
#define EDID_BASIC_AUDIO	(1 << 6)
#define EDID_COLOR_DEPTH	0x14

#define DETAILED_TIMING_DESCRIPTIONS_START	0x36

int forced_resolution = -1;

videoformat ud_mode_h14b_vsdb[] = {
	V3840X2160P30,
	V3840X2160P25,
	V3840X2160P24,
	V4096X2160P24
};

static struct v4l2_dv_timings preferred_preset = V4L2_DV_BT_DMT_640X480P60;
static u32 edid_misc;
static int audio_channels;
static int audio_bit_rates;
static int audio_sample_rates;
static int audio_speaker_alloc;

struct fb_audio test_audio_info;

void edid_check_set_i2c_capabilities(void)
{
	u8 val[1];

	displayport_reg_dpcd_read(DPCD_ADD_I2C_SPEED_CONTROL_CAPABILITES, 1, val);
	displayport_info("DPCD_ADD_I2C_SPEED_CONTROL_CAPABILITES = 0x%x\n", val[0]);

	if (val[0] != 0) {
		if (val[0] & I2C_1Mbps)
			val[0] = I2C_1Mbps;
		else if (val[0] & I2C_400Kbps)
			val[0] = I2C_400Kbps;
		else if (val[0] & I2C_100Kbps)
			val[0] = I2C_100Kbps;
		else if (val[0] & I2C_10Kbps)
			val[0] = I2C_10Kbps;
		else if (val[0] & I2C_1Kbps)
			val[0] = I2C_1Kbps;
		else
			val[0] = I2C_400Kbps;

		displayport_reg_dpcd_write(DPCD_ADD_I2C_SPEED_CONTROL_STATUS, 1, val);
		displayport_dbg("DPCD_ADD_I2C_SPEED_CONTROL_STATUS = 0x%x\n", val[0]);
	}
}

static int edid_checksum(u8 *data, int block)
{
	int i;
	u8 sum = 0, all_null = 0;

	for (i = 0; i < EDID_BLOCK_SIZE; i++) {
		sum += data[i];
		all_null |= data[i];
	}

	if (sum || all_null == 0x0) {
		displayport_err("checksum error block = %d sum = %02x\n", block, sum);
		return -EPROTO;
	}

	return 0;
}

static int edid_read_block(u32 sst_id, struct displayport_device *displayport,
		int block, u8 *buf, size_t len)
{
	int ret = 0;
	u8 offset = EDID_OFFSET(block);

	if (len < EDID_BLOCK_SIZE)
		return -EINVAL;

	edid_check_set_i2c_capabilities();

	if (displayport->mst_cap == 0) {
		ret = displayport_reg_edid_read(offset, EDID_BLOCK_SIZE, buf);
		if (ret)
			return ret;
	} else {
#if 0
		msg_aux_tx.req_id = REMOTE_DPCD_WRITE;
		msg_aux_tx.port_num = displayport->sst[sst_id]->vc_config->port_num;
		msg_aux_tx.dpcd_address = 0x00109;
		msg_aux_tx.num_write_bytes = 1;
		msg_aux_tx.write_data = 0x08;

		sb_msg_header.link_cnt_total = 1;
		sb_msg_header.link_cnt_remain = 0;
		sb_msg_header.broadcast_msg = 0x0;
		sb_msg_header.path_msg = 0x0;
		sb_msg_header.sb_msg_body_length = 7;
		sb_msg_header.start_of_msg_transcation = 1;
		sb_msg_header.end_of_msg_transcation = 1;
		sb_msg_header.msg_seq_no = 0;

		displayport_info("msg_aux_tx.port_num = %d\n", msg_aux_tx.port_num);

		displayport_msg_tx(DOWN_REQ);
		displayport_msg_rx(DOWN_REP);
#endif
		msg_aux_tx.req_id = REMOTE_I2C_READ;
		msg_aux_tx.num_i2c_tx = 1;
		msg_aux_tx.port_num = displayport->sst[sst_id]->vc_config->port_num;
		msg_aux_tx.write_i2c_dev_id = 0x50;
		msg_aux_tx.num_write_bytes = 1;
		msg_aux_tx.write_data = offset;
		msg_aux_tx.no_stop_bit = 0;
		msg_aux_tx.i2c_tx_delay = 0;
		msg_aux_tx.read_i2c_dev_id = 0x50;
		msg_aux_tx.num_read_bytes = EDID_BLOCK_SIZE;

		sb_msg_header.link_cnt_total = 1;
		sb_msg_header.link_cnt_remain = 0;
		sb_msg_header.broadcast_msg = 0;
		sb_msg_header.path_msg = 0;
		sb_msg_header.sb_msg_body_length = 9;
		sb_msg_header.start_of_msg_transcation = 1;
		sb_msg_header.end_of_msg_transcation = 1;
		sb_msg_header.msg_seq_no = 0;

		displayport_msg_tx(DOWN_REQ);
		displayport_msg_aux_remote_i2c_read(buf);
	}

	print_hex_dump(KERN_INFO, "EDID: ", DUMP_PREFIX_OFFSET, 16, 1,
					buf, 128, false);

	return 0;
}

int edid_read(u32 sst_id, struct displayport_device *displayport, u8 **data)
{
	u8 block0[EDID_BLOCK_SIZE];
	u8 *edid;
	int block = 0;
	int block_cnt = 0;
	int ret = 0;
	int retry_num = 5;

EDID_READ_RETRY:
	block = 0;
	block_cnt = 0;

	ret = edid_read_block(sst_id, displayport, 0, block0, sizeof(block0));
	if (ret)
		return ret;

	ret = edid_checksum(block0, block);
	if (ret) {
		if (retry_num <= 0) {
			displayport_err("edid read error\n");
			return ret;
		} else {
			msleep(100);
			retry_num--;
			goto EDID_READ_RETRY;
		}
	}

	block_cnt = block0[EDID_EXTENSION_FLAG] + 1;
	displayport_info("block_cnt = %d\n", block_cnt);

	edid = kmalloc(block_cnt * EDID_BLOCK_SIZE, GFP_KERNEL);
	if (!edid)
		return -ENOMEM;

	memcpy(edid, block0, sizeof(block0));

	while (++block < block_cnt) {
		ret = edid_read_block(sst_id, displayport, block,
					edid + (block * EDID_BLOCK_SIZE), EDID_BLOCK_SIZE);

		/* check error, extension tag and checksum */
		if (ret || *(edid + (block * EDID_BLOCK_SIZE)) != 0x02 ||
				edid_checksum(edid + (block * EDID_BLOCK_SIZE), block)) {
			displayport_info("block_cnt:%d/%d, ret: %d\n", block, block_cnt, ret);
			*data = edid;
			return block;
		}
	}

	*data = edid;

	return block_cnt;
}

static int get_ud_timing(struct fb_vendor *vsdb, int vic_idx)
{
	unsigned char val = 0;
	int idx = -EINVAL;

	val = vsdb->vic_data[vic_idx];
	switch (val) {
	case 0x01:
		idx = 0;
		break;
	case 0x02:
		idx = 1;
		break;
	case 0x03:
		idx = 2;
		break;
	case 0x04:
		idx = 3;
		break;
	}

	return idx;
}

bool edid_find_max_resolution(const struct v4l2_dv_timings *t1,
			const struct v4l2_dv_timings *t2)
{
	if ((t1->bt.width * t1->bt.height < t2->bt.width * t2->bt.height) ||
		((t1->bt.width * t1->bt.height == t2->bt.width * t2->bt.height) &&
		(t1->bt.pixelclock < t2->bt.pixelclock)))
		return true;

	return false;
}

static void edid_find_preset(u32 sst_id,
		struct displayport_device *displayport, const struct fb_videomode *mode)
{
	int i;

	displayport_dbg("EDID: %ux%u@%u - %u(ps?), lm:%u, rm:%u, um:%u, lm:%u",
		mode->xres, mode->yres, mode->refresh, mode->pixclock,
		mode->left_margin, mode->right_margin, mode->upper_margin, mode->lower_margin);

	for (i = 0; i < supported_videos_pre_cnt; i++) {
		if ((mode->refresh == supported_videos[i].fps ||
			mode->refresh == supported_videos[i].fps - 1) &&
			mode->xres == supported_videos[i].dv_timings.bt.width &&
			mode->yres == supported_videos[i].dv_timings.bt.height &&
			mode->left_margin == supported_videos[i].dv_timings.bt.hbackporch &&
			mode->right_margin == supported_videos[i].dv_timings.bt.hfrontporch &&
			mode->upper_margin == supported_videos[i].dv_timings.bt.vbackporch &&
			mode->lower_margin == supported_videos[i].dv_timings.bt.vfrontporch) {
			if (supported_videos[i].edid_support_match == false) {
				if (displayport->mst_cap == 0 || i <= MAX_MST_TIMINGS_ID) {
					displayport_info("EDID: found: %s\n", supported_videos[i].name);

					supported_videos[i].edid_support_match = true;
					preferred_preset = supported_videos[i].dv_timings;

					if (displayport->sst[sst_id]->best_video < i)
						displayport->sst[sst_id]->best_video = i;
				}
			}
		}
	}
}

static void edid_use_default_preset(void)
{
	int i;

	if (forced_resolution >= 0)
		preferred_preset = supported_videos[forced_resolution].dv_timings;
	else
		preferred_preset = supported_videos[EDID_DEFAULT_TIMINGS_IDX].dv_timings;

	for (i = 0; i < supported_videos_pre_cnt; i++) {
		supported_videos[i].edid_support_match =
			v4l2_match_dv_timings(&supported_videos[i].dv_timings,
					&preferred_preset, 0, 0);
	}

	if (!test_audio_info.channel_count)
		audio_channels = 2;
}

void edid_set_preferred_preset(int mode)
{
	int i;

	preferred_preset = supported_videos[mode].dv_timings;
	for (i = 0; i < supported_videos_pre_cnt; i++) {
		supported_videos[i].edid_support_match =
			v4l2_match_dv_timings(&supported_videos[i].dv_timings,
					&preferred_preset, 0, 0);
	}
}

int edid_find_resolution(u16 xres, u16 yres, u16 refresh)
{
	int i;
	int ret=0;

	for (i = 0; i < supported_videos_pre_cnt; i++) {
		if (refresh == supported_videos[i].fps &&
			xres == supported_videos[i].dv_timings.bt.width &&
			yres == supported_videos[i].dv_timings.bt.height) {
			return i;
		}
	}
	return ret;
}

void edid_parse_hdmi14_vsdb(unsigned char *edid_ext_blk,
	struct fb_vendor *vsdb, int block_cnt)
{
	int i, j;
	int hdmi_vic_len;
	int vsdb_offset_calc = VSDB_VIC_FIELD_OFFSET;

	for (i = 0; i < (block_cnt - 1) * EDID_BLOCK_SIZE; i++) {
		if ((edid_ext_blk[i] & DATA_BLOCK_TAG_CODE_MASK)
			== (VSDB_TAG_CODE << DATA_BLOCK_TAG_CODE_BIT_POSITION)
				&& edid_ext_blk[i + IEEE_OUI_0_BYTE_NUM] == HDMI14_IEEE_OUI_0
				&& edid_ext_blk[i + IEEE_OUI_1_BYTE_NUM] == HDMI14_IEEE_OUI_1
				&& edid_ext_blk[i + IEEE_OUI_2_BYTE_NUM] == HDMI14_IEEE_OUI_2) {
			displayport_dbg("EDID: find VSDB for HDMI 1.4\n");

			if (edid_ext_blk[i + 8] & VSDB_HDMI_VIDEO_PRESETNT_MASK) {
				displayport_dbg("EDID: Find HDMI_Video_present in VSDB\n");

				if (!(edid_ext_blk[i + 8] & VSDB_LATENCY_FILEDS_PRESETNT_MASK)) {
					vsdb_offset_calc = vsdb_offset_calc - 2;
					displayport_dbg("EDID: Not support LATENCY_FILEDS_PRESETNT in VSDB\n");
				}

				if (!(edid_ext_blk[i + 8] & VSDB_I_LATENCY_FILEDS_PRESETNT_MASK)) {
					vsdb_offset_calc = vsdb_offset_calc - 2;
					displayport_dbg("EDID: Not support I_LATENCY_FILEDS_PRESETNT in VSDB\n");
				}

				hdmi_vic_len = (edid_ext_blk[i + vsdb_offset_calc]
						& VSDB_VIC_LENGTH_MASK) >> VSDB_VIC_LENGTH_BIT_POSITION;

				if (hdmi_vic_len > 0) {
					vsdb->vic_len = hdmi_vic_len;

					for (j = 0; j < hdmi_vic_len; j++)
						vsdb->vic_data[j] = edid_ext_blk[i + vsdb_offset_calc + j + 1];

					break;
				} else {
					vsdb->vic_len = 0;
					displayport_dbg("EDID: No hdmi vic data in VSDB\n");
					break;
				}
			} else
				displayport_dbg("EDID: Not support HDMI_Video_present in VSDB\n");
		}
	}

	if (i >= (block_cnt - 1) * EDID_BLOCK_SIZE) {
		vsdb->vic_len = 0;
		displayport_dbg("EDID: can't find VSDB for HDMI 1.4 block\n");
	}
}

int static dv_timing_to_fb_video(videoformat video, struct fb_videomode *fb)
{
	struct displayport_supported_preset pre = supported_videos[video];

	fb->name = pre.name;
	fb->refresh = pre.fps;
	fb->xres = pre.dv_timings.bt.width;
	fb->yres = pre.dv_timings.bt.height;
	fb->pixclock = pre.dv_timings.bt.pixelclock;
	fb->left_margin = pre.dv_timings.bt.hbackporch;
	fb->right_margin = pre.dv_timings.bt.hfrontporch;
	fb->upper_margin = pre.dv_timings.bt.vbackporch;
	fb->lower_margin = pre.dv_timings.bt.vfrontporch;
	fb->hsync_len = pre.dv_timings.bt.hsync;
	fb->vsync_len = pre.dv_timings.bt.vsync;
	fb->sync = pre.v_sync_pol | pre.h_sync_pol;
	fb->vmode = FB_VMODE_NONINTERLACED;

	return 0;
}

void edid_find_hdmi14_vsdb_update(u32 sst_id,
		struct displayport_device *displayport, struct fb_vendor *vsdb)
{
	int udmode_idx, vic_idx;

	if (!vsdb)
		return;

	/* find UHD preset in HDMI 1.4 vsdb block*/
	if (vsdb->vic_len) {
		for (vic_idx = 0; vic_idx < vsdb->vic_len; vic_idx++) {
			udmode_idx = get_ud_timing(vsdb, vic_idx);

			displayport_dbg("EDID: udmode_idx = %d\n", udmode_idx);

			if (udmode_idx >= 0) {
				struct fb_videomode fb;

				dv_timing_to_fb_video(ud_mode_h14b_vsdb[udmode_idx], &fb);
				edid_find_preset(sst_id, displayport, &fb);
			}
		}
	}
}

void edid_parse_hdmi20_vsdb(u32 sst_id, struct displayport_device *displayport,
		unsigned char *edid_ext_blk, struct fb_vendor *vsdb, int block_cnt)
{
	int i;

	displayport->sst[sst_id]->rx_edid_data.max_support_clk = 0;
	displayport->sst[sst_id]->rx_edid_data.support_10bpc = 0;

	for (i = 0; i < (block_cnt - 1) * EDID_BLOCK_SIZE; i++) {
		if ((edid_ext_blk[i] & DATA_BLOCK_TAG_CODE_MASK)
				== (VSDB_TAG_CODE << DATA_BLOCK_TAG_CODE_BIT_POSITION)
				&& edid_ext_blk[i + IEEE_OUI_0_BYTE_NUM] == HDMI20_IEEE_OUI_0
				&& edid_ext_blk[i + IEEE_OUI_1_BYTE_NUM] == HDMI20_IEEE_OUI_1
				&& edid_ext_blk[i + IEEE_OUI_2_BYTE_NUM] == HDMI20_IEEE_OUI_2) {
			displayport_dbg("EDID: find VSDB for HDMI 2.0\n");

			/* Max_TMDS_Character_Rate * 5Mhz */
			displayport->sst[sst_id]->rx_edid_data.max_support_clk =
					edid_ext_blk[i + MAX_TMDS_RATE_BYTE_NUM] * 5;
			displayport_info("EDID: Max_TMDS_Character_Rate = %d Mhz\n",
					displayport->sst[sst_id]->rx_edid_data.max_support_clk);

			if (edid_ext_blk[i + DC_SUPPORT_BYTE_NUM] & DC_30BIT)
				displayport->sst[sst_id]->rx_edid_data.support_10bpc = 1;
			else
				displayport->sst[sst_id]->rx_edid_data.support_10bpc = 0;

			displayport_info("EDID: 10 bpc support = %d\n",
				displayport->sst[sst_id]->rx_edid_data.support_10bpc);

			break;
		}
	}

	if (i >= (block_cnt - 1) * EDID_BLOCK_SIZE) {
		vsdb->vic_len = 0;
		displayport_dbg("EDID: can't find VSDB for HDMI 2.0 block\n");
	}
}

void edid_parse_hdr_metadata(u32 sst_id, struct displayport_device *displayport,
		unsigned char *edid_ext_blk,  int block_cnt)
{
	int i;

	displayport->sst[sst_id]->rx_edid_data.hdr_support = 0;
	displayport->sst[sst_id]->rx_edid_data.eotf = 0;
	displayport->sst[sst_id]->rx_edid_data.max_lumi_data = 0;
	displayport->sst[sst_id]->rx_edid_data.max_average_lumi_data = 0;
	displayport->sst[sst_id]->rx_edid_data.min_lumi_data = 0;

	for (i = 0; i < (block_cnt - 1) * EDID_BLOCK_SIZE; i++) {
		if ((edid_ext_blk[i] & DATA_BLOCK_TAG_CODE_MASK)
				== (USE_EXTENDED_TAG_CODE << DATA_BLOCK_TAG_CODE_BIT_POSITION)
				&& edid_ext_blk[i + EXTENDED_TAG_CODE_BYTE_NUM]
				== EXTENDED_HDR_TAG_CODE) {
			displayport_dbg("EDID: find HDR Metadata Data Block\n");

			displayport->sst[sst_id]->rx_edid_data.eotf =
				edid_ext_blk[i + SUPPORTED_EOTF_BYTE_NUM];
			displayport_dbg("EDID: SUPPORTED_EOTF = 0x%x\n",
				displayport->sst[sst_id]->rx_edid_data.eotf);

			if (displayport->sst[sst_id]->rx_edid_data.eotf & SMPTE_ST_2084) {
				displayport->sst[sst_id]->rx_edid_data.hdr_support = 1;
				displayport_info("EDID: SMPTE_ST_2084 support, but not now\n");
			}

			displayport->sst[sst_id]->rx_edid_data.max_lumi_data =
					edid_ext_blk[i + MAX_LUMI_BYTE_NUM];
			displayport_dbg("EDID: MAX_LUMI = 0x%x\n",
					displayport->sst[sst_id]->rx_edid_data.max_lumi_data);

			displayport->sst[sst_id]->rx_edid_data.max_average_lumi_data =
					edid_ext_blk[i + MAX_AVERAGE_LUMI_BYTE_NUM];
			displayport_dbg("EDID: MAX_AVERAGE_LUMI = 0x%x\n",
					displayport->sst[sst_id]->rx_edid_data.max_average_lumi_data);

			displayport->sst[sst_id]->rx_edid_data.min_lumi_data =
					edid_ext_blk[i + MIN_LUMI_BYTE_NUM];
			displayport_dbg("EDID: MIN_LUMI = 0x%x\n",
					displayport->sst[sst_id]->rx_edid_data.min_lumi_data);

			displayport_info("HDR: EOTF(0x%X) ST2084(%u) GAMMA(%s|%s) LUMI(max:%u,avg:%u,min:%u)\n",
					displayport->sst[sst_id]->rx_edid_data.eotf,
					displayport->sst[sst_id]->rx_edid_data.hdr_support,
					displayport->sst[sst_id]->rx_edid_data.eotf & 0x1 ? "SDR" : "",
					displayport->sst[sst_id]->rx_edid_data.eotf & 0x2 ? "HDR" : "",
					displayport->sst[sst_id]->rx_edid_data.max_lumi_data,
					displayport->sst[sst_id]->rx_edid_data.max_average_lumi_data,
					displayport->sst[sst_id]->rx_edid_data.min_lumi_data);
			break;
		}
	}

	if (i >= (block_cnt - 1) * EDID_BLOCK_SIZE)
		displayport_dbg("EDID: can't find HDR Metadata Data Block\n");
}

void edid_find_preset_in_video_data_block(u32 sst_id,
		struct displayport_device *displayport, u8 vic)
{
	int i;

	for (i = 0; i < supported_videos_pre_cnt; i++) {
		if ((vic != 0) && (supported_videos[i].vic == vic) &&
				(supported_videos[i].edid_support_match == false)) {
			if (displayport->mst_cap == 0 || i <= MAX_MST_TIMINGS_ID) {
				supported_videos[i].edid_support_match = true;

				if (displayport->sst[sst_id]->best_video < i)
					displayport->sst[sst_id]->best_video = i;

				displayport_info("EDID: found(VDB): %s\n", supported_videos[i].name);
			}
		}

	}
}

static int edid_parse_audio_video_db(u32 sst_id, struct displayport_device *displayport,
		unsigned char *edid, struct fb_audio *sad)
{
	int i;
	u8 pos = 4;

	if (!edid)
		return -EINVAL;

	if (edid[0] != 0x2 || edid[1] != 0x3 ||
	    edid[2] < 4 || edid[2] > 128 - DETAILED_TIMING_DESCRIPTION_SIZE)
		return -EINVAL;

	if (!sad)
		return -EINVAL;

	while (pos < edid[2]) {
		u8 len = edid[pos] & DATA_BLOCK_LENGTH_MASK;
		u8 type = (edid[pos] >> DATA_BLOCK_TAG_CODE_BIT_POSITION) & 7;
		displayport_dbg("Data block %u of %u bytes\n", type, len);

		if (len == 0)
			break;

		pos++;
		if (type == AUDIO_DATA_BLOCK) {
			for (i = pos; i < pos + len; i += 3) {
				if (((edid[i] >> 3) & 0xf) != 1)
					continue; /* skip non-lpcm */

				displayport_dbg("LPCM ch=%d\n", (edid[i] & 7) + 1);

				sad->channel_count |= 1 << (edid[i] & 0x7);
				sad->sample_rates |= (edid[i + 1] & 0x7F);
				sad->bit_rates |= (edid[i + 2] & 0x7);

				displayport_dbg("ch:0x%X, sample:0x%X, bitrate:0x%X\n",
					sad->channel_count, sad->sample_rates, sad->bit_rates);
			}
		} else if (type == VIDEO_DATA_BLOCK) {
			for (i = pos; i < pos + len; i++) {
				u8 vic = edid[i] & SVD_VIC_MASK;
				edid_find_preset_in_video_data_block(sst_id, displayport, vic);
				displayport_dbg("EDID: Video data block vic:%d %s\n",
					vic, supported_videos[i].name);
			}
		} else if (type == SPEAKER_DATA_BLOCK) {
			sad->speaker |= edid[pos] & 0xff;
			displayport_dbg("EDID: speaker 0x%X\n", sad->speaker);
		}

		pos += len;
	}

	return 0;
}

void edid_check_detail_timing_desc1(u32 sst_id, struct displayport_device *displayport,
		struct fb_monspecs *specs, int modedb_len, u8 *edid)
{
	int i;
	struct fb_videomode *mode = NULL;
	u64 pixelclock = 0;
	u8 *block = edid + DETAILED_TIMING_DESCRIPTIONS_START;

	for (i = 0; i < modedb_len; i++) {
		mode = &(specs->modedb[i]);
		if (mode->flag == (FB_MODE_IS_FIRST | FB_MODE_IS_DETAILED))
			break;
	}

	if (i >= modedb_len)
		return;

	mode = &(specs->modedb[i]);
	pixelclock = (u64)((u32)block[1] << 8 | (u32)block[0]) * 10000;

	displayport_info("detail_timing_desc1: %d*%d@%d (%lld, %dps)\n",
			mode->xres, mode->yres, mode->refresh, pixelclock, mode->pixclock);

	for (i = 0; i < supported_videos_pre_cnt; i++) {
		if (mode->vmode == FB_VMODE_NONINTERLACED &&
			(mode->refresh == supported_videos[i].fps ||
			 mode->refresh == supported_videos[i].fps - 1) &&
			mode->xres == supported_videos[i].dv_timings.bt.width &&
			mode->yres == supported_videos[i].dv_timings.bt.height) {
			if (supported_videos[i].edid_support_match == true) {
				displayport_info("already found timing:%d\n", i);
				return;
			}

			break; /* matched but not found */
		}
	}

	/* check if index is valid and index is bigger than best video */
	if (i >= supported_videos_pre_cnt || i <= displayport->sst[sst_id]->best_video) {
		displayport_info("invalid timing i:%d, best:%d\n",
				i, displayport->sst[sst_id]->best_video);
		return;
	}

	displayport_info("find same supported timing: %d*%d@%d (%lld)\n",
			supported_videos[i].dv_timings.bt.width,
			supported_videos[i].dv_timings.bt.height,
			supported_videos[i].fps,
			supported_videos[i].dv_timings.bt.pixelclock);

	if (supported_videos[V640X480P60].dv_timings.bt.pixelclock >= pixelclock ||
			supported_videos[V4096X2160P60].dv_timings.bt.pixelclock <= pixelclock) {
		displayport_info("EDID: invalid pixel clock\n");
		return;
	}

	displayport->sst[sst_id]->best_video = VDUMMYTIMING;
	supported_videos[VDUMMYTIMING].dv_timings.bt.width = mode->xres;
	supported_videos[VDUMMYTIMING].dv_timings.bt.height = mode->yres;
	supported_videos[VDUMMYTIMING].dv_timings.bt.interlaced = false;
	supported_videos[VDUMMYTIMING].dv_timings.bt.pixelclock = pixelclock;
	supported_videos[VDUMMYTIMING].dv_timings.bt.hfrontporch = mode->right_margin;
	supported_videos[VDUMMYTIMING].dv_timings.bt.hsync = mode->hsync_len;
	supported_videos[VDUMMYTIMING].dv_timings.bt.hbackporch = mode->left_margin;
	supported_videos[VDUMMYTIMING].dv_timings.bt.vfrontporch = mode->lower_margin;
	supported_videos[VDUMMYTIMING].dv_timings.bt.vsync = mode->vsync_len;
	supported_videos[VDUMMYTIMING].dv_timings.bt.vbackporch = mode->upper_margin;
	supported_videos[VDUMMYTIMING].fps = mode->refresh;
	supported_videos[VDUMMYTIMING].v_sync_pol = (mode->sync & FB_SYNC_VERT_HIGH_ACT);
	supported_videos[VDUMMYTIMING].h_sync_pol = (mode->sync & FB_SYNC_HOR_HIGH_ACT);
	supported_videos[VDUMMYTIMING].edid_support_match = true;
	preferred_preset = supported_videos[VDUMMYTIMING].dv_timings;

	displayport_dbg("EDID: modedb : %d*%d@%d (%lld)\n", mode->xres, mode->yres, mode->refresh,
			supported_videos[VDUMMYTIMING].dv_timings.bt.pixelclock);
	displayport_dbg("EDID: modedb : %d %d %d  %d %d %d  %d %d %d\n",
			mode->left_margin, mode->hsync_len, mode->right_margin,
			mode->upper_margin, mode->vsync_len, mode->lower_margin,
			mode->sync, mode->vmode, mode->flag);
	displayport_info("EDID: %s edid_support_match = %d\n", supported_videos[VDUMMYTIMING].name,
			supported_videos[VDUMMYTIMING].edid_support_match);
}

int edid_update(u32 sst_id, struct displayport_device *displayport)
{
	struct fb_monspecs specs;
	struct fb_vendor vsdb;
	struct fb_audio sad;
	u8 *edid = NULL;
	int block_cnt = 0;
	int i;
	int basic_audio = 0;
	int modedb_len = 0;

	audio_channels = 0;
	audio_sample_rates = 0;
	audio_bit_rates = 0;
	audio_speaker_alloc = 0;

	edid_misc = 0;
	memset(&vsdb, 0, sizeof(vsdb));
	memset(&specs, 0, sizeof(specs));
	memset(&sad, 0, sizeof(sad));

	memset(displayport->sst[sst_id]->rx_edid_data.edid_manufacturer,
			0, sizeof(specs.manufacturer));
	displayport->sst[sst_id]->rx_edid_data.edid_product = 0;
	displayport->sst[sst_id]->rx_edid_data.edid_serial = 0;

	preferred_preset = supported_videos[EDID_DEFAULT_TIMINGS_IDX].dv_timings;
	supported_videos[0].edid_support_match = true; /*default support VGA*/
	supported_videos[VDUMMYTIMING].dv_timings.bt.width = 0;
	supported_videos[VDUMMYTIMING].dv_timings.bt.height = 0;
	for (i = 1; i < supported_videos_pre_cnt; i++)
		supported_videos[i].edid_support_match = false;
	block_cnt = edid_read(sst_id, displayport, &edid);
	if (block_cnt < 0)
		goto out;

	fb_edid_to_monspecs(edid, &specs);
	modedb_len = specs.modedb_len;

	displayport_info("mon name: %s, gamma: %u.%u\n", specs.monitor,
			specs.gamma / 100, specs.gamma % 100);
	for (i = 1; i < block_cnt; i++)
		fb_edid_add_monspecs(edid + i * EDID_BLOCK_SIZE, &specs);

	/* find 2D preset */
	for (i = 0; i < specs.modedb_len; i++)
		edid_find_preset(sst_id, displayport, &specs.modedb[i]);

	/* color depth */
	if (edid[EDID_COLOR_DEPTH] & 0x80) {
		if (((edid[EDID_COLOR_DEPTH] & 0x70) >> 4) == 1)
			displayport->sst[sst_id]->bpc = BPC_6;
	}

	/* vendro block */
	memcpy(displayport->sst[sst_id]->rx_edid_data.edid_manufacturer,
			specs.manufacturer, sizeof(specs.manufacturer));
	displayport->sst[sst_id]->rx_edid_data.edid_product = specs.model;
	displayport->sst[sst_id]->rx_edid_data.edid_serial = specs.serial;

	/* number of 128bytes blocks to follow */
	if (block_cnt <= 1)
		goto out;

	if (edid[EDID_NATIVE_FORMAT] & EDID_BASIC_AUDIO) {
		basic_audio = 1;
		edid_misc = FB_MISC_HDMI;
	}

	edid_parse_hdmi14_vsdb(edid + EDID_BLOCK_SIZE, &vsdb, block_cnt);
	edid_find_hdmi14_vsdb_update(sst_id, displayport, &vsdb);

	edid_parse_hdmi20_vsdb(sst_id, displayport,
			edid + EDID_BLOCK_SIZE, &vsdb, block_cnt);

	edid_parse_hdr_metadata(sst_id, displayport,
			edid + EDID_BLOCK_SIZE, block_cnt);

	for (i = 1; i < block_cnt; i++)
		edid_parse_audio_video_db(sst_id, displayport,
				edid + (EDID_BLOCK_SIZE * i), &sad);

	if (!edid_misc)
		edid_misc = specs.misc;

	if (edid_misc & FB_MISC_HDMI) {
		audio_speaker_alloc = sad.speaker;
		if (sad.channel_count) {
			audio_channels = sad.channel_count;
			audio_sample_rates = sad.sample_rates;
			audio_bit_rates = sad.bit_rates;
		} else if (basic_audio) {
			audio_channels = 2;
			audio_sample_rates = FB_AUDIO_48KHZ; /*default audio info*/
			audio_bit_rates = FB_AUDIO_16BIT;
		}
	}

	if (test_audio_info.channel_count != 0) {
		audio_channels = test_audio_info.channel_count;
		audio_sample_rates = test_audio_info.sample_rates;
		audio_bit_rates = test_audio_info.bit_rates;
		displayport_info("test audio ch:0x%X, sample:0x%X, bit:0x%X\n",
			audio_channels, audio_sample_rates, audio_bit_rates);
	}

	displayport_info("misc:0x%X, Audio ch:0x%X, sample:0x%X, bit:0x%X\n",
			edid_misc, audio_channels, audio_sample_rates, audio_bit_rates);

out:
	edid_check_detail_timing_desc1(sst_id, displayport,
			&specs, modedb_len, edid);

	/* No supported preset found, use default */
	if (forced_resolution >= 0) {
		displayport_info("edid_use_default_preset\n");
		edid_use_default_preset();
	}

	if (block_cnt == -EPROTO)
		edid_misc = FB_MISC_HDMI;

	if (block_cnt >= 2)
		kfree(edid);

	return block_cnt;
}

struct v4l2_dv_timings edid_preferred_preset(void)
{
	return preferred_preset;
}

bool edid_supports_hdmi(struct displayport_device *displayport)
{
	return edid_misc & FB_MISC_HDMI;
}

bool edid_support_pro_audio(void)
{
	if (audio_channels >= FB_AUDIO_8CH && audio_sample_rates >= FB_AUDIO_192KHZ)
		return true;

	return false;
}

u32 edid_audio_informs(void)
{
	struct displayport_device *displayport = get_displayport_drvdata();
	u32 value = 0, ch_info = 0;
	u32 link_rate = displayport_reg_phy_get_link_bw();

	if (audio_channels > 0)
		ch_info = audio_channels;

	/* if below conditions does not meet pro audio, then reduce sample frequency.
	 * 1. current video is not support pro audio
	 * 2. link rate is below HBR2
	 * 3. dex setting is not set
	 * 4. channel is over 2 and under 8.
	 * 5. channel is 8. And smaple freq is under 192KHz
	 */
	if ((!supported_videos[displayport->sst[SST1]->cur_video].pro_audio_support ||
				link_rate < LINK_RATE_5_4Gbps)) {
		if ((ch_info > FB_AUDIO_1N2CH && ch_info < FB_AUDIO_8CH) ||
				(ch_info >= FB_AUDIO_8CH && audio_sample_rates < FB_AUDIO_192KHZ)) {
			displayport_info("reduce SF(pro_aud:%d, link_rate:0x%X, ch:0x%X, sf:0x%X)\n",
					supported_videos[displayport->sst[SST1]->cur_video].pro_audio_support, link_rate,
					ch_info, audio_sample_rates);
			audio_sample_rates &= 0x7; /* reduce to under 48KHz */
		}
	}

	value = ((audio_sample_rates << 19) | (audio_bit_rates << 16) |
			(audio_speaker_alloc << 8) | ch_info);
	value |= (1 << 26); /* 1: DP, 0: HDMI */

	displayport_info("audio info = 0x%X\n", value);

	return value;
}

struct fb_audio *edid_get_test_audio_info(void)
{
	return &test_audio_info;
}

u8 edid_read_checksum(void)
{
	int ret, i;
	u8 buf[EDID_BLOCK_SIZE];
	u8 offset = EDID_OFFSET(0);
	int sum = 0;

	ret = displayport_reg_edid_read(offset, EDID_BLOCK_SIZE, buf);
	if (ret)
		return ret;

	for (i = 0; i < EDID_BLOCK_SIZE; i++)
		sum += buf[i];

	displayport_info("edid_read_checksum %02x, %02x", sum%265, buf[EDID_BLOCK_SIZE-1]);

	return buf[EDID_BLOCK_SIZE-1];
}
