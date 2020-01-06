/* SPDX-License-Identifier: GPL-2.0 */

/*
 * transcode.c - convert audio file to WAVE
 *
 * Copyright (C) 2019		Andrew Clayton <andrew@digital-domain.net>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include "short_types.h"

#define WAVE_SAMPLE_RATE	16000
#define AVIO_CTX_BUF_SZ		 4096

/*
 * WAVE file header based on definition from
 * https://gist.github.com/Jon-Schneider/8b7c53d27a7a13346a643dac9c19d34f
 *
 * We must ensure this structure doesn't have any holes or
 * padding so we can just map it straight to the WAVE data.
 */
struct wave_hdr {
	/* RIFF Header: "RIFF" */
	char riff_header[4];
	/* size of audio data + sizeof(struct wave_hdr) - 8 */
	int wav_size;
	/* "WAVE" */
	char wav_header[4];

	/* Format Header */
	/* "fmt " (includes trailing space) */
	char fmt_header[4];
	/* Should be 16 for PCM */
	int fmt_chunk_size;
	/* Should be 1 for PCM. 3 for IEEE Float */
	s16 audio_format;
	s16 num_channels;
	int sample_rate;
	/*
	 * Number of bytes per second
	 * sample_rate * num_channels * bit_depth/8
	 */
	int byte_rate;
	/* num_channels * bytes per sample */
	s16 sample_alignment;
	/* bits per sample */
	s16 bit_depth;

	/* Data Header */
	/* "data" */
	char data_header[4];
	/*
	 * size of audio
	 * number of samples * num_channels * bit_depth/8
	 */
	int data_bytes;
} __attribute__((__packed__));

struct audio_buffer {
	u8 *ptr;
	int size; /* size left in the buffer */
};

static void write_wave_hdr(int fd, size_t size)
{
	struct wave_hdr wh;

	memcpy(&wh.riff_header, "RIFF", 4);
	wh.wav_size = size + sizeof(struct wave_hdr) - 8;
	memcpy(&wh.wav_header, "WAVE", 4);
	memcpy(&wh.fmt_header, "fmt ", 4);
	wh.fmt_chunk_size = 16;
	wh.audio_format = 1;
	wh.num_channels = 1;
	wh.sample_rate = WAVE_SAMPLE_RATE;
	wh.sample_alignment = 2;
	wh.bit_depth = 16;
	wh.byte_rate = wh.sample_rate * wh.sample_alignment;
	memcpy(&wh.data_header, "data", 4);
	wh.data_bytes = size;

	write(fd, &wh, sizeof(struct wave_hdr));
}

