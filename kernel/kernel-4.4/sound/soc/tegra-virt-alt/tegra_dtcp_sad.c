/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property
 * and proprietary rights in and to this software and related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA Corporation is strictly prohibited.
 */

#include "tegra_dtcp_sad.h"
#include "tegra_virt_alt_ivc.h"
#include "tegra210_virt_alt_admaif.h"

#include <linux/delay.h>
#include <sound/tegra_adma.h>

#define IO_AUDIO_DTCP_COMMANDLINE_LENGTH 100
#define IO_AUDIO_DTCP_NUM_COMMAND 10
#define AUDIO_DEST_SAMPLE_RATE 48000
#define AUDIO_DEST_SAMPLE_WIDTH 24

#define MOST_DTCP_SAD0_SYNC_HI      0x3C
#define MOST_DTCP_SAD1_SYNC_LO      0xB2
#define MOST_DTCP_SAD1_NUM_BYTES    0x07

/**
 * EMI types
 */
#define EMI_MODE_A       0x3
#define EMI_MODE_B       0x2
#define EMI_MODE_C       0x1
#define EMI_MODE_NA      0x0

/**
 * ADMA info
 */
#define ADMA_PAGE1_BASE 0x2940000
#define ADMA_PAGE_SIZE  0x10000

/**
 * Default metadata used
 */
static const sad_metadata_t
default_metadata = {
	MEDIA_TYPE_UNDEF, /* undefined media type */
	PCM_CONF_314, /* Eight channel */
	CA_METHOD_NONE, /* No encryption */
	0, /* Unknown codec type */
	24, /* sample_width */
	48000, /* sample_rate */
	0 /* bit rate */
};

/* DTCP config file related definea */
typedef struct {
	char command[IO_AUDIO_DTCP_NUM_COMMAND]
		[IO_AUDIO_DTCP_COMMANDLINE_LENGTH];
	bool dtcp_config_file_read;
	unsigned int num_commands;
} dtcp_command_t;

static dtcp_command_t dtcp_config_data;

#define islower(c)     ('a' <= (c) && (c) <= 'z')
#define toupper(c)     (islower(c) ? ((c) - 'a' + 'A') : (c))

static char *sad_strupr(char *src_string)
{
	char *string;

	if (!src_string)
		return NULL;

	/* Walk entire string, uppercasing the letters */
	for (string = src_string; *string; string++)
		*string = (char)toupper((int)*string);

	return src_string;
}

static int sad_atoi(char *str)
{
	int res = 0, i = 0;

	/* Iterate through all characters of input string and
	 * update result
	*/
	for (i = 0; str[i] != '\0'; ++i)
		res = res*10 + str[i] - '0';

	return res;
}

static uint8_t
get_asad_from_pcm_conf(uint8_t pcm_conf)
{
	/* As per MOST Audio spec ver 3.5 , the ASAD values of 0x20 and 0x30 are
	 * not allowed so data needs to reformatted to be compatible to 0xC0
	 * and 0xD0 respectively
	 */

	uint8_t asad = 0;

	switch (pcm_conf) {
	case PCM_CONF_100: /*0x20 => "C" only */
		asad = PCM_CONF_200;/* 0xC0 => "L" and "R" */
		break;
	case PCM_CONF_110: /*0x30 => "C" and "LFE" */
		asad = PCM_CONF_210; /*0xD0 => "L" , "R" and "LFE" */
		break;
	default:
		asad = pcm_conf;
		break;
	}

	return asad;
}

/*
*DTCP enable will be determined using the default criterion
*This can be overridden using a config file of below format
*Typical config file
*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
*-ca_method CSS @end
*-codec_type CDDA @end
*-source_sample_width -min 16 @end
*-destination_sample_width -min 16 @end
*-source_sample_rate -min 44100 @end
*-pcm_conf 0xFF @end
*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
*/
static int32_t
read_dtcp_config_file(void)
{
	/* DT support to be added*/
	return -1;
}

