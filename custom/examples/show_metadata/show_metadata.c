/**
 * @file libavformat metadata extraction API usage example
 * @example show_metadata.c
 *
 * Show metadata from an input file.
 */

#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <stdio.h>

int main(int argc, char **argv) {
  AVFormatContext *fmt_ctx = NULL;
  const AVDictionaryEntry *tag = NULL;
  int ret;

  if (argc != 2) {
    printf(
        "usage: %s <input_file>\n"
        "example program to demonstrate the use of the libavformat metadata API.\n"
        "\n",
        argv[0]);
    return 1;
  }

  if ((ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL)))
    return ret;

  if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
    return ret;
  }

  while ((tag = av_dict_iterate(fmt_ctx->metadata, tag)))
    printf("%s=%s\n", tag->key, tag->value);

  avformat_close_input(&fmt_ctx);
  return 0;
}
