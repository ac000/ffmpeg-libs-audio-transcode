transcode: transcode.c
	$(CC) -Wall -Wextra -g -O2 $(shell pkg-config --cflags libavcodec libavformat libavutil libswresample) -o $@ $< $(shell pkg-config --libs libavcodec libavformat libavutil libswresample)

html: ffmpeg-libs-audio-transcode.md
	pandoc $< --metadata pagetitle=ffmpeg-libs-audio-transcode -s --highlight-style tango -o ffmpeg-libs-audio-transcode.html

clean:
	rm -f transcode ffmpeg-libs-audio-transcode.html