static bool
get_dtcp_config(sad_metadata_t *config)
{
	unsigned int num = 0;
	int ret;
	char fargv[IO_AUDIO_DTCP_NUM_COMMAND][IO_AUDIO_DTCP_COMMANDLINE_LENGTH];

	unsigned char ca_method = CA_METHOD_NONE;
	unsigned char codec_type = CODEC_TYPE_UNKNOWN;
	unsigned char source_sample_width, dest_sample_width, pcm_conf;
	unsigned int source_sample_rate, destination_sample_rate;

	/*Default condition for enabling DTCP */

	/* Any encrypted content */
	bool b_ca_method_default = (config->ca_method != CA_METHOD_NONE);
	bool b_ca_method = b_ca_method_default;

	/* All Codecs */
	bool b_codec_type_default = true;
	bool b_codec_type = b_codec_type_default;

	/* Source sample width > 16 bits */
	bool b_source_sample_width_default = (config->sample_width > 16);
	bool b_source_sample_width = b_source_sample_width_default;

	/* Destination sample width > 16 bits */
	bool b_dest_sample_width_default = (AUDIO_DEST_SAMPLE_WIDTH > 16);
	bool b_dest_sample_width = b_dest_sample_width_default;

	/* Source sample rate > 48 KHz */
	bool b_source_sample_rate_default = (config->sample_rate > 48000);
	bool b_source_sample_rate = b_source_sample_rate_default;

	/* Destination sample rate > 48 KHz */
	bool b_dest_sample_rate_default = (AUDIO_DEST_SAMPLE_RATE > 48000);
	bool b_dest_sample_rate = b_dest_sample_rate_default;

	/* All channel configurations */
	bool b_pcm_conf_default = true;
	bool b_pcm_conf = b_pcm_conf_default;

	if (!dtcp_config_data.dtcp_config_file_read) {
		ret = read_dtcp_config_file();
		if (!ret)
			dtcp_config_data.dtcp_config_file_read = true;
		else
			return 0;
	}

	while (num < dtcp_config_data.num_commands) {
		int cur_pos = 0;
		int argc = 0;
		int i = 0;

		int scan_ret = sscanf(dtcp_config_data.command[num],
				"%s", fargv[argc]);

		while (scan_ret != 0) {
			if ((strcmp((const char *) fargv[argc], "@end") == 0)
				|| (fargv[argc][0] == '#'))
				break;

			cur_pos += strlen((const char *) fargv[argc]);
			while (*(dtcp_config_data.command[num] + cur_pos)
				== ' ' || *(dtcp_config_data.command[num] +
				cur_pos) == '\t')
				cur_pos++;
			argc++;
			scan_ret = sscanf(dtcp_config_data.command[num] +
					cur_pos, "%s", fargv[argc]);
		}

		if (strcmp((const char *) fargv[0], "-ca_method") == 0) {
			b_ca_method = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) sad_strupr(
						fargv[i]), "CSS") == 0)
					ca_method = CA_METHOD_CSS;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "AACS") == 0)
					ca_method = CA_METHOD_AACS;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "NONE") == 0)
					ca_method = CA_METHOD_NONE;

				if (config->ca_method == ca_method) {
					b_ca_method = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0], "-codec_type")
				== 0) {
			b_codec_type = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) sad_strupr(
						fargv[i]), "UNKNOWN") == 0)
					codec_type = CODEC_TYPE_UNKNOWN;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "CDDA") == 0)
					codec_type = CODEC_TYPE_CDDA;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "MPA") == 0)
					codec_type = CODEC_TYPE_MPA;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "MP3") == 0)
					codec_type = CODEC_TYPE_MP3;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "AC3") == 0)
					codec_type = CODEC_TYPE_AC3;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "DTS") == 0)
					codec_type = CODEC_TYPE_DTS;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "AACLC") == 0)
					codec_type = CODEC_TYPE_AACLC;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "AACHE") == 0)
					codec_type = CODEC_TYPE_AACHE;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "WMA") == 0)
					codec_type = CODEC_TYPE_WMA;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "WMALSL") == 0)
					codec_type = CODEC_TYPE_WMALSL;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "WMAPRO") == 0)
					codec_type = CODEC_TYPE_WMAPRO;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "SBC") == 0)
					codec_type = CODEC_TYPE_SBC;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "FLAC") == 0)
					codec_type = CODEC_TYPE_FLAC;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "VORBIS") == 0)
					codec_type = CODEC_TYPE_VORBIS;
				else if (strcmp((const char *) sad_strupr(
						fargv[i]), "ALAC") == 0)
					codec_type = CODEC_TYPE_ALAC;

				if (config->codec_type == codec_type) {
					b_codec_type = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0],
				"-source_sample_width") == 0) {
			b_source_sample_width = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) fargv[i], "-min")
					== 0) {
					if (config->sample_width >=
						sad_atoi(fargv[i+1])) {
						b_source_sample_width = true;
						break;
					}
				}

				source_sample_width = sad_atoi(fargv[i]);
				if (config->sample_width ==
						source_sample_width) {
					b_source_sample_width = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0],
				"-destination_sample_width") == 0) {
			b_dest_sample_width = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) fargv[i], "-min")
								== 0) {
					if (sad_atoi(fargv[i+1])
						<= AUDIO_DEST_SAMPLE_WIDTH) {
						b_dest_sample_width = true;
						break;
					}
				}

				dest_sample_width = sad_atoi(fargv[i]);
				if (dest_sample_width ==
					AUDIO_DEST_SAMPLE_WIDTH
					) {
					b_dest_sample_width = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0],
				"-source_sample_rate") == 0) {
			b_source_sample_rate = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) fargv[i], "-min")
							== 0) {
					if (config->sample_rate >=
							sad_atoi(fargv[i+1])) {
						b_source_sample_rate = true;
						break;
					}
				}

				source_sample_rate = sad_atoi(fargv[i]);
				if (config->sample_rate
					== source_sample_rate) {
					b_source_sample_rate = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0],
				"-destination_sample_rate") == 0) {
			b_dest_sample_rate = false;
			for (i = 1; i < argc ; i++) {
				if (strcmp((const char *) fargv[i], "-min")
					== 0) {
					if (AUDIO_DEST_SAMPLE_RATE
						>= sad_atoi(fargv[i+1])) {
						b_dest_sample_rate = true;
						break;
					}
				}

				destination_sample_rate = sad_atoi(fargv[i]);
				if (AUDIO_DEST_SAMPLE_RATE ==
					destination_sample_rate) {
					b_dest_sample_rate = true;
					break;
				}
			}
		} else if (strcmp((const char *) fargv[0], "-pcm_conf") == 0) {
			b_pcm_conf = false;
			for (i = 1; i < argc ; i++) {
				pcm_conf = sad_atoi(fargv[i]);
				if (config->pcm_conf == pcm_conf) {
					b_pcm_conf = true;
					break;
				}
			}
		}

		num++;
	}

	/* Config file should be used to only strengthen the conditions for
	 * enabling the DTCP "OR" with default conditions to make sure that
	 * config file does not loosen the conditions to set DTCP  See NvBug
	 * 1392782 for more details.
	*/
	b_ca_method = b_ca_method || b_ca_method_default;
	b_codec_type = b_codec_type || b_codec_type_default;
	b_source_sample_width = b_source_sample_width ||
				b_source_sample_width_default;
	b_dest_sample_width = b_dest_sample_width ||
				b_dest_sample_width_default;
	b_source_sample_rate = b_source_sample_rate ||
				b_source_sample_rate_default;
	b_dest_sample_rate = b_dest_sample_rate || b_dest_sample_rate_default;
	b_pcm_conf = b_pcm_conf || b_pcm_conf_default;

	/* Calculate enable flag based on CSS specifications. See NvBug 1450720
	 * for more details.
	*/
	return b_ca_method &&
		b_codec_type &&
		((b_source_sample_width && b_dest_sample_width)
		|| (b_source_sample_rate && b_dest_sample_rate)) &&
		b_pcm_conf;
}

