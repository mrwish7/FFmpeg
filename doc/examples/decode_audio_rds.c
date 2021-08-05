/*
 * Copyright (c) 2001 Fabrice Bellard
 * Copyright (c) 2021 Carsten Gross
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * audio decoding with RDS FrameSide data libavcodec API example
 *
 * @example decode_audio.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>

#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

/*
 *  RDS - radio data system stuff to present RDS text messages 
 *
 */

/* Convert EBU table 1 to latin 1
 * Taken out of http://www.interactive-radio-system.com/docs/EN50067_RDS_Standard.pdf 
 * non latin1 characters are translated to "." */

static uint8_t ebutable1[] = {
    /* 0    1    2    3    4    5    6    7    8    9    a    b    c    d    e    f     */
    0xe1,0xe0,0xe9,0xe8,0xed,0xec,0xf3,0xf2,0xfa,0xf9,0xd1,0xc7,0x2e,0xdf,0xa1,0x2e,    /* 0x80 - 0x8f */
    0xe2,0xe4,0xea,0xeb,0xee,0xef,0xf4,0xf6,0xfb,0xfc,0xf1,0xe7,0x2e,0x2e,0x2e,0x2e,    /* 0x90 - 0x9f */
    0xaa,0x2e,0xa9,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x24,0x2e,0x2e,0x2e,0x2e,    /* 0xa0 - 0xaf */
    0xba,0xb9,0xb2,0xb3,0xb1,0x2e,0x2e,0x2e,0xb5,0xbf,0xf7,0xb0,0xbc,0xbd,0xbe,0xa7,    /* 0xb0 - 0xbf */
    0xc1,0xc0,0xc9,0xc8,0xcd,0xcc,0xd3,0xd2,0xda,0xd9,0x2e,0x2e,0x2e,0x2e,0xd0,0x2d,    /* 0xc0 - 0xcf */
    0xc2,0xc4,0xca,0xcb,0xce,0xcf,0xd4,0xd6,0xdb,0xdc,0x2d,0x2e,0x2e,0x2e,0x2e,0x2e,    /* 0xd0 - 0xdf */
    0xc3,0xc5,0xc6,0x2e,0x2e,0xdd,0xd5,0xd8,0xde,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0xf0,    /* 0xe0 - 0xef */
    0xe3,0xe5,0xe6,0x2e,0x2e,0xfd,0xf5,0xf8,0xfe,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,0x2e,    /* 0xf0 - 0xff */
};

static uint8_t ebu2latin1(uint8_t character) {
    if (character < 0x20) {
        if (character == 0x0d || character == 0x0a) {
            return ' ';
        }
        return '.';
    }
    if (character >= 0x80) {
        return ebutable1[(character & 0x7f)];
    }
    return character;
}

static char * handle_rt(uint8_t* rds_message, uint8_t size) {
    uint8_t msg_len = rds_message[7];
    uint8_t i;
	static char radiotext[128];
    /* Radiotext consists of two message parts with 64 characters each
     * indexed by an index being either 0 or 1, we ignore this for this
     * demonstration code */
    if (msg_len > 0x41) {
        msg_len = 0x41;
    }
    /* Cleanup old message */
    if (msg_len > 0) {
        for (i = msg_len - 1; i < 0x40; i++) {
            radiotext[i] = ' ';
        }
    }
    /* Check and convert message to latin1 */
    for (i = 9; i < 8 + msg_len; i++) {
        /* is some character different? */
        radiotext[i - 9] = ebu2latin1(rds_message[i]);
    }
	return radiotext;
}

/* Handle a RDS data chunk. */
static void rds_handle_message(uint8_t* rds_message, uint8_t size, uint64_t audio_frame) {
    uint8_t type = rds_message[4];
	char * text;
    switch (type) {
        case 0x0a: // RT (Radiotexta)
			// DumpHex(rds_message, size);
            text = handle_rt(rds_message, size);
			fprintf(stderr, "RT (Frame#%ld): %.64s\n", audio_frame, text);
            break;
        case 0x02:
            //handle_ps(rds_message, size); 
            break;
    }
}