static int map_file(int fd, u8 **ptr, size_t *size)
{
	struct stat sb;

	fstat(fd, &sb);
	*size = sb.st_size;

	*ptr = mmap(NULL, *size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (*ptr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	return 0;
}

static int read_packet(void *opaque, u8 *buf, int buf_size)
{
	struct audio_buffer *audio_buf = opaque;

	buf_size = FFMIN(buf_size, audio_buf->size);

	/* copy internal buffer data to buf */
	memcpy(buf, audio_buf->ptr, buf_size);
	audio_buf->ptr += buf_size;
	audio_buf->size -= buf_size;

	return buf_size;
}

static void convert_frame(struct SwrContext *swr, AVCodecContext *codec,
			  AVFrame *frame, s16 **data, int *size, bool flush)
{
	int nr_samples;
	s64 delay;
	u8 *buffer;

	delay = swr_get_delay(swr, codec->sample_rate);
	nr_samples = av_rescale_rnd(delay + frame->nb_samples,
				    WAVE_SAMPLE_RATE, codec->sample_rate,
				    AV_ROUND_UP);
	av_samples_alloc(&buffer, NULL, 1, nr_samples, AV_SAMPLE_FMT_S16, 0);

	/*
	 * !flush is used to check if we are flushing any remaining
	 * conversion buffers...
	 */
	nr_samples = swr_convert(swr, &buffer, nr_samples,
				 !flush ? (const u8 **)frame->data : NULL,
				 !flush ? frame->nb_samples : 0);

	*data = realloc(*data, (*size + nr_samples) * sizeof(s16));
	memcpy(*data + *size, buffer, nr_samples * sizeof(s16));
	*size += nr_samples;
	av_freep(&buffer);
}

static bool is_audio_stream(const AVStream *stream)
{
	if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		return true;

	return false;
}

static int decode_audio(struct audio_buffer *audio_buf, s16 **data, int *size)
{
	AVFormatContext *fmt_ctx;
	AVIOContext *avio_ctx;
	AVStream *stream;
	AVCodecContext *codec;
	AVPacket packet;
	AVFrame *frame;
	struct SwrContext *swr;
	u8 *avio_ctx_buffer;
	unsigned int i;
	int stream_index = -1;
	int err;

	fmt_ctx = avformat_alloc_context();
	avio_ctx_buffer = av_malloc(AVIO_CTX_BUF_SZ);
	avio_ctx = avio_alloc_context(avio_ctx_buffer, AVIO_CTX_BUF_SZ, 0,
				      audio_buf, &read_packet, NULL, NULL);
	fmt_ctx->pb = avio_ctx;

	err = avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
	if (err) {
		fprintf(stderr, "Could not read audio buffer\n");
		return -1;
	}

	err = avformat_find_stream_info(fmt_ctx, NULL);
	if (err < 0) {
		fprintf(stderr,
			"Could not retrieve stream info from audio buffer\n");
		return -1;
	}

	for (i = 0; i < fmt_ctx->nb_streams; i++) {
		if (is_audio_stream(fmt_ctx->streams[i])) {
			stream_index = i;
			break;
		}
	}

	if (stream_index == -1) {
		fprintf(stderr,
			"Could not retrieve audio stream from buffer\n");
		return -1;
	}

	stream = fmt_ctx->streams[stream_index];
	codec = avcodec_alloc_context3(
			avcodec_find_decoder(stream->codecpar->codec_id));
	avcodec_parameters_to_context(codec, stream->codecpar);
	err = avcodec_open2(codec, avcodec_find_decoder(codec->codec_id),
							NULL);
	if (err) {
		fprintf(stderr,
			"Failed to open decoder for stream #%d in audio buffer\n",
			stream_index);
		return -1;
	}

	/* prepare resampler */
	swr = swr_alloc();

	av_opt_set_int(swr, "in_channel_count", codec->channels, 0);
	av_opt_set_int(swr, "out_channel_count", 1, 0);
	av_opt_set_int(swr, "in_channel_layout", codec->channel_layout, 0);
	av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_MONO, 0);
	av_opt_set_int(swr, "in_sample_rate", codec->sample_rate, 0);
	av_opt_set_int(swr, "out_sample_rate", WAVE_SAMPLE_RATE, 0);
	av_opt_set_sample_fmt(swr, "in_sample_fmt", codec->sample_fmt, 0);
	av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

	swr_init(swr);
	if (!swr_is_initialized(swr)) {
		fprintf(stderr,
			"Resampler has not been properly initialized\n");
		return -1;
	}

	av_init_packet(&packet);
	frame = av_frame_alloc();
	if (!frame) {
		fprintf(stderr, "Error allocating the frame\n");
		return -1;
	}

	/* iterate through frames */
	*data = NULL;
	*size = 0;
	while (av_read_frame(fmt_ctx, &packet) >= 0) {
		avcodec_send_packet(codec, &packet);

		err = avcodec_receive_frame(codec, frame);
		if (err == AVERROR(EAGAIN))
			continue;

		convert_frame(swr, codec, frame, data, size, false);
	}
	/* Flush any remaining conversion buffers... */
	convert_frame(swr, codec, frame, data, size, true);

	av_frame_free(&frame);
	swr_free(&swr);
	avcodec_close(codec);
	avformat_close_input(&fmt_ctx);
	avformat_free_context(fmt_ctx);

	if (avio_ctx) {
		av_freep(&avio_ctx->buffer);
		av_freep(&avio_ctx);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct audio_buffer audio_buf;
	size_t buf_size;
	s16 *data;
	u8 *buf;
	int size;
	int ifd;
	int ofd;
	int err;

	if (argc < 3) {
		fprintf(stderr, "Usage: transcode in_file out_wave\n");
		exit(EXIT_FAILURE);
	}

	ifd = open(argv[1], O_RDONLY);
	if (ifd == -1) {
		fprintf(stderr, "Couldn't open input file\n");
		exit(EXIT_FAILURE);
	}

	ofd = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0666);

	err = map_file(ifd, &buf, &buf_size);
	if (err)
		exit(EXIT_FAILURE);

        audio_buf.ptr = buf;
        audio_buf.size = buf_size;

	err = decode_audio(&audio_buf, &data, &size);
	if (err)
		exit(EXIT_FAILURE);

	write_wave_hdr(ofd, size * sizeof(s16));
	write(ofd, data, size * sizeof(s16));

	free(data);
	munmap(buf, buf_size);

	close(ifd);
	close(ofd);

	exit(EXIT_SUCCESS);
}