static uint8_t
get_emi_config(sad_metadata_t *source_metadata)
{
	int ret = get_dtcp_config(source_metadata);

	if (ret) {
		pr_debug("setting EMI_MODE_A\n");
		return EMI_MODE_A;
	}
	pr_debug("setting EMI_MODE_NA\n");
	return EMI_MODE_NA;
}

/* Enable this to set default */
static void
sad_header_reset(sad_context_t *s, sad_metadata_t *m)
{
	memcpy(m, &default_metadata, sizeof(*m));
}

static void
dump_metadata(sad_metadata_t *source_metadata)
{
	pr_debug("MediaType           = %u\n", source_metadata->media_type);
	pr_debug("Pcm conf            = %u\n", source_metadata->pcm_conf);
	pr_debug("CA method           = %u\n", source_metadata->ca_method);
	pr_debug("Codec type          = %u\n", source_metadata->codec_type);
	pr_debug("Source Sample Width = %u\n", source_metadata->sample_width);
	pr_debug("Source Sample Rate  = %d\n", source_metadata->sample_rate);
	pr_debug("Source Bit Rate     = %d\n", source_metadata->bit_rate);
}

static void
dump_header(uint16_t *header)
{
	pr_debug("header1 = 0x%04x\n", header[0]);
	pr_debug("header2 = 0x%04x\n", header[1]);
	pr_debug("header3 = 0x%04x\n", header[2]);
	pr_debug("header4 = 0x%04x\n", header[3]);
	pr_debug("header5 = 0x%04x\n", header[4]);
	pr_debug("header6 = 0x%04x\n", header[5]);
	pr_debug("header7 = 0x%04x\n", header[6]);
	pr_debug("header8 = 0x%04x\n", header[7]);
}