static void rds_convert(uint8_t* buffer, size_t size, uint64_t audio_frame) {
    static uint8_t current_pos = 0;
    static uint8_t rds_message[255];
    int32_t  i = 0;
    uint8_t rds_data_size = buffer[2];
    if (rds_data_size == 0) {
        return;
    }
    for (i = 0; i < size; i++) {
        uint8_t mychar = buffer[i];
        if (mychar == 0xfd) {
            /* special marker: 0xfd 0x01 means 0xfe, 0xfd 0x02 means 0xff */
            i++;
            rds_message[current_pos] = mychar + buffer[i];
            current_pos ++;
        } else {
            rds_message[current_pos] = mychar;
            current_pos ++;
        }
    }
	rds_handle_message(rds_message, current_pos, audio_frame);
    current_pos = 0;
    return;
}

static void decode(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame,
                   FILE *outfile)
{
    int i, ch;
    int ret, data_size;
	static int64_t frame_nr = 0;
    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
		AVFrameSideData* sd = NULL;
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
		frame_nr++;
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            fprintf(stderr, "Failed to calculate data size\n");
            exit(1);
        }
		sd = av_frame_get_side_data(frame, AV_FRAME_DATA_RDS_DATA_PACKET);
        if (sd) {
			if ( sd->size >= 2) {
				rds_convert(sd->data + 1, sd->size - 2, frame_nr);
			}
		}
        for (i = 0; i < frame->nb_samples; i++)
            for (ch = 0; ch < dec_ctx->channels; ch++)
                fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
    }
}

int main(int argc, char **argv)
{
    const char *outfilename, *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVCodecParserContext *parser = NULL;
    int len, ret;
    FILE *f, *outfile;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    AVPacket *pkt;
    AVFrame *decoded_frame = NULL;
    enum AVSampleFormat sfmt;
    int n_channels = 0;
    const char *fmt;

    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }
    filename    = argv[1];
    outfilename = argv[2];

    pkt = av_packet_alloc();

    /* find the MPEG audio decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC_LATM);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "Parser not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }
    outfile = fopen(outfilename, "wb");
    if (!outfile) {
        av_free(c);
        exit(1);
    }

    /* decode until eof */
    data      = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, f);
	
	if (data_size < 3) {
		fprintf(stderr, "File to short\n");
		exit(1);
	}
    while (data_size > 0) {
        if (!decoded_frame) {
            if (!(decoded_frame = av_frame_alloc())) {
                fprintf(stderr, "Could not allocate audio frame\n");
                exit(1);
            }
        }

        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                               data, data_size,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            fprintf(stderr, "Error while parsing\n");
            exit(1);
        }
        data      += ret;
        data_size -= ret;

        if (pkt->size)
            decode(c, pkt, decoded_frame, outfile);

        if (data_size < AUDIO_REFILL_THRESH) {
            memmove(inbuf, data, data_size);
            data = inbuf;
            len = fread(data + data_size, 1,
                        AUDIO_INBUF_SIZE - data_size, f);
            if (len > 0)
                data_size += len;
        }
    }

    /* flush the decoder */
    pkt->data = NULL;
    pkt->size = 0;
    decode(c, pkt, decoded_frame, outfile);

    /* print output pcm infomations, because there have no metadata of pcm */
    sfmt = c->sample_fmt;

    if (av_sample_fmt_is_planar(sfmt)) {
        const char *packed = av_get_sample_fmt_name(sfmt);
        printf("Warning: the sample format the decoder produced is planar "
               "(%s). This example will output the first channel only.\n",
               packed ? packed : "?");
        sfmt = av_get_packed_sample_fmt(sfmt);
    }

    n_channels = c->channels;
    if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
        goto end;

    printf("Play the output audio file with the command:\n"
           "ffplay -f %s -ac %d -ar %d %s\n",
           fmt, n_channels, c->sample_rate,
           outfilename);
end:
    fclose(outfile);
    fclose(f);

    avcodec_free_context(&c);
    av_parser_close(parser);
    av_frame_free(&decoded_frame);
    av_packet_free(&pkt);

    return 0;
}
