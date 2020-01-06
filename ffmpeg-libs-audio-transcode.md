##### [digital-domain.net](https://digital-domain.net/)

# Using the ffmpeg-libs C API to transcode audio

Recently, a C project required me to convert audio files into WAVE audio for further processing.

Now this could have just been a fairly quick job doing a [fork(2)](http://man7.org/linux/man-pages/man2/fork.2.html)/[exec(3)](http://man7.org/linux/man-pages/man3/exec.3.html) or some such on the ffmpeg binary. However there is a right & wrong way to do things and certainly in this case that was just wrong.

The **right** way was to use the various libraries provided by [ffmpeg](https://www.ffmpeg.org/); libav*.

This turned out to take much longer than expected. Apart from the ffmpeg docs themselves (which seeing as they are essentially just the API documentation, do kind of assume you know what you are doing in the first place) there are many examples, however they are generally doing quite a lot and not so great when you just want to see how to do some specific task.

However there was one example that did show some handy stuff [avio_reading.c](https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/avio_reading.c)

The other main source of help in working this out was this [blog post](https://rodic.fr/blog/libavcodec-tutorial-decode-audio-file/).

So with those in hand this is what we ended up with. Hopefully by presenting
the following which has been built on top of the above it will save some
head scratching in the future.

This was needing to work with already open file descriptors, hence the way some things are done the way they are below...

I have also removed all error checking to help with readability. The full sources are available [here](https://github.com/ac000/ffmpeg-libs-transcode/).

**NOTE:** The various ffmpeg libraries are constantly changing and this currently builds cleanly with at least ffmpeg-libs 4.1

So without further ado.


Lets get the basic stuff out of the way, include all the required headers

```c
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
```

Then a couple of #defines

```c
#define WAVE_SAMPLE_RATE        16000
#define AVIO_CTX_BUF_SZ          4096
```

The first one specifies the desired sample rate of the output WAVE audio.

Next comes the definition of the WAVE header, there is plenty of documentation on this, so nothing more will be said

```c
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

```

This is used by ffmpeg while reading the audio data

```c
struct audio_buffer {
        u8 *ptr;
        int size; /* size left in the buffer */
};
```

Now for the functions. This first one writes out the wave header

```c
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
```

Next we mmap(2) the input audio file as we need to operate on this as a memory buffer

```c
static int map_file(int fd, u8 **ptr, size_t *size)
{
        struct stat sb;

        fstat(fd, &sb);
        *size = sb.st_size;

        *ptr = mmap(NULL, *size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);

        return 0;
}
```

This function uses our *audio_buffer* from above to read in the audio data and is hooked up in *decode_audio()*

```c
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
```

audio\_buf->ptr is our mmap'ed file. This next function is actually the core one responsible for doing the actual audio conversion, it's called in a loop (from decode_audio()) operating on a frame of data at a time

```c
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
```

Next up a simple helper function, this was done really just to help with the readability of the code...

```c
static bool is_audio_stream(const AVStream *stream)
{
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                return true;

        return false;
}
```

Now for the main driver of the whole thing, we initialise various ffmpeg  data structures, configure the WAVE output parameters and do the audio conversion

```c
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

        avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
        avformat_find_stream_info(fmt_ctx, NULL);

        for (i = 0; i < fmt_ctx->nb_streams; i++) {
                if (is_audio_stream(fmt_ctx->streams[i])) {
                        stream_index = i;
                        break;
                }
        }

        stream = fmt_ctx->streams[stream_index];
        codec = avcodec_alloc_context3(
                        avcodec_find_decoder(stream->codecpar->codec_id));
        avcodec_parameters_to_context(codec, stream->codecpar);
        avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), NULL);

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
        av_init_packet(&packet);
        frame = av_frame_alloc();

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
```

Now for a simple main() function to drive the above

```c
int main(int argc, char *argv[])
{
        struct audio_buffer audio_buf;
        size_t buf_size;
        s16 *data;
        u8 *buf;
        int size;
        int ifd;
        int ofd;

        if (argc < 3) {
                fprintf(stderr, "Usage: transcode in_file out_wave\n");
                exit(EXIT_FAILURE);
        }

        ifd = open(argv[1], O_RDONLY);
        ofd = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0666);

        map_file(ifd, &buf, &buf_size);

        audio_buf.ptr = buf;
        audio_buf.size = buf_size;

        decode_audio(&audio_buf, &data, &size);

        write_wave_hdr(ofd, size * sizeof(s16));
        write(ofd, data, size * sizeof(s16));

        free(data);
        munmap(buf, buf_size);

        close(ifd);
        close(ofd);

        exit(EXIT_SUCCESS);
}
```

As mentioned above, that builds with ffmpeg-libs 4.1+, to build with older ffmpeg-libs, like 2.8, you need the below patch (also in the repository)

```diff
diff --git a/transcode.c b/transcode-old.c
index 27b6277..b142982 100644
--- a/transcode.c
+++ b/transcode-old.c
@@ -155,7 +155,7 @@ static void convert_frame(struct SwrContext *swr, AVCodecContext *codec,
 
 static bool is_audio_stream(const AVStream *stream)
 {
-       if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
+       if (stream->codec->codec_type == AVMEDIA_TYPE_AUDIO)
                return true;
 
        return false;
@@ -175,6 +175,8 @@ static int decode_audio(struct audio_buffer *audio_buf, s16 **data, int *size)
        int stream_index = -1;
        int err;
 
+       av_register_all();
+
        fmt_ctx = avformat_alloc_context();
        avio_ctx_buffer = av_malloc(AVIO_CTX_BUF_SZ);
        avio_ctx = avio_alloc_context(avio_ctx_buffer, AVIO_CTX_BUF_SZ, 0,
@@ -208,9 +210,7 @@ static int decode_audio(struct audio_buffer *audio_buf, s16 **data, int *size)
        }
 
        stream = fmt_ctx->streams[stream_index];
-       codec = avcodec_alloc_context3(
-                       avcodec_find_decoder(stream->codecpar->codec_id));
-       avcodec_parameters_to_context(codec, stream->codecpar);
+       codec = stream->codec;
        err = avcodec_open2(codec, avcodec_find_decoder(codec->codec_id),
                                                        NULL);
        if (err) {
@@ -250,10 +250,10 @@ static int decode_audio(struct audio_buffer *audio_buf, s16 **data, int *size)
        *data = NULL;
        *size = 0;
        while (av_read_frame(fmt_ctx, &packet) >= 0) {
-               avcodec_send_packet(codec, &packet);
+               int got_frame;
 
-               err = avcodec_receive_frame(codec, frame);
-               if (err == AVERROR(EAGAIN))
+               avcodec_decode_audio4(codec, frame, &got_frame, &packet);
+               if (!got_frame)
                        continue;
 
                convert_frame(swr, codec, frame, data, size, false);
```

---
[Andrew Clayton](mailto:Andrew Clayton <andrew@digital-domain.net>), Jan 5th 2020