static void
dump_buffer(uint16_t *buffer, uint32_t block_size)
{
	int i;

	for (i = 0; i < block_size; i++) {
		if (i % (MOST_DTCP_SAD_SBFRM_LENGTH + 1) == 0)
			pr_debug("\n");
		pr_debug("0x%04X\n", buffer[i]);
	}
}

static void
sad_header_create(uint16_t *header,
	sad_metadata_t *source_metadata, uint32_t sad_mode)
{
	/* Create the SAD header based on the source metadata information*/

	uint8_t media_type = source_metadata->media_type;
	uint8_t pcm_conf = source_metadata->pcm_conf;
	uint8_t emi = 0;
	uint8_t dtcp_info = 0;
	uint8_t asad = get_asad_from_pcm_conf(pcm_conf);
	uint8_t sad1_2 = 0;
	uint8_t sad1_3 = 0;
	uint8_t sad1_4 = 0;
	uint8_t sad1_5 = 0;

	if (!header || !source_metadata) {
		pr_err("Invalid params header:%p source_metadata: %p\n",
					header, source_metadata);
		return;
	}

	if (sad_mode != SAD_HEADER_MODE) {
		memset(header, 0, MOST_DTCP_SAD_HDR_LENGTH * sizeof(uint16_t));
		return;
	}

	emi = get_emi_config(source_metadata);
	dtcp_info = emi << 5;

	if (media_type != MEDIA_TYPE_DVD) {
		sad1_2 = (source_metadata->bit_rate >> 8) & 0xFF;
		sad1_3 = source_metadata->bit_rate & 0xFF;
		sad1_4 = source_metadata->codec_type;
		sad1_5 = source_metadata->sample_width;
	}
	/* SAD Bytes | Info-DVD         or  Info-other */
	/* RSVD1(0)  | [NUM_BYTES(7)    or  NUM_BYTES       ] */
	header[0] = MOST_DTCP_SAD1_NUM_BYTES & 0x00FF;
	/* RSVD2(0)  | [MEDIA_TYPE_DVD  or  MEDIA_TYPE_UNDEF] */
	header[1] = media_type & 0x00FF;
	/* RSVD3(0)  | [CCI_CGSM(0)     or  bit_rate_hi     ] */
	header[2] = (sad1_2 & 0x00FF);
	/* RSVD4(0)  | [ISRC_RSVD1(0)   or  bit_rate_lo     ] */
	header[3] = (sad1_3 & 0x00FF);
	/* SYNCHI    | [ISRC_RSVD2(0)   or  codec_type      ] */
	header[4] = ((MOST_DTCP_SAD0_SYNC_HI << 8) & 0xFF00)
					| (sad1_4 & 0x00FF);
	/* SYNCLO    | [RSVD3(0)        or  sample_width    ] */
	header[5] = ((MOST_DTCP_SAD1_SYNC_LO << 8) & 0xFF00)
					| (sad1_5 & 0x00FF);
	/* DTCP_INFO | [RSVD4(0)        or  unused(0)       ] */
	header[6] = (dtcp_info << 8) & 0xFF00;
	/* EXTN(0)   | [ASAD            or  ASAD            ] */
	header[7] = asad & 0x00FF;
	dump_header(header);
}

static uint32_t
sad_buffer_create(uint16_t *buffer, uint16_t *header, uint32_t sad_mode)
{
	uint32_t i, block_size = 0;
	uint16_t sad_fill_data[MOST_DTCP_SAD_SBFRM_LENGTH];

	if (!header || !buffer) {
		pr_err("Invalid params header:%p buffer: %p\n", header, buffer);
		return -1;
	}

	for (i = 0; i < MOST_DTCP_SAD_SBFRM_LENGTH; i++)
		sad_fill_data[i] = (sad_mode == SAD_SUBFRAME_MODE)
					? (i + 1) : 0;

	for (i = 0; i < MOST_DTCP_SAD_HDR_LENGTH; i++) {

		buffer[block_size] = header[i];
		block_size++;

		memcpy(buffer + block_size, sad_fill_data,
			MOST_DTCP_SAD_SBFRM_LENGTH * sizeof(uint16_t));
		block_size += MOST_DTCP_SAD_SBFRM_LENGTH;
	}

	dump_buffer(buffer, block_size);
	return block_size;
}

int32_t
sad_update(sad_context_t *s, sad_metadata_t *m,
		uint32_t cmd, uint8_t *msg, uint16_t msg_size)
{
	switch (cmd) {
	case DCMD_PCM_GET_SAD_METADATA:
		memcpy(msg, m, msg_size);
		dump_metadata(m);
		break;
	case DCMD_PCM_SET_SAD_METADATA:
		memcpy(m, msg, sizeof(*m));
		break;
	default:
		return 0;
	}

	return 0;
}

int32_t
sad_prepare(sad_context_t *s, sad_metadata_t *metadata)
{
	int8_t *pdst = NULL;
	uint32_t block_size = 0, num_blocks = 0, block_size_in_bytes;

	if (!s) {
		pr_err("Invalid params for sad update\n");
		return -EAGAIN;
	}

	sad_header_create(s->sad_header, metadata, s->sad_mode);

	block_size = sad_buffer_create(s->sad_block, s->sad_header,
			s->sad_mode);
	if (block_size < 0) {
		pr_err("sad_prepare failed\n");
		return -1;
	}

	block_size_in_bytes = block_size * sizeof(uint16_t);

	pdst = s->buffer_addr;
	num_blocks = s->buffer_size / block_size_in_bytes;
	while (num_blocks-- > 0) {
		memcpy(pdst, s->sad_block, block_size_in_bytes);
		pdst += block_size_in_bytes;
	}

	return 0;
}

static void sad_dma_channel_init(sad_context_t *s)
{
	uint32_t config, ctrl, fifo_ctrl, wcount;

	volatile uint8_t *ch_base = (uint8_t *)s->dma_addr;

	wcount = s->period_size;
	ctrl = readl(ch_base + ADMA_CH_CTRL);
	ctrl &= ~ADMA_CH_CTRL_TRANSFER_MODE_MASK;
	ctrl |= ADMA_MODE_CONTINUOUS << ADMA_CH_CTRL_TRANSFER_MODE_SHIFT;
	ctrl |= ADMA_CH_CTRL_FLOWCTRL_ENABLE;

	config = readl(ch_base + ADMA_CH_CONFIG);
	config &= ~ADMA_CH_CONFIG_BURST_SIZE_MASK;
	config |= WORDS_4 << ADMA_CH_CONFIG_BURST_SIZE_SHIFT;

	fifo_ctrl = readl(ch_base + ADMA_CH_AHUB_FIFO_CTRL);
	fifo_ctrl &= ~(1 << ADMA_CH_AHUB_FIFO_CTRL_FETCHING_POLICY_SHIFT);
	fifo_ctrl |= BURST_BASED
		<< ADMA_CH_AHUB_FIFO_CTRL_FETCHING_POLICY_SHIFT;

	ctrl &= ~ADMA_CH_CTRL_TRANSFER_DIRECTION_MASK;
	ctrl |= MEMORY_TO_AHUB << ADMA_CH_CTRL_TRANSFER_DIRECTION_SHIFT;

	ctrl &= ~ADMA_CH_CTRL_TX_REQUEST_SELECT_MASK;
	ctrl |= (s->admaif_id + 1) << ADMA_CH_CTRL_TX_REQUEST_SELECT_SHIFT;

	config &= ~(ADMA_CH_CONFIG_SOURCE_MEMORY_BUFFER_MASK);
	config |= (s->num_periods - 1)
		<< ADMA_CH_CONFIG_SOURCE_MEMORY_BUFFER_SHIFT;

	writel(s->buffer_phys_addr, ch_base + ADMA_CH_LOWER_SOURCE_ADDR);

	writel(wcount, ch_base + ADMA_CH_TC);
	writel(fifo_ctrl, ch_base + ADMA_CH_AHUB_FIFO_CTRL);
	writel(config, ch_base + ADMA_CH_CONFIG);
	writel(ctrl, ch_base + ADMA_CH_CTRL);

}

static void sad_dma_channel_enable(sad_context_t *s)
{
	volatile uint8_t *ch_base = (uint8_t *)s->dma_addr;

	writel(1, ch_base + ADMA_CH_CMD);
}

static int32_t is_sad_dma_enabled(sad_context_t *s)
{
	volatile uint8_t *ch_base = (uint8_t *)s->dma_addr;
	uint32_t csts;

	csts = readl(ch_base + ADMA_CH_STATUS);
	csts &= ADMA_CH_STATUS_TRANSFER_ENABLED;
	return csts;
}

static void sad_dma_channel_disable(sad_context_t *s)
{
	volatile uint8_t *ch_base = (uint8_t *)s->dma_addr;
	uint32_t status, dcnt = 10;

	/* Disable interrupts  */
	writel(1, ch_base + ADMA_CH_INT_CLEAR);

	/* Disable ADMA */
	writel(0, ch_base + ADMA_CH_CMD);

	/* Clear interrupt status if it is there */
	status = readl(ch_base + ADMA_CH_INT_STATUS);
	if (status & ADMA_CH_INT_TD_STATUS)
		writel(1, ch_base + ADMA_CH_INT_CLEAR);

	while (is_sad_dma_enabled(s) && dcnt--)
		udelay(TEGRA_ADMA_BURST_COMPLETE_TIME);

	if (is_sad_dma_enabled(s))
		pr_err("Stop failed for channel %d", s->dma_id);

}

int32_t
sad_init(sad_context_t *s, sad_metadata_t *m, struct device *dev)
{
	uint32_t dma_chan_addr;

	s->num_periods = SAD_NUM_PERIODS;
	s->period_size = SAD_PERIOD_SIZE;

	s->buffer_size = s->period_size * s->num_periods;

	/* allocate shm buffer */
	s->buffer_addr = dma_alloc_coherent(dev, s->buffer_size,
				&s->buffer_phys_addr, GFP_KERNEL);
	if (!s->buffer_addr) {
		pr_err("Failed to alocate sad buffer\n");
		return -1;
	}

	/* populate defaults  */
	sad_header_reset(s, m);

	/* get adma resource corresponding to dma id */
	dma_chan_addr = ADMA_PAGE1_BASE + (s->dma_ch_page * ADMA_PAGE_SIZE)
					+ (s->dma_id *  CH_REG_SIZE);
	s->dma_addr = ioremap_nocache(dma_chan_addr, CH_REG_SIZE);
	if (!s->dma_addr) {
		pr_err("Unable to map dma channel for sad\n");
		return -1;
	}

	return 0;
}

static int32_t
sad_setup_admaif_cif(sad_context_t *s, struct device *dev)
{
	struct nvaudio_ivc_ctxt *hivc_client =
		nvaudio_ivc_alloc_ctxt(dev);
	uint32_t err, value = 0;
	struct nvaudio_ivc_msg msg;
	struct tegra210_virt_audio_cif cif_conf;

	cif_conf.threshold = 0;
	cif_conf.audio_channels = MOST_DTCP_SAD_SBFRM_LENGTH + 1;
	cif_conf.client_channels = MOST_DTCP_SAD_SBFRM_LENGTH + 1;
	cif_conf.client_bits = TEGRA210_AUDIOCIF_BITS_16;
	cif_conf.audio_bits = TEGRA210_AUDIOCIF_BITS_32;

	value = ((cif_conf.threshold <<
			TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_SHIFT) |
		((cif_conf.audio_channels - 1) <<
			TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_SHIFT) |
		((cif_conf.client_channels - 1) <<
			TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_SHIFT) |
		(cif_conf.audio_bits <<
			TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT) |
		(cif_conf.client_bits <<
			TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT));

	memset(&msg, 0, sizeof(struct nvaudio_ivc_msg));
	msg.params.dmaif_info.id = s->admaif_id;
	msg.params.dmaif_info.value = value;
	msg.cmd = NVAUDIO_DMAIF_SET_TXCIF;

	err = nvaudio_ivc_send_retry(hivc_client,
			&msg,
			sizeof(struct nvaudio_ivc_msg));
	if (err < 0) {
		pr_err("%s: Timedout on ivc_send_retry\n", __func__);
		return err;
	}

	return 0;
}

static int32_t
sad_toggle_admaif_playback(sad_context_t *s, struct device *dev)
{
	struct nvaudio_ivc_ctxt *hivc_client =
		nvaudio_ivc_alloc_ctxt(dev);
	int err;
	struct nvaudio_ivc_msg msg;

	memset(&msg, 0, sizeof(struct nvaudio_ivc_msg));
	msg.cmd = s->enable_sad_flood ?
			NVAUDIO_START_PLAYBACK : NVAUDIO_STOP_PLAYBACK;
	msg.params.dmaif_info.id = s->admaif_id;

	err = nvaudio_ivc_send_retry(hivc_client,
			&msg,
			sizeof(struct nvaudio_ivc_msg));
	if (err < 0) {
		pr_err("%s: Timedout on ivc_send_retry\n", __func__);
		return err;
	}

	return 0;
}

int32_t
sad_flood_init(sad_context_t *s, struct device *dev)
{
	sad_metadata_t sad_metadata;

	if (sad_init(s, &sad_metadata, dev))
		return -1;

	sad_dma_channel_init(s);

	sad_metadata.media_type = MEDIA_TYPE_UNDEF; /* undefined media type */
	sad_metadata.pcm_conf = PCM_CONF_314;       /* Eight channel */
	sad_metadata.ca_method = CA_METHOD_NONE;    /* No encryption */
	sad_metadata.codec_type = 0;                /* Unknown codec type */
	sad_metadata.sample_width = 24;             /* sample_width */
	sad_metadata.sample_rate = 48000;           /* sample_rate */
	sad_metadata.bit_rate = 0;                  /* bit rate */

	pr_info("SAD Mode: %s, ADMAIF: %d, DMA channel: %d\n",
			s->sad_mode ? "SAD Header" : "Subframe", s->admaif_id,
					s->dma_id);

	sad_prepare(s, &sad_metadata);

	if (sad_setup_admaif_cif(s, dev))
		return -1;

	s->init_sad_flood = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(sad_flood_init);


int32_t
sad_flood_deinit(sad_context_t *s, struct device *dev)
{
	if (s->buffer_addr) {
		dma_free_coherent(dev, s->buffer_size, s->buffer_addr,
				s->buffer_phys_addr);
		s->buffer_addr = NULL;
	}

	s->init_sad_flood = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(sad_flood_deinit);

int32_t
sad_flood_enable(sad_context_t *s, uint32_t enable, struct device *dev)
{
	if (enable)
		sad_dma_channel_enable(s);
	else
		sad_dma_channel_disable(s);

	s->enable_sad_flood = enable;

	if (sad_toggle_admaif_playback(s, dev))
		return -1;

	return 0;
}
EXPORT_SYMBOL_GPL(sad_flood_enable);

MODULE_LICENSE("GPL");
