/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include <SDL.h>
#include <SDL_thread.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>

#include "cmdutils.h"
#include "config.h"
#include "config_components.h"
#include "libavdevice/avdevice.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/eval.h"
#include "libavutil/fifo.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/tx.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "opt_common.h"

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define SDL_AUDIO_MIN_BUFFER_SIZE 512       // 最小音频缓冲
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30  // 计算实际的缓冲区大小，记住不要引起太频繁的音频回调

#define SDL_VOLUME_STEP (0.75)  // 音量控制的步长(以dB为单位)

#define AV_SYNC_THRESHOLD_MIN 0.04      // 最低同步阈值，如果低于该值，则不需要同步校正
#define AV_SYNC_THRESHOLD_MAX 0.1       // 最大同步阈值，如果大于该值，则需要做同步校正
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1  // 如果一个帧的持续时间超过这个长度，将不会被复制来补偿AV同步
#define AV_NOSYNC_THRESHOLD 10.0        // 如果误差太大，则不进行AV校正

#define SAMPLE_CORRECTION_PERCENT_MAX 10  // 最大音频速度变化，以获得正确的同步

/* 根据实时码流的缓冲区填充时间做外部时钟调整 */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900   // 最小值
#define EXTERNAL_CLOCK_SPEED_MAX 1.010   // 最大值
#define EXTERNAL_CLOCK_SPEED_STEP 0.001  // 步长

#define AUDIO_DIFF_AVG_NB 20  // 使用 A-V 差来求平均值

#define REFRESH_RATE 0.01  // 屏幕刷新频率，应该小于1/fps

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)  // 采样大小

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

typedef struct MyAVPacketList {
  AVPacket *pkt;
  int serial;
} MyAVPacketList;

typedef struct PacketQueue {
  AVFifo *pkt_list;   //
  int nb_packets;     // 队列中 packet 的数量
  int size;           // 队列所占内存空间大小
  int64_t duration;   // 队列中所有packet总的播放时长
  int abort_request;  // 中断请求
  int serial;         // 播放序列，所谓播放序列就是一段连续的播放动作，一个seek操作会启动一段新的播放序列
  SDL_mutex *mutex;   //
  SDL_cond *cond;     //
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
  int freq;                   // 频率
  AVChannelLayout ch_layout;  // 声道布局
  enum AVSampleFormat fmt;    // 采样格式
  int frame_size;             // 采样大小
  int bytes_per_sec;          // 码率，每秒多少字节
} AudioParams;

typedef struct Clock {
  double pts;           // 当前帧(待播放)显示时间戳，播放后，当前帧变成上一帧
  double pts_drift;     // 当前帧显示时间戳与当前系统时钟时间的差值
  double last_updated;  // 当前时钟(如视频时钟)最后一次更新时间，也可称当前时钟时间
  double speed;         // 时钟速度控制，用于控制播放速度
  int serial;           // 播放序列
  int paused;           // 暂停标志
  int *queue_serial;    // 指向当前包队列序列的指针，用于过时时钟检测
} Clock;

typedef struct FrameData {
  int64_t pkt_pos;
} FrameData;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
  AVFrame *frame;
  AVSubtitle sub;
  int serial;
  double pts;       // 帧的显示时间戳
  double duration;  // 帧的估计持续时间
  int64_t pos;      // 帧在输入文件中的字节位置
  int width;
  int height;
  int format;
  AVRational sar;
  int uploaded;
  int flip_v;  // 翻转
} Frame;

typedef struct FrameQueue {
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex;        // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
  int windex;        // 写索引
  int size;          // 总帧数
  int max_size;      // 队列可存储最大帧数
  int keep_last;     // 是否保留已播放的最后一帧使能标志
  int rindex_shown;  // 是否保留已播放的最后一帧实现手段
  SDL_mutex *mutex;
  SDL_cond *cond;
  PacketQueue *pktq;  // 指向对应的packet_queue
} FrameQueue;

enum {
  AV_SYNC_AUDIO_MASTER,    // 视频同步于音频，默认选项
  AV_SYNC_VIDEO_MASTER,    // 音频同步于视频
  AV_SYNC_EXTERNAL_CLOCK,  // 音视频同步于外部时钟
};

/* 解码管理器 */
typedef struct Decoder {
  AVPacket *pkt;               // 包数据
  PacketQueue *queue;          // 包队列
  AVCodecContext *avctx;       // 解码器上下文
  int pkt_serial;              // 序列
  int finished;                // 是否已经结束
  int packet_pending;          // 是否有包在等待
  SDL_cond *empty_queue_cond;  //
  int64_t start_pts;           // 起始时间戳
  AVRational start_pts_tb;     // 起始时基
  int64_t next_pts;            // 下个包时间戳
  AVRational next_pts_tb;      // 下个包时基
  SDL_Thread *decoder_tid;     // 解码线程
} Decoder;

/* 全局管理器 */
typedef struct VideoState {
  SDL_Thread *read_tid;          // 解复用线程
  const AVInputFormat *iformat;  // 输入源格式
  int abort_request;             // 中断请求
  int force_refresh;             // 强制刷新
  int paused;                    // 暂停
  int last_paused;               // 上次暂停状态
  int queue_attachments_req;
  int seek_req;      // 标识一次SEEK请求
  int seek_flags;    // SEEK标志，诸如AVSEEK_FLAG_BYTE等
  int64_t seek_pos;  // SEEK的目标位置(当前位置+增量)
  int64_t seek_rel;  // 本次SEEK的位置增量
  int read_pause_return;
  AVFormatContext *ifmt_ctx;  // 输入源格式上下文
  int realtime;               // 是否实时码流

  Clock audclk;  // 视频帧缓冲队列
  Clock vidclk;  // 字幕帧缓冲队列
  Clock extclk;  // 音频帧缓冲队列

  FrameQueue pictq;  // 视频帧缓冲队列
  FrameQueue subpq;  // 字幕帧缓冲队列
  FrameQueue sampq;  // 音频帧缓冲队列

  Decoder auddec;  // 音频解码器
  Decoder viddec;  // 视频解码器
  Decoder subdec;  // 字幕解码器

  int audio_stream;  // 音频流索引

  int av_sync_type;  // 时钟同步类型，默认为 AV_SYNC_AUDIO_MASTER 视频同步于音频

  double audio_clock;      // 每个音频帧更新一下此值，以pts形式表示
  int audio_clock_serial;  // 播放序列，seek可改变此值
  double audio_diff_cum;   // 用于AV差异平均计算
  double audio_diff_avg_coef;
  double audio_diff_threshold;          // 音频差分阈值
  int audio_diff_avg_count;             // 平均差分阈值
  AVStream *audio_st;                   // 音频流
  PacketQueue audioq;                   // 音频packet队列
  int audio_hw_buf_size;                // SDL音频缓冲区大小(单位字节)
  uint8_t *audio_buf;                   // 指向待播放的一帧音频数据，指向的数据区将被拷入SDL音频缓冲区。若经过重采样则指向audio_buf1，否则指向frame中的音频
  uint8_t *audio_buf1;                  // 音频重采样的输出缓冲区
  unsigned int audio_buf_size;          // 待播放的一帧音频数据(audio_buf指向)的大小 in bytes
  unsigned int audio_buf1_size;         // 申请到的音频缓冲区audio_buf1的实际尺寸
  int audio_buf_index;                  // 当前音频帧中已拷入SDL音频缓冲区的位置索引(指向第一个待拷贝字节)
  int audio_write_buf_size;             // 当前音频帧中尚未拷入SDL音频缓冲区的数据量，audio_buf_size = audio_buf_index + audio_write_buf_size
  int audio_volume;                     // 音量
  int muted;                            // 静音状态
  struct AudioParams audio_src;         // 音频帧参数
  struct AudioParams audio_filter_src;  // 音频过滤器源参数
  struct AudioParams audio_tgt;         // SDL支持的音频参数，重采样转换：audio_src->audio_tgt
  struct SwrContext *swr_ctx;           // 音频重采样context
  int frame_drops_early;                // 丢弃视频packet计数
  int frame_drops_late;                 // 丢弃视频frame计数

  enum ShowMode {
    SHOW_MODE_NONE = -1,
    SHOW_MODE_VIDEO = 0,  // 视频
    SHOW_MODE_WAVES,      // 音频波形
    SHOW_MODE_RDFT,       // 音频频谱
    SHOW_MODE_NB
  } show_mode;  // 显示模式
  int16_t sample_array[SAMPLE_ARRAY_SIZE];
  int sample_array_index;
  int last_i_start;
  AVTXContext *rdft;  // 自适应滤波器上下文
  av_tx_fn rdft_fn;
  int rdft_bits;  // 自适应比特率
  float *real_data;
  AVComplexFloat *rdft_data;
  int xpos;
  double last_vis_time;
  SDL_Texture *vis_texture;
  SDL_Texture *sub_texture;
  SDL_Texture *vid_texture;

  int subtitle_stream;    // 字幕流索引
  AVStream *subtitle_st;  // 字幕流
  PacketQueue subtitleq;  // 字幕packet队列

  double frame_timer;  // 记录最后一帧播放的时刻
  double frame_last_returned_time;
  double frame_last_filter_delay;
  int video_stream;           // 视频流索引
  AVStream *video_st;         // 视频流
  PacketQueue videoq;         // 视频packet队列
  double max_frame_duration;  // 帧的最大持续时间——超过这个时间，我们认为跳跃是时间戳不连续
  struct SwsContext *sub_convert_ctx;
  int eof;

  char *filename;
  int width, height, xleft, ytop;
  int step;  // 逐帧播放

  int vfilter_idx;
  AVFilterContext *in_video_filter;   // the first filter in the video chain
  AVFilterContext *out_video_filter;  // the last filter in the video chain
  AVFilterContext *in_audio_filter;   // the first filter in the audio chain
  AVFilterContext *out_audio_filter;  // the last filter in the audio chain
  AVFilterGraph *agraph;              // audio filter graph

  int last_video_stream, last_audio_stream, last_subtitle_stream;

  SDL_cond *continue_read_thread;
} VideoState;

/* options specified by the user */
static const AVInputFormat *file_iformat;                      // [cmd-arg] 输入格式
static const char *input_filename;                             // [cmd-arg] 输入文件名
static const char *window_title;                               // [cmd-arg] 标题
static int default_width = 640;                                // 默认宽度
static int default_height = 480;                               // 默认高度
static int screen_width = 0;                                   // [cmd-arg] 屏幕宽度
static int screen_height = 0;                                  // [cmd-arg] 屏幕高度
static int screen_left = SDL_WINDOWPOS_CENTERED;               // [cmd-arg] 窗口位置
static int screen_top = SDL_WINDOWPOS_CENTERED;                // [cmd-arg] 窗口位置
static int audio_disable;                                      // [cmd-arg] 禁用音频
static int video_disable;                                      // [cmd-arg] 禁用视频
static int subtitle_disable;                                   // [cmd-arg] 禁用字幕
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};  // [cmd-arg] 
static int seek_by_bytes = -1;                                 // [cmd-arg] 按字节查找 0=off 1=on -1=auto
static float seek_interval = 10;                               // [cmd-arg] 设置左、右键的查找间隔，以秒为单位
static int display_disable;                                    // [cmd-arg] 禁止显示
static int borderless;                                         // [cmd-arg] 设置播放界面无边框
static int alwaysontop;                                        // [cmd-arg] 窗口置顶
static int startup_volume = 100;                               // [cmd-arg] 起始音量
static int show_status = -1;                                   // [cmd-arg] 终端状态显示
static int av_sync_type = AV_SYNC_AUDIO_MASTER;                // [cmd-arg] 音视频同步类型
static int64_t start_time = AV_NOPTS_VALUE;                    // [cmd-arg] 开始时间
static int64_t duration = AV_NOPTS_VALUE;                      // [cmd-arg] 播放时长
static int fast = 0;                                           // [cmd-arg] 不符合规范的优化
static int genpts = 0;                                         // [cmd-arg]
static int lowres = 0;                                         // [cmd-arg]
static int decoder_reorder_pts = -1;                           // [cmd-arg] 让解码器重新排序PTS 0=off 1=on -1=auto
static int autoexit;                                           // [cmd-arg] 最后退出
static int exit_on_keydown;                                    // [cmd-arg] 键盘按键退出
static int exit_on_mousedown;                                  // [cmd-arg] 鼠标按键退出
static int loop = 1;                                           // [cmd-arg] 循环次数

// [cmd-arg] 用于设置当视频帧失去同步时，是否丢弃视频帧。"-framedrop"选项以bool方式改变变量framedrop值
// 1) 当命令行不带"-framedrop"选项或"-noframedrop"时，framedrop值为默认值-1，若同步方式是"同步到视频"
//    则不丢弃失去同步的视频帧，否则将丢弃失去同步的视频帧。
// 2) 当命令行带"-framedrop"选项时，framedrop值为1，无论何种同步方式，均丢弃失去同步的视频帧。
// 3) 当命令行带"-noframedrop"选项时，framedrop值为0，无论何种同步方式，均不丢弃失去同步的视频帧。
static int framedrop = -1;
static int infinite_buffer = -1;                  // [cmd-arg] 缓冲区大小限制，=1 表示不限制
static enum ShowMode show_mode = SHOW_MODE_NONE;  // [cmd-arg] 显示类型
static const char *audio_codec_name;              // [cmd-arg] 音频解码器名
static const char *subtitle_codec_name;           // [cmd-arg] 字幕解码器名
static const char *video_codec_name;              // [cmd-arg] 视频解码器名
double rdftspeed = 0.02;                          // [cmd-arg] 自适应滤波器的速度
static int64_t cursor_last_shown;                 // 上一次显示光标
static int cursor_hidden = 0;                     // 隐藏光标
static const char **vfilters_list = NULL;         // [cmd-arg] 视频 filters
static int nb_vfilters = 0;
static char *afilters = NULL;     // [cmd-arg] 音频 filters
static int autorotate = 1;        // [cmd-arg] 自动旋转
static int find_stream_info = 1;  // [cmd-arg] 读取并解码流，用启发式填充缺失的信息。默认为1
static int filter_nbthreads = 0;  // [cmd-arg] 每个 graph 的过滤器线程数
static int is_full_screen;        // [cmd-arg] 全屏
static int dummy;                 // [cmd-arg] 读取指定文件
static int64_t audio_callback_time;

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

static const struct TextureFormatEntry {
  enum AVPixelFormat format;
  int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

static inline int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                                 enum AVSampleFormat fmt2, int64_t channel_count2)
{
  /* If channel count == 1, planar and non-planar formats are the same */
  if (channel_count1 == 1 && channel_count2 == 1)
    return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
  else
    return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
  MyAVPacketList pkt1;
  int ret;

  if (q->abort_request)
    return -1;

  pkt1.pkt = pkt;
  pkt1.serial = q->serial;

  ret = av_fifo_write(q->pkt_list, &pkt1, 1);
  if (ret < 0)
    return ret;
  q->nb_packets++;
  q->size += pkt1.pkt->size + sizeof(pkt1);
  q->duration += pkt1.pkt->duration;
  /* XXX: should duplicate packet data in DV case */
  SDL_CondSignal(q->cond);
  return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
  AVPacket *pkt1;
  int ret;

  pkt1 = av_packet_alloc();
  if (!pkt1) {
    av_packet_unref(pkt);
    return -1;
  }
  av_packet_move_ref(pkt1, pkt);

  SDL_LockMutex(q->mutex);
  ret = packet_queue_put_private(q, pkt1);
  SDL_UnlockMutex(q->mutex);

  if (ret < 0)
    av_packet_free(&pkt1);

  return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
  pkt->stream_index = stream_index;
  return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
  memset(q, 0, sizeof(PacketQueue));
  q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
  if (!q->pkt_list)
    return AVERROR(ENOMEM);
  q->mutex = SDL_CreateMutex();
  if (!q->mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->cond = SDL_CreateCond();
  if (!q->cond) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  q->abort_request = 1;
  return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
  MyAVPacketList pkt1;

  SDL_LockMutex(q->mutex);
  while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
    av_packet_free(&pkt1.pkt);
  q->nb_packets = 0;
  q->size = 0;
  q->duration = 0;
  q->serial++;
  SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
  packet_queue_flush(q);
  av_fifo_freep2(&q->pkt_list);
  SDL_DestroyMutex(q->mutex);
  SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
  SDL_LockMutex(q->mutex);

  q->abort_request = 1;

  SDL_CondSignal(q->cond);

  SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
  SDL_LockMutex(q->mutex);
  q->abort_request = 0;
  q->serial++;
  SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
  MyAVPacketList pkt1;
  int ret;

  SDL_LockMutex(q->mutex);

  for (;;) {
    if (q->abort_request) {
      ret = -1;
      break;
    }

    if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
      q->nb_packets--;
      q->size -= pkt1.pkt->size + sizeof(pkt1);
      q->duration -= pkt1.pkt->duration;
      av_packet_move_ref(pkt, pkt1.pkt);
      if (serial)
        *serial = pkt1.serial;
      av_packet_free(&pkt1.pkt);
      ret = 1;
      break;
    } else if (!block) {
      ret = 0;
      break;
    } else {
      SDL_CondWait(q->cond, q->mutex);
    }
  }
  SDL_UnlockMutex(q->mutex);
  return ret;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond)
{
  memset(d, 0, sizeof(Decoder));
  d->pkt = av_packet_alloc();
  if (!d->pkt)
    return AVERROR(ENOMEM);
  d->avctx = avctx;
  d->queue = queue;
  d->empty_queue_cond = empty_queue_cond;
  d->start_pts = AV_NOPTS_VALUE;
  d->pkt_serial = -1;
  return 0;
}

/**
 * @brief 解码函数 解码音频、视频、字幕数据
 * @param [in] d 解码管理器
 * @param [out] frame 音视频解码数据
 * @param [out] sub 字幕解码数据
 * @return 1 表成功; 0 表文件结束; -1 表失败
 */
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
  int ret = AVERROR(EAGAIN);

  for (;;) {
    /* receive 解码数据帧 */
    if (d->queue->serial == d->pkt_serial) {
      do {
        if (d->queue->abort_request)
          return -1;

        switch (d->avctx->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
          ret = avcodec_receive_frame(d->avctx, frame);
          if (ret >= 0) {
            if (decoder_reorder_pts == -1) {
              frame->pts = frame->best_effort_timestamp;
            } else if (!decoder_reorder_pts) {
              frame->pts = frame->pkt_dts;
            }
          }
          break;
        case AVMEDIA_TYPE_AUDIO:
          ret = avcodec_receive_frame(d->avctx, frame);
          if (ret >= 0) {
            AVRational tb = (AVRational){1, frame->sample_rate};
            if (frame->pts != AV_NOPTS_VALUE)
              frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
            else if (d->next_pts != AV_NOPTS_VALUE)
              frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
            if (frame->pts != AV_NOPTS_VALUE) {
              d->next_pts = frame->pts + frame->nb_samples;
              d->next_pts_tb = tb;
            }
          }
          break;
        }
        if (ret == AVERROR_EOF) {
          d->finished = d->pkt_serial;
          avcodec_flush_buffers(d->avctx);
          return 0;
        }
        if (ret >= 0)
          return 1;
      } while (ret != AVERROR(EAGAIN));
    }

    /* 从 PacketQueue 中读取带解码数据包 */
    do {
      if (d->queue->nb_packets == 0)  // 如果队列为空，唤醒 read_thread 线程解封装
        SDL_CondSignal(d->empty_queue_cond);

      if (d->packet_pending) {
        d->packet_pending = 0;
      } else {
        int old_serial = d->pkt_serial;
        if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)  // 读取数据
          return -1;
        if (old_serial != d->pkt_serial) {  // 如果序列发生变化，则清空解码器缓存
          avcodec_flush_buffers(d->avctx);
          d->finished = 0;
          d->next_pts = d->start_pts;
          d->next_pts_tb = d->start_pts_tb;
        }
      }

      /* 序列号判定。如果读取的包序列号一致，则退出循环去send。如果序列号不一致，则 unref 数据 */
      if (d->queue->serial == d->pkt_serial)
        break;
      av_packet_unref(d->pkt);
    } while (1);

    /* send 待解码数据包 */
    if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      int got_frame = 0;
      ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
      if (ret < 0) {
        ret = AVERROR(EAGAIN);
      } else {
        if (got_frame && !d->pkt->data)
          d->packet_pending = 1;
        ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
      }
      av_packet_unref(d->pkt);
    } else {
      if (d->pkt->buf && !d->pkt->opaque_ref) {
        FrameData *fd;

        d->pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
        if (!d->pkt->opaque_ref)
          return AVERROR(ENOMEM);
        fd = (FrameData *)d->pkt->opaque_ref->data;
        fd->pkt_pos = d->pkt->pos;
      }

      /* 发送 AVPacket。如果发送失败，把 pkt 缓存起来，下次继续发送。 */
      if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
        av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
        d->packet_pending = 1;
      } else {
        av_packet_unref(d->pkt);
      }
    }
  }
}

static void decoder_destroy(Decoder *d)
{
  av_packet_free(&d->pkt);
  avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
  av_frame_unref(vp->frame);
  avsubtitle_free(&vp->sub);
}

/* FrameQueue 初始化 */
static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
  int i;
  memset(f, 0, sizeof(FrameQueue));
  if (!(f->mutex = SDL_CreateMutex())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  if (!(f->cond = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  f->pktq = pktq;
  f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
  f->keep_last = !!keep_last;
  for (i = 0; i < f->max_size; i++)
    if (!(f->queue[i].frame = av_frame_alloc()))
      return AVERROR(ENOMEM);
  return 0;
}

/* FrameQueue 销毁 */
static void frame_queue_destroy(FrameQueue *f)
{
  int i;
  for (i = 0; i < f->max_size; i++) {
    Frame *vp = &f->queue[i];
    frame_queue_unref_item(vp);  // 释放对vp->frame中的数据缓冲区的引用，注意不是释放frame对象本身
    av_frame_free(&vp->frame);   // 释放vp->frame对象
  }
  SDL_DestroyMutex(f->mutex);
  SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
  SDL_LockMutex(f->mutex);
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

/* 获取当前节点指针，只读不删 */
static Frame *frame_queue_peek(FrameQueue *f)
{
  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/* 获取下一节点指针，只读不删 */
static Frame *frame_queue_peek_next(FrameQueue *f)
{
  return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

/* 获取上一节点指针，只读不删 */
static Frame *frame_queue_peek_last(FrameQueue *f)
{
  return &f->queue[f->rindex];
}

/* 获取写指针：向队尾申请写帧，若队列已满，则等待；若处于中断，则返回 NULL */
static Frame *frame_queue_peek_writable(FrameQueue *f)
{
  /* wait until we have space to put a new frame */
  SDL_LockMutex(f->mutex);
  while (f->size >= f->max_size && !f->pktq->abort_request) {  // 如果队列已满，且不处于中断状态，则等待
    SDL_CondWait(f->cond, f->mutex);
  }
  SDL_UnlockMutex(f->mutex);

  if (f->pktq->abort_request)  // 中断状态直接返回空指针
    return NULL;

  return &f->queue[f->windex];
}

/* 获取读指针：若队列为空，则等待；若处于中断，则返回 NULL */
static Frame *frame_queue_peek_readable(FrameQueue *f)
{
  /* wait until we have a readable a new frame */
  SDL_LockMutex(f->mutex);
  while (f->size - f->rindex_shown <= 0 &&
         !f->pktq->abort_request) {
    SDL_CondWait(f->cond, f->mutex);
  }
  SDL_UnlockMutex(f->mutex);

  if (f->pktq->abort_request)
    return NULL;

  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/* 更新写指针: 向队尾压入一帧，只更新计数和写指针，帧数据应该在此之前写入 */
static void frame_queue_push(FrameQueue *f)
{
  if (++f->windex == f->max_size)
    f->windex = 0;
  SDL_LockMutex(f->mutex);
  f->size++;
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

/* 更新读指针，同时删除旧节点 */
static void frame_queue_next(FrameQueue *f)
{
  if (f->keep_last && !f->rindex_shown) {
    f->rindex_shown = 1;
    return;
  }
  frame_queue_unref_item(&f->queue[f->rindex]);
  if (++f->rindex == f->max_size)
    f->rindex = 0;
  SDL_LockMutex(f->mutex);
  f->size--;
  SDL_CondSignal(f->cond);
  SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
  return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
  Frame *fp = &f->queue[f->rindex];
  if (f->rindex_shown && fp->serial == f->pktq->serial)
    return fp->pos;
  else
    return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
  packet_queue_abort(d->queue);
  frame_queue_signal(fq);
  SDL_WaitThread(d->decoder_tid, NULL);
  d->decoder_tid = NULL;
  packet_queue_flush(d->queue);
}

static inline void fill_rectangle(int x, int y, int w, int h)
{
  SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = w;
  rect.h = h;
  if (w && h)
    SDL_RenderFillRect(renderer, &rect);
}

/*!
 * @brief 纹理分配；当 texture 为空时，分配；纹理尺寸或像素格式发生变化时，重新分配；
 * @param [out] texture 待分配纹理，若已存在，则先销毁
 * @param [in] new_format SDL像素格式
 * @param [in] new_width 纹理宽
 * @param [in] new_height 纹理高
 * @param [in] blendmode 融合类型
 * @param [in] init_texture >0 表示需要将内存初始化为0
 * @return 成功返回 0；失败返回 -1
 */
static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
  Uint32 format;
  int access, w, h;
  if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
    void *pixels;
    int pitch;
    if (*texture)
      SDL_DestroyTexture(*texture);
    if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
      return -1;
    if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
      return -1;
    if (init_texture) {
      if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
        return -1;
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(*texture);
    }
    av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
  }
  return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
  AVRational aspect_ratio = pic_sar;
  int64_t width, height, x, y;

  if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
    aspect_ratio = av_make_q(1, 1);

  aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

  /* XXX: we suppose the screen has a 1.0 pixel ratio */
  height = scr_height;
  width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
  if (width > scr_width) {
    width = scr_width;
    height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
  }
  x = (scr_width - width) / 2;
  y = (scr_height - height) / 2;
  rect->x = scr_xleft + x;
  rect->y = scr_ytop + y;
  rect->w = FFMAX((int)width, 1);
  rect->h = FFMAX((int)height, 1);
}

/*!
 * @brief 根据 FFmpeg 的像素格式得到 SDL 对应的像素格式
 * @param [in] format FFmpeg的像素格式
 * @param [out] sdl_pix_fmt SDL的像素格式
 * @param [out] sdl_blendmode
 */
static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
  int i;
  *sdl_blendmode = SDL_BLENDMODE_NONE;
  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  if (format == AV_PIX_FMT_RGB32 ||
      format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32 ||
      format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;
  for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
    }
  }
}

static int upload_texture(SDL_Texture **tex, AVFrame *frame)
{
  int ret = 0;
  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;
  get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
  if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
    return -1;
  switch (sdl_pix_fmt) {
  case SDL_PIXELFORMAT_IYUV:  // frame格式对应 SDL_PIXELFORMAT_IYUV, 不用进行图像格式转换，调用SDL_UpdateYUVTexture()更新SDL texture
    if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
      ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                 frame->data[1], frame->linesize[1],
                                 frame->data[2], frame->linesize[2]);
    } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
      ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                                 frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                 frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
    } else {
      av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
      return -1;
    }
    break;
  default:  // frame格式对应其他SDL像素格式，不用进行图像格式转换，调用SDL_UpdateTexture()更新SDL texture
    if (frame->linesize[0] < 0) {
      ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
    } else {
      ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
    }
    break;
  }
  return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2, 0, 8)
  SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
  if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
    if (frame->color_range == AVCOL_RANGE_JPEG)
      mode = SDL_YUV_CONVERSION_JPEG;
    else if (frame->colorspace == AVCOL_SPC_BT709)
      mode = SDL_YUV_CONVERSION_BT709;
    else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
      mode = SDL_YUV_CONVERSION_BT601;
  }
  SDL_SetYUVConversionMode(mode); /* FIXME: no support for linear transfer */
#endif
}

static void video_image_display(VideoState *vs)
{
  Frame *vp;
  Frame *sp = NULL;
  SDL_Rect rect;

  vp = frame_queue_peek_last(&vs->pictq);
  if (vs->subtitle_st) {
    if (frame_queue_nb_remaining(&vs->subpq) > 0) {
      sp = frame_queue_peek(&vs->subpq);

      if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
        if (!sp->uploaded) {
          uint8_t *pixels[4];
          int pitch[4];
          int i;
          if (!sp->width || !sp->height) {
            sp->width = vp->width;
            sp->height = vp->height;
          }
          if (realloc_texture(&vs->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
            return;

          for (i = 0; i < sp->sub.num_rects; i++) {
            AVSubtitleRect *sub_rect = sp->sub.rects[i];

            sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
            sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
            sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
            sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

            vs->sub_convert_ctx = sws_getCachedContext(vs->sub_convert_ctx,
                                                       sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                       sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                       0, NULL, NULL, NULL);
            if (!vs->sub_convert_ctx) {
              av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
              return;
            }
            if (!SDL_LockTexture(vs->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
              sws_scale(vs->sub_convert_ctx, (const uint8_t *const *)sub_rect->data, sub_rect->linesize,
                        0, sub_rect->h, pixels, pitch);
              SDL_UnlockTexture(vs->sub_texture);
            }
          }
          sp->uploaded = 1;
        }
      } else
        sp = NULL;
    }
  }

  calculate_display_rect(&rect, vs->xleft, vs->ytop, vs->width, vs->height, vp->width, vp->height, vp->sar);
  set_sdl_yuv_conversion_mode(vp->frame);

  if (!vp->uploaded) {
    if (upload_texture(&vs->vid_texture, vp->frame) < 0) {
      set_sdl_yuv_conversion_mode(NULL);
      return;
    }
    vp->uploaded = 1;
    vp->flip_v = vp->frame->linesize[0] < 0;
  }

  SDL_RenderCopyEx(renderer, vs->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);
  set_sdl_yuv_conversion_mode(NULL);
  if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
    SDL_RenderCopy(renderer, vs->sub_texture, NULL, &rect);
#else
    int i;
    double xratio = (double)rect.w / (double)sp->width;
    double yratio = (double)rect.h / (double)sp->height;
    for (i = 0; i < sp->sub.num_rects; i++) {
      SDL_Rect *sub_rect = (SDL_Rect *)sp->sub.rects[i];
      SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                         .y = rect.y + sub_rect->y * yratio,
                         .w = sub_rect->w * xratio,
                         .h = sub_rect->h * yratio};
      SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
    }
#endif
  }
}

static inline int compute_mod(int a, int b)
{
  return a < 0 ? a % b + b : a % b;
}

static void video_audio_display(VideoState *vs)
{
  int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
  int ch, channels, h, h2;
  int64_t time_diff;
  int rdft_bits, nb_freq;

  for (rdft_bits = 1; (1 << rdft_bits) < 2 * vs->height; rdft_bits++)
    ;
  nb_freq = 1 << (rdft_bits - 1);

  /* compute display index : center on currently output samples */
  channels = vs->audio_tgt.ch_layout.nb_channels;
  nb_display_channels = channels;
  if (!vs->paused) {
    int data_used = vs->show_mode == SHOW_MODE_WAVES ? vs->width : (2 * nb_freq);
    n = 2 * channels;
    delay = vs->audio_write_buf_size;
    delay /= n;

    /* to be more precise, we take into account the time spent since
       the last buffer computation */
    if (audio_callback_time) {
      time_diff = av_gettime_relative() - audio_callback_time;
      delay -= (time_diff * vs->audio_tgt.freq) / 1000000;
    }

    delay += 2 * data_used;
    if (delay < data_used)
      delay = data_used;

    i_start = x = compute_mod(vs->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
    if (vs->show_mode == SHOW_MODE_WAVES) {
      h = INT_MIN;
      for (i = 0; i < 1000; i += channels) {
        int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
        int a = vs->sample_array[idx];
        int b = vs->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
        int c = vs->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
        int d = vs->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
        int score = a - d;
        if (h < score && (b ^ c) < 0) {
          h = score;
          i_start = idx;
        }
      }
    }

    vs->last_i_start = i_start;
  } else {
    i_start = vs->last_i_start;
  }

  if (vs->show_mode == SHOW_MODE_WAVES) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

    /* total height for one channel */
    h = vs->height / nb_display_channels;
    /* graph height / 2 */
    h2 = (h * 9) / 20;
    for (ch = 0; ch < nb_display_channels; ch++) {
      i = i_start + ch;
      y1 = vs->ytop + ch * h + (h / 2); /* position of center line */
      for (x = 0; x < vs->width; x++) {
        y = (vs->sample_array[i] * h2) >> 15;
        if (y < 0) {
          y = -y;
          ys = y1 - y;
        } else {
          ys = y1;
        }
        fill_rectangle(vs->xleft + x, ys, 1, y);
        i += channels;
        if (i >= SAMPLE_ARRAY_SIZE)
          i -= SAMPLE_ARRAY_SIZE;
      }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

    for (ch = 1; ch < nb_display_channels; ch++) {
      y = vs->ytop + ch * h;
      fill_rectangle(vs->xleft, y, vs->width, 1);
    }
  } else {
    int err = 0;
    if (realloc_texture(&vs->vis_texture, SDL_PIXELFORMAT_ARGB8888, vs->width, vs->height, SDL_BLENDMODE_NONE, 1) < 0)
      return;

    if (vs->xpos >= vs->width)
      vs->xpos = 0;
    nb_display_channels = FFMIN(nb_display_channels, 2);
    if (rdft_bits != vs->rdft_bits) {
      const float rdft_scale = 1.0;
      av_tx_uninit(&vs->rdft);
      av_freep(&vs->real_data);
      av_freep(&vs->rdft_data);
      vs->rdft_bits = rdft_bits;
      vs->real_data = av_malloc_array(nb_freq, 4 * sizeof(*vs->real_data));
      vs->rdft_data = av_malloc_array(nb_freq + 1, 2 * sizeof(*vs->rdft_data));
      err = av_tx_init(&vs->rdft, &vs->rdft_fn, AV_TX_FLOAT_RDFT,
                       0, 1 << rdft_bits, &rdft_scale, 0);
    }
    if (err < 0 || !vs->rdft_data) {
      av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
      vs->show_mode = SHOW_MODE_WAVES;
    } else {
      float *data_in[2];
      AVComplexFloat *data[2];
      SDL_Rect rect = {.x = vs->xpos, .y = 0, .w = 1, .h = vs->height};
      uint32_t *pixels;
      int pitch;
      for (ch = 0; ch < nb_display_channels; ch++) {
        data_in[ch] = vs->real_data + 2 * nb_freq * ch;
        data[ch] = vs->rdft_data + nb_freq * ch;
        i = i_start + ch;
        for (x = 0; x < 2 * nb_freq; x++) {
          double w = (x - nb_freq) * (1.0 / nb_freq);
          data_in[ch][x] = vs->sample_array[i] * (1.0 - w * w);
          i += channels;
          if (i >= SAMPLE_ARRAY_SIZE)
            i -= SAMPLE_ARRAY_SIZE;
        }
        vs->rdft_fn(vs->rdft, data[ch], data_in[ch], sizeof(float));
        data[ch][0].im = data[ch][nb_freq].re;
        data[ch][nb_freq].re = 0;
      }
      /* Least efficient way to do this, we should of course
       * directly access it but it is more than fast enough. */
      if (!SDL_LockTexture(vs->vis_texture, &rect, (void **)&pixels, &pitch)) {
        pitch >>= 2;
        pixels += pitch * vs->height;
        for (y = 0; y < vs->height; y++) {
          double w = 1 / sqrt(nb_freq);
          int a = sqrt(w * sqrt(data[0][y].re * data[0][y].re + data[0][y].im * data[0][y].im));
          int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][y].re, data[1][y].im))
                                             : a;
          a = FFMIN(a, 255);
          b = FFMIN(b, 255);
          pixels -= pitch;
          *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
        }
        SDL_UnlockTexture(vs->vis_texture);
      }
      SDL_RenderCopy(renderer, vs->vis_texture, NULL, NULL);
    }
    if (!vs->paused)
      vs->xpos++;
  }
}

static void stream_component_close(VideoState *vs, int stream_index)
{
  AVFormatContext *ic = vs->ifmt_ctx;
  AVCodecParameters *codecpar;

  if (stream_index < 0 || stream_index >= ic->nb_streams)
    return;
  codecpar = ic->streams[stream_index]->codecpar;

  switch (codecpar->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    decoder_abort(&vs->auddec, &vs->sampq);
    SDL_CloseAudioDevice(audio_dev);
    decoder_destroy(&vs->auddec);
    swr_free(&vs->swr_ctx);
    av_freep(&vs->audio_buf1);
    vs->audio_buf1_size = 0;
    vs->audio_buf = NULL;

    if (vs->rdft) {
      av_tx_uninit(&vs->rdft);
      av_freep(&vs->real_data);
      av_freep(&vs->rdft_data);
      vs->rdft = NULL;
      vs->rdft_bits = 0;
    }
    break;
  case AVMEDIA_TYPE_VIDEO:
    decoder_abort(&vs->viddec, &vs->pictq);
    decoder_destroy(&vs->viddec);
    break;
  case AVMEDIA_TYPE_SUBTITLE:
    decoder_abort(&vs->subdec, &vs->subpq);
    decoder_destroy(&vs->subdec);
    break;
  default:
    break;
  }

  ic->streams[stream_index]->discard = AVDISCARD_ALL;
  switch (codecpar->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    vs->audio_st = NULL;
    vs->audio_stream = -1;
    break;
  case AVMEDIA_TYPE_VIDEO:
    vs->video_st = NULL;
    vs->video_stream = -1;
    break;
  case AVMEDIA_TYPE_SUBTITLE:
    vs->subtitle_st = NULL;
    vs->subtitle_stream = -1;
    break;
  default:
    break;
  }
}

static void stream_close(VideoState *vs)
{
  /* XXX: use a special url_shutdown call to abort parse cleanly */
  vs->abort_request = 1;
  SDL_WaitThread(vs->read_tid, NULL);

  /* close each stream */
  if (vs->audio_stream >= 0)
    stream_component_close(vs, vs->audio_stream);
  if (vs->video_stream >= 0)
    stream_component_close(vs, vs->video_stream);
  if (vs->subtitle_stream >= 0)
    stream_component_close(vs, vs->subtitle_stream);

  avformat_close_input(&vs->ifmt_ctx);

  packet_queue_destroy(&vs->videoq);
  packet_queue_destroy(&vs->audioq);
  packet_queue_destroy(&vs->subtitleq);

  /* free all pictures */
  frame_queue_destroy(&vs->pictq);
  frame_queue_destroy(&vs->sampq);
  frame_queue_destroy(&vs->subpq);
  SDL_DestroyCond(vs->continue_read_thread);
  sws_freeContext(vs->sub_convert_ctx);
  av_free(vs->filename);
  if (vs->vis_texture)
    SDL_DestroyTexture(vs->vis_texture);
  if (vs->vid_texture)
    SDL_DestroyTexture(vs->vid_texture);
  if (vs->sub_texture)
    SDL_DestroyTexture(vs->sub_texture);
  av_free(vs);
}

/* 关闭，退出 */
static void do_exit(VideoState *vs)
{
  if (vs) {
    stream_close(vs);
  }
  if (renderer)
    SDL_DestroyRenderer(renderer);
  if (window)
    SDL_DestroyWindow(window);
  uninit_opts();
  av_freep(&vfilters_list);
  avformat_network_deinit();
  if (show_status)
    printf("\n");
  SDL_Quit();
  av_log(NULL, AV_LOG_QUIET, "%s", "");
  exit(0);
}

static void sigterm_handler(int sig)
{
  exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX)
    max_height = height;
  calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
  default_width = rect.w;
  default_height = rect.h;
}

static int video_open(VideoState *vs)
{
  int w, h;

  w = screen_width ? screen_width : default_width;
  h = screen_height ? screen_height : default_height;

  if (!window_title)
    window_title = input_filename;
  SDL_SetWindowTitle(window, window_title);

  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);

  vs->width = w;
  vs->height = h;

  return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *vs)
{
  if (!vs->width)
    video_open(vs);

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);  // 使用特定颜色清空当前渲染目标
  if (vs->audio_st && vs->show_mode != SHOW_MODE_VIDEO)
    video_audio_display(vs);
  else if (vs->video_st)
    video_image_display(vs);    // 更新当前渲染目标
  SDL_RenderPresent(renderer);  // 执行渲染，更新屏幕显示
}

static double get_clock(Clock *c)
{
  if (*c->queue_serial != c->serial)
    return NAN;
  if (c->paused) {
    return c->pts;
  } else {
    double time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
  }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
  c->pts = pts;
  c->last_updated = time;
  c->pts_drift = c->pts - time;
  c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
  double time = av_gettime_relative() / 1000000.0;
  set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
  set_clock(c, get_clock(c), c->serial);
  c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
  c->speed = 1.0;
  c->paused = 0;
  c->queue_serial = queue_serial;
  set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
  double clock = get_clock(c);
  double slave_clock = get_clock(slave);
  if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
    set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *vs)
{
  if (vs->av_sync_type == AV_SYNC_VIDEO_MASTER) {
    if (vs->video_st)
      return AV_SYNC_VIDEO_MASTER;
    else
      return AV_SYNC_AUDIO_MASTER;
  } else if (vs->av_sync_type == AV_SYNC_AUDIO_MASTER) {
    if (vs->audio_st)
      return AV_SYNC_AUDIO_MASTER;
    else
      return AV_SYNC_EXTERNAL_CLOCK;
  } else {
    return AV_SYNC_EXTERNAL_CLOCK;
  }
}

/* get the current master clock value */
static double get_master_clock(VideoState *vs)
{
  double val;

  switch (get_master_sync_type(vs)) {
  case AV_SYNC_VIDEO_MASTER:
    val = get_clock(&vs->vidclk);
    break;
  case AV_SYNC_AUDIO_MASTER:
    val = get_clock(&vs->audclk);
    break;
  default:
    val = get_clock(&vs->extclk);
    break;
  }
  return val;
}

static void check_external_clock_speed(VideoState *vs)
{
  if (vs->video_stream >= 0 && vs->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
      vs->audio_stream >= 0 && vs->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
    set_clock_speed(&vs->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, vs->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
  } else if ((vs->video_stream < 0 || vs->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
             (vs->audio_stream < 0 || vs->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
    set_clock_speed(&vs->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, vs->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
  } else {
    double speed = vs->extclk.speed;
    if (speed != 1.0)
      set_clock_speed(&vs->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
  }
}

/* seek in the stream */
static void stream_seek(VideoState *vs, int64_t pos, int64_t rel, int by_bytes)
{
  if (!vs->seek_req) {
    vs->seek_pos = pos;
    vs->seek_rel = rel;
    vs->seek_flags &= ~AVSEEK_FLAG_BYTE;
    if (by_bytes)
      vs->seek_flags |= AVSEEK_FLAG_BYTE;
    vs->seek_req = 1;
    SDL_CondSignal(vs->continue_read_thread);
  }
}

/* 暂停或恢复播放，每调用一次，就实现一次状态翻转 */
static void stream_toggle_pause(VideoState *vs)
{
  if (vs->paused) {
    vs->frame_timer += av_gettime_relative() / 1000000.0 - vs->vidclk.last_updated;
    if (vs->read_pause_return != AVERROR(ENOSYS)) {
      vs->vidclk.paused = 0;
    }
    set_clock(&vs->vidclk, get_clock(&vs->vidclk), vs->vidclk.serial);
  }
  set_clock(&vs->extclk, get_clock(&vs->extclk), vs->extclk.serial);
  vs->paused = vs->audclk.paused = vs->vidclk.paused = vs->extclk.paused = !vs->paused;
}

static void toggle_pause(VideoState *vs)
{
  stream_toggle_pause(vs);
  vs->step = 0;
}

static void toggle_mute(VideoState *vs)
{
  vs->muted = !vs->muted;
}

static void update_volume(VideoState *vs, int sign, double step)
{
  double volume_level = vs->audio_volume ? (20 * log(vs->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
  int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
  vs->audio_volume = av_clip(vs->audio_volume == new_volume ? (vs->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *vs)
{
  /* if the stream is paused unpause it, then step */
  if (vs->paused)
    stream_toggle_pause(vs);
  vs->step = 1;
}

static double compute_target_delay(double delay, VideoState *vs)
{
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (get_master_sync_type(vs) != AV_SYNC_VIDEO_MASTER) {
    /* if video is slave, we try to correct big delays by
       duplicating or deleting a frame */
    diff = get_clock(&vs->vidclk) - get_master_clock(vs);

    /* skip or repeat frame. We take into account the
       delay to compute the threshold. I still don't know
       if it is the best guess */
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!isnan(diff) && fabs(diff) < vs->max_frame_duration) {
      if (diff <= -sync_threshold)
        delay = FFMAX(0, delay + diff);
      else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
        delay = delay + diff;
      else if (diff >= sync_threshold)
        delay = 2 * delay;
    }
  }

  av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
         delay, -diff);

  return delay;
}

static double vp_duration(VideoState *vs, Frame *vp, Frame *nextvp)
{
  if (vp->serial == nextvp->serial) {
    double duration = nextvp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > vs->max_frame_duration)
      return vp->duration;
    else
      return duration;
  } else {
    return 0.0;
  }
}

static void update_video_pts(VideoState *vs, double pts, int serial)
{
  /* update current video pts */
  set_clock(&vs->vidclk, pts, serial);
  sync_clock_to_slave(&vs->extclk, &vs->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
  VideoState *vs = opaque;
  double time;

  Frame *sp, *sp2;

  if (!vs->paused && get_master_sync_type(vs) == AV_SYNC_EXTERNAL_CLOCK && vs->realtime)
    check_external_clock_speed(vs);

  if (!display_disable && vs->show_mode != SHOW_MODE_VIDEO && vs->audio_st) {
    time = av_gettime_relative() / 1000000.0;
    if (vs->force_refresh || vs->last_vis_time + rdftspeed < time) {
      video_display(vs);
      vs->last_vis_time = time;
    }
    *remaining_time = FFMIN(*remaining_time, vs->last_vis_time + rdftspeed - time);
  }

  if (vs->video_st) {
  retry:
    if (frame_queue_nb_remaining(&vs->pictq) == 0) {
      // nothing to do, no picture to display in the queue
    } else {
      double last_duration, duration, delay;
      Frame *vp, *lastvp;

      /* dequeue the picture */
      lastvp = frame_queue_peek_last(&vs->pictq);
      vp = frame_queue_peek(&vs->pictq);

      if (vp->serial != vs->videoq.serial) {
        frame_queue_next(&vs->pictq);
        goto retry;
      }

      if (lastvp->serial != vp->serial)
        vs->frame_timer = av_gettime_relative() / 1000000.0;

      if (vs->paused)
        goto display;

      /* compute nominal last_duration */
      last_duration = vp_duration(vs, lastvp, vp);
      delay = compute_target_delay(last_duration, vs);

      time = av_gettime_relative() / 1000000.0;
      if (time < vs->frame_timer + delay) {
        *remaining_time = FFMIN(vs->frame_timer + delay - time, *remaining_time);
        goto display;
      }

      vs->frame_timer += delay;
      if (delay > 0 && time - vs->frame_timer > AV_SYNC_THRESHOLD_MAX)
        vs->frame_timer = time;

      SDL_LockMutex(vs->pictq.mutex);
      if (!isnan(vp->pts))
        update_video_pts(vs, vp->pts, vp->serial);
      SDL_UnlockMutex(vs->pictq.mutex);

      if (frame_queue_nb_remaining(&vs->pictq) > 1) {
        Frame *nextvp = frame_queue_peek_next(&vs->pictq);
        duration = vp_duration(vs, vp, nextvp);
        if (!vs->step && (framedrop > 0 || (framedrop && get_master_sync_type(vs) != AV_SYNC_VIDEO_MASTER)) && time > vs->frame_timer + duration) {
          vs->frame_drops_late++;
          frame_queue_next(&vs->pictq);
          goto retry;
        }
      }

      if (vs->subtitle_st) {
        while (frame_queue_nb_remaining(&vs->subpq) > 0) {
          sp = frame_queue_peek(&vs->subpq);

          if (frame_queue_nb_remaining(&vs->subpq) > 1)
            sp2 = frame_queue_peek_next(&vs->subpq);
          else
            sp2 = NULL;

          if (sp->serial != vs->subtitleq.serial || (vs->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000))) || (sp2 && vs->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000)))) {
            if (sp->uploaded) {
              int i;
              for (i = 0; i < sp->sub.num_rects; i++) {
                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                uint8_t *pixels;
                int pitch, j;

                if (!SDL_LockTexture(vs->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                  for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                    memset(pixels, 0, sub_rect->w << 2);
                  SDL_UnlockTexture(vs->sub_texture);
                }
              }
            }
            frame_queue_next(&vs->subpq);
          } else {
            break;
          }
        }
      }

      frame_queue_next(&vs->pictq);
      vs->force_refresh = 1;

      if (vs->step && !vs->paused)
        stream_toggle_pause(vs);
    }
  display:
    /* display picture */
    if (!display_disable && vs->force_refresh && vs->show_mode == SHOW_MODE_VIDEO && vs->pictq.rindex_shown)
      video_display(vs);
  }
  vs->force_refresh = 0;
  if (show_status) {
    AVBPrint buf;
    static int64_t last_time;
    int64_t cur_time;
    int aqsize, vqsize, sqsize;
    double av_diff;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
      aqsize = 0;
      vqsize = 0;
      sqsize = 0;
      if (vs->audio_st)
        aqsize = vs->audioq.size;
      if (vs->video_st)
        vqsize = vs->videoq.size;
      if (vs->subtitle_st)
        sqsize = vs->subtitleq.size;
      av_diff = 0;
      if (vs->audio_st && vs->video_st)
        av_diff = get_clock(&vs->audclk) - get_clock(&vs->vidclk);
      else if (vs->video_st)
        av_diff = get_master_clock(vs) - get_clock(&vs->vidclk);
      else if (vs->audio_st)
        av_diff = get_master_clock(vs) - get_clock(&vs->audclk);

      av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
      av_bprintf(&buf,
                 "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%" PRId64 "/%" PRId64 "   \r",
                 get_master_clock(vs),
                 (vs->audio_st && vs->video_st) ? "A-V" : (vs->video_st ? "M-V" : (vs->audio_st ? "M-A" : "   ")),
                 av_diff,
                 vs->frame_drops_early + vs->frame_drops_late,
                 aqsize / 1024,
                 vqsize / 1024,
                 sqsize,
                 vs->video_st ? vs->viddec.avctx->pts_correction_num_faulty_dts : 0,
                 vs->video_st ? vs->viddec.avctx->pts_correction_num_faulty_pts : 0);

      if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
        fprintf(stderr, "%s", buf.str);
      else
        av_log(NULL, AV_LOG_INFO, "%s", buf.str);

      fflush(stderr);
      av_bprint_finalize(&buf, NULL);

      last_time = cur_time;
    }
  }
}

/* 写视频帧队列 / 视频帧入队操作 */
static int queue_picture(VideoState *vs, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
  Frame *vp;

#if defined(DEBUG_SYNC)
  printf("frame_type=%c pts=%0.3f\n",
         av_get_picture_type_char(src_frame->pict_type), pts);
#endif

  if (!(vp = frame_queue_peek_writable(&vs->pictq)))  // 向队尾申请一个写帧，若已满则等待
    return -1;

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = 0;

  vp->width = src_frame->width;
  vp->height = src_frame->height;
  vp->format = src_frame->format;

  vp->pts = pts;
  vp->duration = duration;
  vp->pos = pos;
  vp->serial = serial;

  set_default_window_size(vp->width, vp->height, vp->sar);

  av_frame_move_ref(vp->frame, src_frame);  // 帧数据移动
  frame_queue_push(&vs->pictq);             // 更新写指针，标志入队操作完成
  return 0;
}

/* 从packet队列中取一个packet解码得到一个frame，并判断是否要根据framedrop机制丢弃失去同步的视频帧 */
static int get_video_frame(VideoState *vs, AVFrame *frame)
{
  int got_picture;

  if ((got_picture = decoder_decode_frame(&vs->viddec, frame, NULL)) < 0)
    return -1;

  if (got_picture) {
    double dpts = NAN;

    if (frame->pts != AV_NOPTS_VALUE)
      dpts = av_q2d(vs->video_st->time_base) * frame->pts;

    frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(vs->ifmt_ctx, vs->video_st, frame);

    if (framedrop > 0 || (framedrop && get_master_sync_type(vs) != AV_SYNC_VIDEO_MASTER)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        double diff = dpts - get_master_clock(vs);
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
            diff - vs->frame_last_filter_delay < 0 &&
            vs->viddec.pkt_serial == vs->vidclk.serial &&
            vs->videoq.nb_packets) {
          vs->frame_drops_early++;
          av_frame_unref(frame);  // 视频帧失去同步则直接扔掉
          got_picture = 0;
        }
      }
    }
  }

  return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
  int ret, i;
  int nb_filters = graph->nb_filters;
  AVFilterInOut *outputs = NULL, *inputs = NULL;

  if (filtergraph) {
    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
      ret = AVERROR(ENOMEM);
      goto fail;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = source_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
      goto fail;
  } else {
    if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
      goto fail;
  }

  /* Reorder the filters to ensure that inputs of the custom filters are merged first */
  for (i = 0; i < graph->nb_filters - nb_filters; i++)
    FFSWAP(AVFilterContext *, graph->filters[i], graph->filters[i + nb_filters]);

  ret = avfilter_graph_config(graph, NULL);
fail:
  avfilter_inout_free(&outputs);
  avfilter_inout_free(&inputs);
  return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *vs, const char *vfilters, AVFrame *frame)
{
  enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
  char sws_flags_str[512] = "";
  char buffersrc_args[256];
  int ret;
  AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
  AVCodecParameters *codecpar = vs->video_st->codecpar;
  AVRational fr = av_guess_frame_rate(vs->ifmt_ctx, vs->video_st, NULL);
  const AVDictionaryEntry *e = NULL;
  int nb_pix_fmts = 0;
  int i, j;

  for (i = 0; i < renderer_info.num_texture_formats; i++) {
    for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
      if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
        pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
        break;
      }
    }
  }
  pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

  while ((e = av_dict_iterate(sws_dict, e))) {
    if (!strcmp(e->key, "sws_flags")) {
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
    } else
      av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
  }
  if (strlen(sws_flags_str))
    sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

  graph->scale_sws_opts = av_strdup(sws_flags_str);

  snprintf(buffersrc_args, sizeof(buffersrc_args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
           frame->width, frame->height, frame->format,
           vs->video_st->time_base.num, vs->video_st->time_base.den,
           codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
  if (fr.num && fr.den)
    av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

  if ((ret = avfilter_graph_create_filter(&filt_src,
                                          avfilter_get_by_name("buffer"),
                                          "ffplay_buffer", buffersrc_args, NULL,
                                          graph)) < 0)
    goto fail;

  ret = avfilter_graph_create_filter(&filt_out,
                                     avfilter_get_by_name("buffersink"),
                                     "ffplay_buffersink", NULL, NULL, graph);
  if (ret < 0)
    goto fail;

  if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto fail;

  last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg)                                            \
  do {                                                                    \
    AVFilterContext *filt_ctx;                                            \
                                                                          \
    ret = avfilter_graph_create_filter(&filt_ctx,                         \
                                       avfilter_get_by_name(name),        \
                                       "ffplay_" name, arg, NULL, graph); \
    if (ret < 0)                                                          \
      goto fail;                                                          \
                                                                          \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                     \
    if (ret < 0)                                                          \
      goto fail;                                                          \
                                                                          \
    last_filter = filt_ctx;                                               \
  } while (0)

  if (autorotate) {
    double theta = 0.0;
    int32_t *displaymatrix = NULL;
    AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (sd)
      displaymatrix = (int32_t *)sd->data;
    if (!displaymatrix) {
      const AVPacketSideData *sd = av_packet_side_data_get(vs->video_st->codecpar->coded_side_data,
                                                           vs->video_st->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
      if (sd)
        displaymatrix = (int32_t *)sd->data;
    }
    theta = get_rotation(displaymatrix);

    if (fabs(theta - 90) < 1.0) {
      INSERT_FILT("transpose", "clock");
    } else if (fabs(theta - 180) < 1.0) {
      INSERT_FILT("hflip", NULL);
      INSERT_FILT("vflip", NULL);
    } else if (fabs(theta - 270) < 1.0) {
      INSERT_FILT("transpose", "cclock");
    } else if (fabs(theta) > 1.0) {
      char rotate_buf[64];
      snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
      INSERT_FILT("rotate", rotate_buf);
    }
  }

  if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
    goto fail;

  vs->in_video_filter = filt_src;
  vs->out_video_filter = filt_out;

fail:
  return ret;
}

static int configure_audio_filters(VideoState *vs, const char *afilters, int force_output_format)
{
  static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
  int sample_rates[2] = {0, -1};
  AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
  char aresample_swr_opts[512] = "";
  const AVDictionaryEntry *e = NULL;
  AVBPrint bp;
  char asrc_args[256];
  int ret;

  avfilter_graph_free(&vs->agraph);
  if (!(vs->agraph = avfilter_graph_alloc()))
    return AVERROR(ENOMEM);
  vs->agraph->nb_threads = filter_nbthreads;

  av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

  while ((e = av_dict_iterate(swr_opts, e)))
    av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
  if (strlen(aresample_swr_opts))
    aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
  av_opt_set(vs->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

  av_channel_layout_describe_bprint(&vs->audio_filter_src.ch_layout, &bp);

  ret = snprintf(asrc_args, sizeof(asrc_args),
                 "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                 vs->audio_filter_src.freq, av_get_sample_fmt_name(vs->audio_filter_src.fmt),
                 1, vs->audio_filter_src.freq, bp.str);

  ret = avfilter_graph_create_filter(&filt_asrc,
                                     avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                     asrc_args, NULL, vs->agraph);
  if (ret < 0)
    goto end;

  ret = avfilter_graph_create_filter(&filt_asink,
                                     avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                     NULL, NULL, vs->agraph);
  if (ret < 0)
    goto end;

  if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto end;
  if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
    goto end;

  if (force_output_format) {
    sample_rates[0] = vs->audio_tgt.freq;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
    if ((ret = av_opt_set(filt_asink, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
    if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
      goto end;
  }

  if ((ret = configure_filtergraph(vs->agraph, afilters, filt_asrc, filt_asink)) < 0)
    goto end;

  vs->in_audio_filter = filt_asrc;
  vs->out_audio_filter = filt_asink;

end:
  if (ret < 0)
    avfilter_graph_free(&vs->agraph);
  av_bprint_finalize(&bp, NULL);

  return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg)
{
  packet_queue_start(d->queue);
  d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
  if (!d->decoder_tid) {
    av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
    return AVERROR(ENOMEM);
  }
  return 0;
}

/* 音频解码线程：从音频 packet_queue 中取数据，解码后放入音频 frame_queue */
static int audio_thread(void *arg)
{
  VideoState *vs = arg;
  AVFrame *frame = av_frame_alloc();
  Frame *af;
  int last_serial = -1;
  int reconfigure;
  int got_frame = 0;
  AVRational tb;
  int ret = 0;

  if (!frame)
    return AVERROR(ENOMEM);

  do {
    if ((got_frame = decoder_decode_frame(&vs->auddec, frame, NULL)) < 0) // 音频解码
      goto the_end;

    if (got_frame) {
      tb = (AVRational){1, frame->sample_rate};

      reconfigure =
          cmp_audio_fmts(vs->audio_filter_src.fmt, vs->audio_filter_src.ch_layout.nb_channels, frame->format, frame->ch_layout.nb_channels) ||
          av_channel_layout_compare(&vs->audio_filter_src.ch_layout, &frame->ch_layout) ||
          vs->audio_filter_src.freq != frame->sample_rate ||
          vs->auddec.pkt_serial != last_serial;

      /* 判断音频数据是否和 filter 要求一致，不一致需要重新 configure_audio_filters */
      if (reconfigure) {
        char buf1[1024], buf2[1024];
        av_channel_layout_describe(&vs->audio_filter_src.ch_layout, buf1, sizeof(buf1));
        av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
        av_log(NULL, AV_LOG_DEBUG,
               "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
               vs->audio_filter_src.freq, vs->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(vs->audio_filter_src.fmt), buf1, last_serial,
               frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(frame->format), buf2, vs->auddec.pkt_serial);

        vs->audio_filter_src.fmt = frame->format;
        ret = av_channel_layout_copy(&vs->audio_filter_src.ch_layout, &frame->ch_layout);
        if (ret < 0)
          goto the_end;
        vs->audio_filter_src.freq = frame->sample_rate;
        last_serial = vs->auddec.pkt_serial;

        if ((ret = configure_audio_filters(vs, afilters, 1)) < 0)
          goto the_end;
      }

      if ((ret = av_buffersrc_add_frame(vs->in_audio_filter, frame)) < 0)
        goto the_end;

      while ((ret = av_buffersink_get_frame_flags(vs->out_audio_filter, frame, 0)) >= 0) {
        FrameData *fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;
        tb = av_buffersink_get_time_base(vs->out_audio_filter);
        if (!(af = frame_queue_peek_writable(&vs->sampq)))
          goto the_end;

        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        af->pos = fd ? fd->pkt_pos : -1;
        af->serial = vs->auddec.pkt_serial;
        af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});  // 当前帧包含的(单个声道)采样数/采样率就是当前帧的播放时长

        av_frame_move_ref(af->frame, frame);  // 将frame数据拷入af->frame，af->frame指向音频frame队列尾部
        frame_queue_push(&vs->sampq);         // 更新音频frame队列大小及写指针

        if (vs->audioq.serial != vs->auddec.pkt_serial)
          break;
      }
      if (ret == AVERROR_EOF)
        vs->auddec.finished = vs->auddec.pkt_serial;
    }
  } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

the_end:
  avfilter_graph_free(&vs->agraph);
  av_frame_free(&frame);
  return ret;
}

/* 视频解码线程：从视频 packet_queue 中取数据，解码后放入视频 frame_queue */
static int video_thread(void *arg)
{
  VideoState *vs = arg;
  AVFrame *frame = av_frame_alloc();
  double pts;
  double duration;
  int ret;
  AVRational tb = vs->video_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(vs->ifmt_ctx, vs->video_st, NULL);

  AVFilterGraph *graph = NULL;
  AVFilterContext *filt_out = NULL, *filt_in = NULL;
  int last_w = 0;
  int last_h = 0;
  enum AVPixelFormat last_format = -2;
  int last_serial = -1;
  int last_vfilter_idx = 0;

  if (!frame)
    return AVERROR(ENOMEM);

  for (;;) {
    ret = get_video_frame(vs, frame);
    if (ret < 0)
      goto the_end;
    if (!ret)
      continue;

    if (last_w != frame->width || last_h != frame->height || last_format != frame->format || last_serial != vs->viddec.pkt_serial || last_vfilter_idx != vs->vfilter_idx) {
      av_log(NULL, AV_LOG_DEBUG,
             "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
             last_w, last_h,
             (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
             frame->width, frame->height,
             (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), vs->viddec.pkt_serial);
      avfilter_graph_free(&graph);
      graph = avfilter_graph_alloc();
      if (!graph) {
        ret = AVERROR(ENOMEM);
        goto the_end;
      }
      graph->nb_threads = filter_nbthreads;
      if ((ret = configure_video_filters(graph, vs, vfilters_list ? vfilters_list[vs->vfilter_idx] : NULL, frame)) < 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = vs;
        SDL_PushEvent(&event);
        goto the_end;
      }
      filt_in = vs->in_video_filter;
      filt_out = vs->out_video_filter;
      last_w = frame->width;
      last_h = frame->height;
      last_format = frame->format;
      last_serial = vs->viddec.pkt_serial;
      last_vfilter_idx = vs->vfilter_idx;
      frame_rate = av_buffersink_get_frame_rate(filt_out);
    }

    ret = av_buffersrc_add_frame(filt_in, frame);
    if (ret < 0)
      goto the_end;

    while (ret >= 0) {
      FrameData *fd;

      vs->frame_last_returned_time = av_gettime_relative() / 1000000.0;

      ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
      if (ret < 0) {
        if (ret == AVERROR_EOF)
          vs->viddec.finished = vs->viddec.pkt_serial;
        ret = 0;
        break;
      }

      fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;

      vs->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - vs->frame_last_returned_time;
      if (fabs(vs->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
        vs->frame_last_filter_delay = 0;
      tb = av_buffersink_get_time_base(filt_out);
      duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
      pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
      ret = queue_picture(vs, frame, pts, duration, fd ? fd->pkt_pos : -1, vs->viddec.pkt_serial);
      av_frame_unref(frame);
      if (vs->videoq.serial != vs->viddec.pkt_serial)
        break;
    }

    if (ret < 0)
      goto the_end;
  }
the_end:
  avfilter_graph_free(&graph);
  av_frame_free(&frame);
  return 0;
}

/* 字幕解码线程 */
static int subtitle_thread(void *arg)
{
  VideoState *vs = arg;
  Frame *sp;
  int got_subtitle;
  double pts;

  for (;;) {
    if (!(sp = frame_queue_peek_writable(&vs->subpq)))
      return 0;

    if ((got_subtitle = decoder_decode_frame(&vs->subdec, NULL, &sp->sub)) < 0)
      break;

    pts = 0;

    if (got_subtitle && sp->sub.format == 0) {
      if (sp->sub.pts != AV_NOPTS_VALUE)
        pts = sp->sub.pts / (double)AV_TIME_BASE;
      sp->pts = pts;
      sp->serial = vs->subdec.pkt_serial;
      sp->width = vs->subdec.avctx->width;
      sp->height = vs->subdec.avctx->height;
      sp->uploaded = 0;

      /* now we can update the picture count */
      frame_queue_push(&vs->subpq);
    } else if (got_subtitle) {
      avsubtitle_free(&sp->sub);
    }
  }
  return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *vs, short *samples, int samples_size)
{
  int size, len;

  size = samples_size / sizeof(short);
  while (size > 0) {
    len = SAMPLE_ARRAY_SIZE - vs->sample_array_index;
    if (len > size)
      len = size;
    memcpy(vs->sample_array + vs->sample_array_index, samples, len * sizeof(short));
    samples += len;
    vs->sample_array_index += len;
    if (vs->sample_array_index >= SAMPLE_ARRAY_SIZE)
      vs->sample_array_index = 0;
    size -= len;
  }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *vs, int nb_samples)
{
  int wanted_nb_samples = nb_samples;

  /* if not master, then we try to remove or add samples to correct the clock */
  if (get_master_sync_type(vs) != AV_SYNC_AUDIO_MASTER) {
    double diff, avg_diff;
    int min_nb_samples, max_nb_samples;

    diff = get_clock(&vs->audclk) - get_master_clock(vs);

    if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      vs->audio_diff_cum = diff + vs->audio_diff_avg_coef * vs->audio_diff_cum;
      if (vs->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        vs->audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        avg_diff = vs->audio_diff_cum * (1.0 - vs->audio_diff_avg_coef);

        if (fabs(avg_diff) >= vs->audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int)(diff * vs->audio_src.freq);
          min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
               diff, avg_diff, wanted_nb_samples - nb_samples,
               vs->audio_clock, vs->audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so
         reset A-V filter */
      vs->audio_diff_avg_count = 0;
      vs->audio_diff_cum = 0;
    }
  }

  return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *vs)
{
  int data_size, resampled_data_size;
  av_unused double audio_clock0;
  int wanted_nb_samples;
  Frame *af;

  if (vs->paused)
    return -1;

  do {
#if defined(_WIN32)
    while (frame_queue_nb_remaining(&vs->sampq) == 0) {
      if ((av_gettime_relative() - audio_callback_time) > 1000000LL * vs->audio_hw_buf_size / vs->audio_tgt.bytes_per_sec / 2)
        return -1;
      av_usleep(1000);
    }
#endif
    if (!(af = frame_queue_peek_readable(&vs->sampq)))
      return -1;
    frame_queue_next(&vs->sampq);
  } while (af->serial != vs->audioq.serial);

  data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                         af->frame->nb_samples,
                                         af->frame->format, 1);

  wanted_nb_samples = synchronize_audio(vs, af->frame->nb_samples);

  if (af->frame->format != vs->audio_src.fmt ||
      av_channel_layout_compare(&af->frame->ch_layout, &vs->audio_src.ch_layout) ||
      af->frame->sample_rate != vs->audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !vs->swr_ctx)) {
    swr_free(&vs->swr_ctx);
    swr_alloc_set_opts2(&vs->swr_ctx,
                        &vs->audio_tgt.ch_layout, vs->audio_tgt.fmt, vs->audio_tgt.freq,
                        &af->frame->ch_layout, af->frame->format, af->frame->sample_rate,
                        0, NULL);
    if (!vs->swr_ctx || swr_init(vs->swr_ctx) < 0) {
      av_log(NULL, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
             af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
             vs->audio_tgt.freq, av_get_sample_fmt_name(vs->audio_tgt.fmt), vs->audio_tgt.ch_layout.nb_channels);
      swr_free(&vs->swr_ctx);
      return -1;
    }
    if (av_channel_layout_copy(&vs->audio_src.ch_layout, &af->frame->ch_layout) < 0)
      return -1;
    vs->audio_src.freq = af->frame->sample_rate;
    vs->audio_src.fmt = af->frame->format;
  }

  if (vs->swr_ctx) {
    const uint8_t **in = (const uint8_t **)af->frame->extended_data;
    uint8_t **out = &vs->audio_buf1;
    int out_count = (int64_t)wanted_nb_samples * vs->audio_tgt.freq / af->frame->sample_rate + 256;
    int out_size = av_samples_get_buffer_size(NULL, vs->audio_tgt.ch_layout.nb_channels, out_count, vs->audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation(vs->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * vs->audio_tgt.freq / af->frame->sample_rate,
                               wanted_nb_samples * vs->audio_tgt.freq / af->frame->sample_rate) < 0) {
        av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
        return -1;
      }
    }
    av_fast_malloc(&vs->audio_buf1, &vs->audio_buf1_size, out_size);
    if (!vs->audio_buf1)
      return AVERROR(ENOMEM);
    len2 = swr_convert(vs->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
    }
    if (len2 == out_count) {
      av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init(vs->swr_ctx) < 0)
        swr_free(&vs->swr_ctx);
    }
    vs->audio_buf = vs->audio_buf1;
    resampled_data_size = len2 * vs->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(vs->audio_tgt.fmt);
  } else {
    vs->audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
  }

  audio_clock0 = vs->audio_clock;
  /* update the audio clock with the pts */
  if (!isnan(af->pts))
    vs->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
  else
    vs->audio_clock = NAN;
  vs->audio_clock_serial = af->serial;
#ifdef DEBUG
  {
    static double last_clock;
    printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
           vs->audio_clock - last_clock,
           vs->audio_clock, audio_clock0);
    last_clock = vs->audio_clock;
  }
#endif
  return resampled_data_size;
}

/*!
 * @brief 音频处理回调函数。读队列获取音频包，解码，播放。
 *        此函数被SDL按需调用，此函数不在用户主线程中，因此数据需要保护
 *        prepare a new audio buffer
 * @param [in] opaque 用户在注册回调函数时指定的参数
 * @param [out] stream 音频数据缓冲区地址，将解码后的音频数据填入此缓冲区
 * @param [in] len 音频数据缓冲区大小，单位字节;
 *
 * @note 回调函数返回后，stream指向的音频缓冲区将变为无效。
 *       双声道采样点的顺序为LRLRLR
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
  VideoState *vs = opaque;
  int audio_size, len1;

  audio_callback_time = av_gettime_relative();

  while (len > 0) {
    if (vs->audio_buf_index >= vs->audio_buf_size) {
      audio_size = audio_decode_frame(vs);
      if (audio_size < 0) {
        /* if error, just output silence */
        vs->audio_buf = NULL;
        vs->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / vs->audio_tgt.frame_size * vs->audio_tgt.frame_size;
      } else {
        if (vs->show_mode != SHOW_MODE_VIDEO)
          update_sample_display(vs, (int16_t *)vs->audio_buf, audio_size);
        vs->audio_buf_size = audio_size;
      }
      vs->audio_buf_index = 0;
    }
    len1 = vs->audio_buf_size - vs->audio_buf_index;
    if (len1 > len)
      len1 = len;
    if (!vs->muted && vs->audio_buf && vs->audio_volume == SDL_MIX_MAXVOLUME)
      memcpy(stream, (uint8_t *)vs->audio_buf + vs->audio_buf_index, len1);
    else {
      memset(stream, 0, len1);
      if (!vs->muted && vs->audio_buf)
        SDL_MixAudioFormat(stream, (uint8_t *)vs->audio_buf + vs->audio_buf_index, AUDIO_S16SYS, len1, vs->audio_volume);
    }
    len -= len1;
    stream += len1;
    vs->audio_buf_index += len1;
  }
  vs->audio_write_buf_size = vs->audio_buf_size - vs->audio_buf_index;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!isnan(vs->audio_clock)) {
    set_clock_at(&vs->audclk, vs->audio_clock - (double)(2 * vs->audio_hw_buf_size + vs->audio_write_buf_size) / vs->audio_tgt.bytes_per_sec, vs->audio_clock_serial, audio_callback_time / 1000000.0);
    sync_clock_to_slave(&vs->extclk, &vs->audclk);
  }
}

/**
 * @brief 打开音频输出 / 音频播放；
 * @param [in] opaque  提供给回调函数的参数
 * @param [in] wanted_channel_layout 希望的通道布局
 * @param [in] wanted_sample_rate 希望的采样率
 * @param [out] audio_hw_params 存储成功打开后的 SDL支持的音频参数
 * @return 成功返回 播放区buf大小，失败返回 -1
 */
static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
  SDL_AudioSpec wanted_spec, spec;
  const char *env;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
  int wanted_nb_channels = wanted_channel_layout->nb_channels;

  env = SDL_getenv("SDL_AUDIO_CHANNELS");
  if (env) {
    wanted_nb_channels = atoi(env);
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
  }
  if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
  }
  wanted_nb_channels = wanted_channel_layout->nb_channels;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
  }
  while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
    next_sample_rate_idx--;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = sdl_audio_callback;
  wanted_spec.userdata = opaque;
  while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
        return -1;
      }
    }
    av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
  }
  if (spec.format != AUDIO_S16SYS) {
    av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
  }
  if (spec.channels != wanted_spec.channels) {
    av_channel_layout_uninit(wanted_channel_layout);
    av_channel_layout_default(wanted_channel_layout, spec.channels);
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
      av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
    }
  }

  audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
  audio_hw_params->freq = spec.freq;
  if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
    return -1;
  audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
  audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
  if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
    av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }
  return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *vs, int stream_index)
{
  AVFormatContext *ifmt_ctx = vs->ifmt_ctx;
  AVCodecContext *avctx;
  AVStream *av_stream;
  const AVCodec *codec;
  const char *forced_codec_name = NULL;
  AVDictionary *opts = NULL;
  const AVDictionaryEntry *t = NULL;
  int sample_rate;
  AVChannelLayout ch_layout = {0};
  int ret = 0;
  int stream_lowres = lowres;

  if (stream_index < 0 || stream_index >= ifmt_ctx->nb_streams)
    return -1;

  avctx = avcodec_alloc_context3(NULL);
  if (!avctx)
    return AVERROR(ENOMEM);

  av_stream = ifmt_ctx->streams[stream_index];
  ret = avcodec_parameters_to_context(avctx, av_stream->codecpar);
  if (ret < 0)
    goto fail;
  avctx->pkt_timebase = av_stream->time_base;

  /* 解码器的创建。如果有指定解码器，根据指定创建；否则根据流信息中的 codec_id 创建 */
  switch (avctx->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    vs->last_audio_stream = stream_index;
    forced_codec_name = audio_codec_name;
    break;
  case AVMEDIA_TYPE_SUBTITLE:
    vs->last_subtitle_stream = stream_index;
    forced_codec_name = subtitle_codec_name;
    break;
  case AVMEDIA_TYPE_VIDEO:
    vs->last_video_stream = stream_index;
    forced_codec_name = video_codec_name;
    break;
  }
  if (forced_codec_name)
    codec = avcodec_find_decoder_by_name(forced_codec_name);
  else
    codec = avcodec_find_decoder(avctx->codec_id);
  if (!codec) {
    if (forced_codec_name)
      av_log(NULL, AV_LOG_WARNING, "No codec could be found with name '%s'\n", forced_codec_name);
    else
      av_log(NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
    ret = AVERROR(EINVAL);
    goto fail;
  }

  avctx->codec_id = codec->id;
  if (stream_lowres > codec->max_lowres) {
    av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
    stream_lowres = codec->max_lowres;
  }
  avctx->lowres = stream_lowres;

  if (fast)
    avctx->flags2 |= AV_CODEC_FLAG2_FAST;

  /* 过滤掉给定编解码器的选项。创建一个新的选项字典，其中仅包含适用于ID为codec_id的编解码器的选项。 */
  ret = filter_codec_opts(codec_opts, avctx->codec_id, ifmt_ctx, av_stream, codec, &opts);
  if (ret < 0)
    goto fail;

  if (!av_dict_get(opts, "threads", NULL, 0))
    av_dict_set(&opts, "threads", "auto", 0);
  if (stream_lowres)
    av_dict_set_int(&opts, "lowres", stream_lowres, 0);

  av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

  if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
    goto fail;
  }
  if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }

  vs->eof = 0;
  av_stream->discard = AVDISCARD_DEFAULT;
  switch (avctx->codec_type) {
  case AVMEDIA_TYPE_AUDIO: {
    {
      AVFilterContext *sink;

      vs->audio_filter_src.freq = avctx->sample_rate;
      ret = av_channel_layout_copy(&vs->audio_filter_src.ch_layout, &avctx->ch_layout);
      if (ret < 0)
        goto fail;
      vs->audio_filter_src.fmt = avctx->sample_fmt;
      if ((ret = configure_audio_filters(vs, afilters, 0)) < 0)
        goto fail;
      sink = vs->out_audio_filter;
      sample_rate = av_buffersink_get_sample_rate(sink);
      ret = av_buffersink_get_ch_layout(sink, &ch_layout);
      if (ret < 0)
        goto fail;
    }

    /* prepare audio output */
    if ((ret = audio_open(vs, &ch_layout, sample_rate, &vs->audio_tgt)) < 0)
      goto fail;
    vs->audio_hw_buf_size = ret;
    vs->audio_src = vs->audio_tgt;
    vs->audio_buf_size = 0;
    vs->audio_buf_index = 0;

    /* init averaging filter */
    vs->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    vs->audio_diff_avg_count = 0;
    /* since we do not have a precise anough audio FIFO fullness,
       we correct audio sync only if larger than this threshold */
    vs->audio_diff_threshold = (double)(vs->audio_hw_buf_size) / vs->audio_tgt.bytes_per_sec;

    vs->audio_stream = stream_index;
    vs->audio_st = av_stream;

    if ((ret = decoder_init(&vs->auddec, avctx, &vs->audioq, vs->continue_read_thread)) < 0)
      goto fail;
    if (vs->ifmt_ctx->iformat->flags & AVFMT_NOTIMESTAMPS) {
      vs->auddec.start_pts = vs->audio_st->start_time;
      vs->auddec.start_pts_tb = vs->audio_st->time_base;
    }
    if ((ret = decoder_start(&vs->auddec, audio_thread, "audio_decoder", vs)) < 0)
      goto out;
    SDL_PauseAudioDevice(audio_dev, 0);
    break;
  }
  case AVMEDIA_TYPE_VIDEO: {
    vs->video_stream = stream_index;
    vs->video_st = av_stream;

    if ((ret = decoder_init(&vs->viddec, avctx, &vs->videoq, vs->continue_read_thread)) < 0)
      goto fail;
    if ((ret = decoder_start(&vs->viddec, video_thread, "video_decoder", vs)) < 0)
      goto out;
    vs->queue_attachments_req = 1;
    break;
  }
  case AVMEDIA_TYPE_SUBTITLE: {
    vs->subtitle_stream = stream_index;
    vs->subtitle_st = av_stream;

    if ((ret = decoder_init(&vs->subdec, avctx, &vs->subtitleq, vs->continue_read_thread)) < 0)
      goto fail;
    if ((ret = decoder_start(&vs->subdec, subtitle_thread, "subtitle_decoder", vs)) < 0)
      goto out;
    break;
  }
  default:
    break;
  }
  goto out;

fail:
  avcodec_free_context(&avctx);
out:
  av_channel_layout_uninit(&ch_layout);
  av_dict_free(&opts);

  return ret;
}

static int decode_interrupt_cb(void *ctx)
{
  VideoState *vs = ctx;
  return vs->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
  return stream_id < 0 ||
         queue->abort_request ||
         (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
         queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

/*!
 * @brief 判定是否为实时码流；
 *        当输入格式名中含有 "rtp"、"rtsp"、"sdp" 或 输入地址中含有 "rtp:"、"udp:" 则判断为实时码流
 * @param s
 * @return
 */
static int is_realtime(AVFormatContext *s)
{
  if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
    return 1;

  if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
    return 1;
  return 0;
}

/* 解复用线程 this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
  VideoState *vs = arg;
  AVFormatContext *ifmt_ctx = NULL; // 输入源格式上下文
  int err, i, ret;
  int st_index[AVMEDIA_TYPE_NB];
  AVPacket *pkt = NULL;
  int64_t stream_start_time;
  int pkt_in_play_range = 0;
  const AVDictionaryEntry *t;
  SDL_mutex *wait_mutex = SDL_CreateMutex();
  int scan_all_pmts_set = 0;
  int64_t pkt_ts;

  if (!wait_mutex) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  memset(st_index, -1, sizeof(st_index));
  vs->eof = 0;

  pkt = av_packet_alloc();
  if (!pkt) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  ifmt_ctx = avformat_alloc_context();
  if (!ifmt_ctx) {
    av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
    ret = AVERROR(ENOMEM);
    goto fail;
  }
  ifmt_ctx->interrupt_callback.callback = decode_interrupt_cb;
  ifmt_ctx->interrupt_callback.opaque = vs;

  /**
   * 查询并处理 scan_all_pmts 选项。 如果 scan_all_pmts 选项未设置，则设置为 1
   * "scan_all_pmts" 是 FFmpeg 中的一个选项，用于控制是否扫描所有的 Program Map Tables (PMTs)。
   * PMTs 是 MPEG-2 传输流中的一种数据表，用于存储每个频道的程序信息，包括每个程序的元数据以及每个程序中的音频、视频和数据流的 PID（Packet ID）。
   * 在一些情况下，一些流可能不会在 PAT（Program Association Table）中列出所有的 PMTs，或者 PMTs 可能会在流的传输过程中改变。
   * 在这些情况下，设置 "scan_all_pmts" 选项为 "1" 可以让 FFmpeg 扫描所有的 PMTs，以确保能够正确地解析流。
  */
  if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
    av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    scan_all_pmts_set = 1;
  }

  err = avformat_open_input(&ifmt_ctx, vs->filename, vs->iformat, &format_opts);
  if (err < 0) {
    print_error(vs->filename, err);
    ret = -1;
    goto fail;
  }
  if (scan_all_pmts_set)  // 如果是有手动处理 scan_all_pmts 选项，在 avformat_open_input 之后需要还原回去。
    av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

  /* 如果 format_opts 中有未被使用的选项，表明该选项有误，即命令有误，这将终止程序执行 */
  if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
    av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto fail;
  }
  vs->ifmt_ctx = ifmt_ctx;

  if (genpts)
    ifmt_ctx->flags |= AVFMT_FLAG_GENPTS;

  if (find_stream_info) {
    AVDictionary **opts;
    int orig_nb_streams = ifmt_ctx->nb_streams;

    err = setup_find_stream_info_opts(ifmt_ctx, codec_opts, &opts);
    if (err < 0) {
      av_log(NULL, AV_LOG_ERROR, "Error setting up avformat_find_stream_info() options\n");
      ret = err;
      goto fail;
    }

    err = avformat_find_stream_info(ifmt_ctx, opts);

    for (i = 0; i < orig_nb_streams; i++)
      av_dict_free(&opts[i]);
    av_freep(&opts);

    if (err < 0) {
      av_log(NULL, AV_LOG_WARNING, "%s: could not find codec parameters\n", vs->filename);
      ret = -1;
      goto fail;
    }
  }

  if (ifmt_ctx->pb)
    ifmt_ctx->pb->eof_reached = 0;  // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

  if (seek_by_bytes < 0)
    seek_by_bytes = !(ifmt_ctx->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                    !!(ifmt_ctx->iformat->flags & AVFMT_TS_DISCONT) &&
                    strcmp("ogg", ifmt_ctx->iformat->name);

  vs->max_frame_duration = (ifmt_ctx->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  if (!window_title && (t = av_dict_get(ifmt_ctx->metadata, "title", NULL, 0)))
    window_title = av_asprintf("%s - %s", t->value, input_filename);

  /* if seeking requested, we execute it */
  if (start_time != AV_NOPTS_VALUE) {
    int64_t timestamp;

    timestamp = start_time;
    /* add the stream start time */
    if (ifmt_ctx->start_time != AV_NOPTS_VALUE)
      timestamp += ifmt_ctx->start_time;
    ret = avformat_seek_file(ifmt_ctx, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0) {
      av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n", vs->filename, (double)timestamp / AV_TIME_BASE);
    }
  }

  vs->realtime = is_realtime(ifmt_ctx);

  if (show_status)
    av_dump_format(ifmt_ctx, 0, vs->filename, 0);

  for (i = 0; i < ifmt_ctx->nb_streams; i++) {
    AVStream *st = ifmt_ctx->streams[i];
    enum AVMediaType type = st->codecpar->codec_type;
    st->discard = AVDISCARD_ALL;
    if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
      if (avformat_match_stream_specifier(ifmt_ctx, st, wanted_stream_spec[type]) > 0)
        st_index[type] = i;
  }
  for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
    if (wanted_stream_spec[i] && st_index[i] == -1) {
      av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string(i));
      st_index[i] = INT_MAX;
    }
  }

  if (!video_disable)
    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
  if (!audio_disable)
    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);
  if (!video_disable && !subtitle_disable)
    st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_SUBTITLE,
                            st_index[AVMEDIA_TYPE_SUBTITLE],
                            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO] : st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);

  vs->show_mode = show_mode;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    AVStream *st = ifmt_ctx->streams[st_index[AVMEDIA_TYPE_VIDEO]];
    AVCodecParameters *codecpar = st->codecpar;
    AVRational sar = av_guess_sample_aspect_ratio(ifmt_ctx, st, NULL);
    if (codecpar->width)
      set_default_window_size(codecpar->width, codecpar->height, sar);
  }

  /* open the streams */
  if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
    stream_component_open(vs, st_index[AVMEDIA_TYPE_AUDIO]);
  }

  ret = -1;
  if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
    ret = stream_component_open(vs, st_index[AVMEDIA_TYPE_VIDEO]);
  }
  if (vs->show_mode == SHOW_MODE_NONE)
    vs->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

  if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
    stream_component_open(vs, st_index[AVMEDIA_TYPE_SUBTITLE]);
  }

  if (vs->video_stream < 0 && vs->audio_stream < 0) {
    av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", vs->filename);
    ret = -1;
    goto fail;
  }

  if (infinite_buffer < 0 && vs->realtime)
    infinite_buffer = 1;

  for (;;) {
    if (vs->abort_request)
      break;
    if (vs->paused != vs->last_paused) {
      vs->last_paused = vs->paused;
      if (vs->paused)
        vs->read_pause_return = av_read_pause(ifmt_ctx);
      else
        av_read_play(ifmt_ctx);
    }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
    if (vs->paused && (!strcmp(ifmt_ctx->iformat->name, "rtsp") || (ifmt_ctx->pb && !strncmp(input_filename, "mmsh:", 5)))) {
      /* wait 10 ms to avoid trying to get another packet */
      /* XXX: horrible */
      SDL_Delay(10);
      continue;
    }
#endif

    if (vs->seek_req) {
      int64_t seek_target = vs->seek_pos;
      int64_t seek_min = vs->seek_rel > 0 ? seek_target - vs->seek_rel + 2 : INT64_MIN;
      int64_t seek_max = vs->seek_rel < 0 ? seek_target - vs->seek_rel - 2 : INT64_MAX;
      // FIXME the +-2 is due to rounding being not done in the correct direction in generation
      //      of the seek_pos/seek_rel variables

      ret = avformat_seek_file(vs->ifmt_ctx, -1, seek_min, seek_target, seek_max, vs->seek_flags);
      if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", vs->ifmt_ctx->url);
      } else {
        if (vs->audio_stream >= 0)
          packet_queue_flush(&vs->audioq);
        if (vs->subtitle_stream >= 0)
          packet_queue_flush(&vs->subtitleq);
        if (vs->video_stream >= 0)
          packet_queue_flush(&vs->videoq);
        if (vs->seek_flags & AVSEEK_FLAG_BYTE) {
          set_clock(&vs->extclk, NAN, 0);
        } else {
          set_clock(&vs->extclk, seek_target / (double)AV_TIME_BASE, 0);
        }
      }
      vs->seek_req = 0;
      vs->queue_attachments_req = 1;
      vs->eof = 0;
      if (vs->paused)
        step_to_next_frame(vs);
    }
    if (vs->queue_attachments_req) {
      if (vs->video_st && vs->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        if ((ret = av_packet_ref(pkt, &vs->video_st->attached_pic)) < 0)
          goto fail;
        packet_queue_put(&vs->videoq, pkt);
        packet_queue_put_nullpacket(&vs->videoq, pkt, vs->video_stream);
      }
      vs->queue_attachments_req = 0;
    }

    /* if the queue are full, no need to read more */
    if (infinite_buffer < 1 &&
        (vs->audioq.size + vs->videoq.size + vs->subtitleq.size > MAX_QUEUE_SIZE ||
         (stream_has_enough_packets(vs->audio_st, vs->audio_stream, &vs->audioq) &&
          stream_has_enough_packets(vs->video_st, vs->video_stream, &vs->videoq) &&
          stream_has_enough_packets(vs->subtitle_st, vs->subtitle_stream, &vs->subtitleq)))) {
      /* wait 10 ms */
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(vs->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    }
    if (!vs->paused &&
        (!vs->audio_st || (vs->auddec.finished == vs->audioq.serial && frame_queue_nb_remaining(&vs->sampq) == 0)) &&
        (!vs->video_st || (vs->viddec.finished == vs->videoq.serial && frame_queue_nb_remaining(&vs->pictq) == 0))) {
      if (loop != 1 && (!loop || --loop)) {
        stream_seek(vs, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
      } else if (autoexit) {
        ret = AVERROR_EOF;
        goto fail;
      }
    }
    ret = av_read_frame(ifmt_ctx, pkt);
    if (ret < 0) {
      if ((ret == AVERROR_EOF || avio_feof(ifmt_ctx->pb)) && !vs->eof) {
        if (vs->video_stream >= 0)
          packet_queue_put_nullpacket(&vs->videoq, pkt, vs->video_stream);
        if (vs->audio_stream >= 0)
          packet_queue_put_nullpacket(&vs->audioq, pkt, vs->audio_stream);
        if (vs->subtitle_stream >= 0)
          packet_queue_put_nullpacket(&vs->subtitleq, pkt, vs->subtitle_stream);
        vs->eof = 1;
      }
      if (ifmt_ctx->pb && ifmt_ctx->pb->error) {
        if (autoexit)
          goto fail;
        else
          break;
      }
      SDL_LockMutex(wait_mutex);
      SDL_CondWaitTimeout(vs->continue_read_thread, wait_mutex, 10);
      SDL_UnlockMutex(wait_mutex);
      continue;
    } else {
      vs->eof = 0;
    }
    /* check if packet is in play range specified by user, then queue, otherwise discard */
    stream_start_time = ifmt_ctx->streams[pkt->stream_index]->start_time;
    pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                        (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                                    av_q2d(ifmt_ctx->streams[pkt->stream_index]->time_base) -
                                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000 <=
                            ((double)duration / 1000000);
    if (pkt->stream_index == vs->audio_stream && pkt_in_play_range) {
      packet_queue_put(&vs->audioq, pkt);
    } else if (pkt->stream_index == vs->video_stream && pkt_in_play_range && !(vs->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      packet_queue_put(&vs->videoq, pkt);
    } else if (pkt->stream_index == vs->subtitle_stream && pkt_in_play_range) {
      packet_queue_put(&vs->subtitleq, pkt);
    } else {
      av_packet_unref(pkt);
    }
  }

  ret = 0;
fail:
  if (ifmt_ctx && !vs->ifmt_ctx)
    avformat_close_input(&ifmt_ctx);

  av_packet_free(&pkt);
  if (ret != 0) {
    SDL_Event event;

    event.type = FF_QUIT_EVENT;
    event.user.data1 = vs;
    SDL_PushEvent(&event);
  }
  SDL_DestroyMutex(wait_mutex);
  return 0;
}

/*!
 * @brief 创建 VideoState 对象； 音、视、字 的 包队列、帧队列、时钟 的初始化；创建解复用线程；
 * @param [in] filename 文件名
 * @param [in] iformat 输入格式
 * @return 返回 VideoState 对象指针
 */
static VideoState *stream_open(const char *filename, const AVInputFormat *iformat)
{
  VideoState *vs;

  vs = av_mallocz(sizeof(VideoState));
  if (!vs)
    return NULL;
  vs->last_video_stream = vs->video_stream = -1;
  vs->last_audio_stream = vs->audio_stream = -1;
  vs->last_subtitle_stream = vs->subtitle_stream = -1;
  vs->filename = av_strdup(filename);
  if (!vs->filename)
    goto fail;
  vs->iformat = iformat;
  vs->ytop = 0;
  vs->xleft = 0;

  /* start video display */
  if (frame_queue_init(&vs->pictq, &vs->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
    goto fail;
  if (frame_queue_init(&vs->subpq, &vs->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
    goto fail;
  if (frame_queue_init(&vs->sampq, &vs->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
    goto fail;

  if (packet_queue_init(&vs->videoq) < 0 ||
      packet_queue_init(&vs->audioq) < 0 ||
      packet_queue_init(&vs->subtitleq) < 0)
    goto fail;

  if (!(vs->continue_read_thread = SDL_CreateCond())) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
    goto fail;
  }

  init_clock(&vs->vidclk, &vs->videoq.serial);
  init_clock(&vs->audclk, &vs->audioq.serial);
  init_clock(&vs->extclk, &vs->extclk.serial);
  vs->audio_clock_serial = -1;

  /* 音量归一化 */
  if (startup_volume < 0)
    av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
  if (startup_volume > 100)
    av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
  startup_volume = av_clip(startup_volume, 0, 100);
  startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

  vs->audio_volume = startup_volume;
  vs->muted = 0;
  vs->av_sync_type = av_sync_type;
  vs->read_tid = SDL_CreateThread(read_thread, "read_thread", vs);
  if (!vs->read_tid) {
    av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
    goto fail;
  }

  return vs;

fail:
  stream_close(vs);
  return NULL;
}

static void stream_cycle_channel(VideoState *vs, int codec_type)
{
  AVFormatContext *ic = vs->ifmt_ctx;
  int start_index, stream_index;
  int old_index;
  AVStream *st;
  AVProgram *p = NULL;
  int nb_streams = vs->ifmt_ctx->nb_streams;

  if (codec_type == AVMEDIA_TYPE_VIDEO) {
    start_index = vs->last_video_stream;
    old_index = vs->video_stream;
  } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
    start_index = vs->last_audio_stream;
    old_index = vs->audio_stream;
  } else {
    start_index = vs->last_subtitle_stream;
    old_index = vs->subtitle_stream;
  }
  stream_index = start_index;

  if (codec_type != AVMEDIA_TYPE_VIDEO && vs->video_stream != -1) {
    p = av_find_program_from_stream(ic, NULL, vs->video_stream);
    if (p) {
      nb_streams = p->nb_stream_indexes;
      for (start_index = 0; start_index < nb_streams; start_index++)
        if (p->stream_index[start_index] == stream_index)
          break;
      if (start_index == nb_streams)
        start_index = -1;
      stream_index = start_index;
    }
  }

  for (;;) {
    if (++stream_index >= nb_streams) {
      if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
        stream_index = -1;
        vs->last_subtitle_stream = -1;
        goto the_end;
      }
      if (start_index == -1)
        return;
      stream_index = 0;
    }
    if (stream_index == start_index)
      return;
    st = vs->ifmt_ctx->streams[p ? p->stream_index[stream_index] : stream_index];
    if (st->codecpar->codec_type == codec_type) {
      /* check that parameters are OK */
      switch (codec_type) {
      case AVMEDIA_TYPE_AUDIO:
        if (st->codecpar->sample_rate != 0 &&
            st->codecpar->ch_layout.nb_channels != 0)
          goto the_end;
        break;
      case AVMEDIA_TYPE_VIDEO:
      case AVMEDIA_TYPE_SUBTITLE:
        goto the_end;
      default:
        break;
      }
    }
  }
the_end:
  if (p && stream_index != -1)
    stream_index = p->stream_index[stream_index];
  av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
         av_get_media_type_string(codec_type),
         old_index,
         stream_index);

  stream_component_close(vs, old_index);
  stream_component_open(vs, stream_index);
}

static void toggle_full_screen(VideoState *vs)
{
  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *vs)
{
  int next = vs->show_mode;
  do {
    next = (next + 1) % SHOW_MODE_NB;
  } while (next != vs->show_mode && (next == SHOW_MODE_VIDEO && !vs->video_st || next != SHOW_MODE_VIDEO && !vs->audio_st));
  if (vs->show_mode != next) {
    vs->force_refresh = 1;
    vs->show_mode = next;
  }
}

/*!
 * @brief 等待和处理SDL事件，同时刷新视频状态。
 * @param [in] is 
 * @param [out] event 
 */
static void refresh_loop_wait_event(VideoState *vs, SDL_Event *event)
{
  double remaining_time = 0.0;
  SDL_PumpEvents();   // 更新内部事件队列
  while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    /* 控制光标的隐藏 */
    if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }

    /* 控制刷新率，减少频繁 SDL_PeepEvents 带来的性能损失 */
    if (remaining_time > 0.0)
      av_usleep((int64_t)(remaining_time * 1000000.0));
    remaining_time = REFRESH_RATE;

    /* 如果视频处于播放状态（非暂停状态）或者强制刷新，那么就刷新视频 */
    if (vs->show_mode != SHOW_MODE_NONE && (!vs->paused || vs->force_refresh))
      video_refresh(vs, &remaining_time);

    SDL_PumpEvents(); // 更新内部事件队列
  }
}

/**
 * @brief 跳转视频章节
 * @param [in] is 
 * @param [in] incr >0 向前跳转， <0 向后跳转
 */
static void seek_chapter(VideoState *vs, int incr)
{
  int64_t pos = get_master_clock(vs) * AV_TIME_BASE;
  int i;

  if (!vs->ifmt_ctx->nb_chapters) // 如果视频没有章节，直接返回
    return;

  /* find the current chapter */
  for (i = 0; i < vs->ifmt_ctx->nb_chapters; i++) {
    AVChapter *ch = vs->ifmt_ctx->chapters[i];
    if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
      i--;
      break;
    }
  }

  i += incr;
  i = FFMAX(i, 0);
  if (i >= vs->ifmt_ctx->nb_chapters)
    return;

  av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);

  /* 计算跳转位置，并完成跳转 */
  pos = av_rescale_q(vs->ifmt_ctx->chapters[i]->start, vs->ifmt_ctx->chapters[i]->time_base, AV_TIME_BASE_Q);
  stream_seek(vs, pos, 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
  SDL_Event event;
  double incr, pos, frac;

  for (;;) {
    double x;
    refresh_loop_wait_event(cur_stream, &event);
    switch (event.type) {
    case SDL_KEYDOWN: {
      if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
        do_exit(cur_stream);
        break;
      }
      // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
      if (!cur_stream->width)
        continue;
      switch (event.key.keysym.sym) {
      case SDLK_f:
        toggle_full_screen(cur_stream);
        cur_stream->force_refresh = 1;
        break;
      case SDLK_p:
      case SDLK_SPACE:
        toggle_pause(cur_stream);
        break;
      case SDLK_m:
        toggle_mute(cur_stream);
        break;
      case SDLK_KP_MULTIPLY:
      case SDLK_0:
        update_volume(cur_stream, 1, SDL_VOLUME_STEP);
        break;
      case SDLK_KP_DIVIDE:
      case SDLK_9:
        update_volume(cur_stream, -1, SDL_VOLUME_STEP);
        break;
      case SDLK_s:  // S: Step to next frame
        step_to_next_frame(cur_stream);
        break;
      case SDLK_a:
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
        break;
      case SDLK_v:
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
        break;
      case SDLK_c:
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
        break;
      case SDLK_t:
        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
        break;
      case SDLK_w:
        if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
          if (++cur_stream->vfilter_idx >= nb_vfilters)
            cur_stream->vfilter_idx = 0;
        } else {
          cur_stream->vfilter_idx = 0;
          toggle_audio_display(cur_stream);
        }
        break;
      case SDLK_PAGEUP:
        if (cur_stream->ifmt_ctx->nb_chapters <= 1) {
          incr = 600.0;
          goto do_seek;
        }
        seek_chapter(cur_stream, 1);
        break;
      case SDLK_PAGEDOWN:
        if (cur_stream->ifmt_ctx->nb_chapters <= 1) {
          incr = -600.0;
          goto do_seek;
        }
        seek_chapter(cur_stream, -1);
        break;
      case SDLK_LEFT:
        incr = seek_interval ? -seek_interval : -10.0;
        goto do_seek;
      case SDLK_RIGHT:
        incr = seek_interval ? seek_interval : 10.0;
        goto do_seek;
      case SDLK_UP:
        incr = 60.0;
        goto do_seek;
      case SDLK_DOWN:
        incr = -60.0;
      do_seek:
        if (seek_by_bytes) {
          pos = -1;
          if (pos < 0 && cur_stream->video_stream >= 0)
            pos = frame_queue_last_pos(&cur_stream->pictq);
          if (pos < 0 && cur_stream->audio_stream >= 0)
            pos = frame_queue_last_pos(&cur_stream->sampq);
          if (pos < 0)
            pos = avio_tell(cur_stream->ifmt_ctx->pb);
          if (cur_stream->ifmt_ctx->bit_rate)
            incr *= cur_stream->ifmt_ctx->bit_rate / 8.0;
          else
            incr *= 180000.0;
          pos += incr;
          stream_seek(cur_stream, pos, incr, 1);
        } else {
          pos = get_master_clock(cur_stream);
          if (isnan(pos))
            pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
          pos += incr;
          if (cur_stream->ifmt_ctx->start_time != AV_NOPTS_VALUE && pos < cur_stream->ifmt_ctx->start_time / (double)AV_TIME_BASE)
            pos = cur_stream->ifmt_ctx->start_time / (double)AV_TIME_BASE;
          stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
        }
        break;
      default:
        break;
      }
      break;
    }
    case SDL_MOUSEBUTTONDOWN: {
      if (exit_on_mousedown) {
        do_exit(cur_stream);
        break;
      }
      if (event.button.button == SDL_BUTTON_LEFT) {
        static int64_t last_mouse_left_click = 0;
        if (av_gettime_relative() - last_mouse_left_click <= 500000) {
          toggle_full_screen(cur_stream);
          cur_stream->force_refresh = 1;
          last_mouse_left_click = 0;
        } else {
          last_mouse_left_click = av_gettime_relative();
        }
      }
    }
    case SDL_MOUSEMOTION: {
      if (cursor_hidden) {
        SDL_ShowCursor(1);
        cursor_hidden = 0;
      }
      cursor_last_shown = av_gettime_relative();
      if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.button != SDL_BUTTON_RIGHT)
          break;
        x = event.button.x;
      } else {
        if (!(event.motion.state & SDL_BUTTON_RMASK))
          break;
        x = event.motion.x;
      }
      if (seek_by_bytes || cur_stream->ifmt_ctx->duration <= 0) {
        uint64_t size = avio_size(cur_stream->ifmt_ctx->pb);
        stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
      } else {
        int64_t ts;
        int ns, hh, mm, ss;
        int tns, thh, tmm, tss;
        tns = cur_stream->ifmt_ctx->duration / 1000000LL;
        thh = tns / 3600;
        tmm = (tns % 3600) / 60;
        tss = (tns % 60);
        frac = x / cur_stream->width;
        ns = frac * tns;
        hh = ns / 3600;
        mm = (ns % 3600) / 60;
        ss = (ns % 60);
        av_log(NULL, AV_LOG_INFO,
               "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
               hh, mm, ss, thh, tmm, tss);
        ts = frac * cur_stream->ifmt_ctx->duration;
        if (cur_stream->ifmt_ctx->start_time != AV_NOPTS_VALUE)
          ts += cur_stream->ifmt_ctx->start_time;
        stream_seek(cur_stream, ts, 0, 0);
      }
      break;
    }
    case SDL_WINDOWEVENT: {
      switch (event.window.event) {
      case SDL_WINDOWEVENT_SIZE_CHANGED:
        screen_width = cur_stream->width = event.window.data1;
        screen_height = cur_stream->height = event.window.data2;
        if (cur_stream->vis_texture) {
          SDL_DestroyTexture(cur_stream->vis_texture);
          cur_stream->vis_texture = NULL;
        }
      case SDL_WINDOWEVENT_EXPOSED:
        cur_stream->force_refresh = 1;
      }
      break;
    }
    case SDL_QUIT:
    case FF_QUIT_EVENT:
      do_exit(cur_stream);
      break;
    default:
      break;
    }
  }
}

/*!
 * @brief 参数解析：显示窗口宽度 screen_width
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_width(void *optctx, const char *opt, const char *arg)
{
  double num;
  int ret = parse_number(opt, arg, OPT_INT64, 1, INT_MAX, &num);
  if (ret < 0)
    return ret;

  screen_width = num;
  return 0;
}

/*!
 * @brief 参数解析：显示窗口高度 screen_height
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_height(void *optctx, const char *opt, const char *arg)
{
  double num;
  int ret = parse_number(opt, arg, OPT_INT64, 1, INT_MAX, &num);
  if (ret < 0)
    return ret;

  screen_height = num;
  return 0;
}

/*!
 * @brief 参数解析：输入文件格式 file_iformat
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_format(void *optctx, const char *opt, const char *arg)
{
  file_iformat = av_find_input_format(arg);
  if (!file_iformat) {
    av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
    return AVERROR(EINVAL);
  }
  return 0;
}

/*!
 * @brief 参数解析：音视频同步模式 av_sync_type
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_sync(void *optctx, const char *opt, const char *arg)
{
  if (!strcmp(arg, "audio"))
    av_sync_type = AV_SYNC_AUDIO_MASTER;
  else if (!strcmp(arg, "video"))
    av_sync_type = AV_SYNC_VIDEO_MASTER;
  else if (!strcmp(arg, "ext"))
    av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
  else {
    av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
    exit(1);
  }
  return 0;
}

/*!
 * @brief 参数解析：显示模式 show_mode
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
  show_mode = !strcmp(arg, "video") ? SHOW_MODE_VIDEO : !strcmp(arg, "waves") ? SHOW_MODE_WAVES
                                                    : !strcmp(arg, "rdft")    ? SHOW_MODE_RDFT
                                                                              : SHOW_MODE_NONE;

  if (show_mode == SHOW_MODE_NONE) {
    double num;
    int ret = parse_number(opt, arg, OPT_INT, 0, SHOW_MODE_NB - 1, &num);
    if (ret < 0)
      return ret;
    show_mode = num;
  }
  return 0;
}

/*!
 * @brief 参数解析：输入文件 input_filename
 * @param optctx 
 * @param filename 
 * @return 
 */
static int opt_input_file(void *optctx, const char *filename)
{
  if (input_filename) {
    av_log(NULL, AV_LOG_FATAL,
           "Argument '%s' provided as input filename, but '%s' was already specified.\n",
           filename, input_filename);
    return AVERROR(EINVAL);
  }
  if (!strcmp(filename, "-"))
    filename = "fd:";
  input_filename = filename;

  return 0;
}

/*!
 * @brief 参数解析：解码器 audio_codec_name、 video_codec_name、 subtitle_codec_name
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_codec(void *optctx, const char *opt, const char *arg)
{
  const char *spec = strchr(opt, ':');
  if (!spec) {
    av_log(NULL, AV_LOG_ERROR, "No media specifier was specified in '%s' in option '%s'\n", arg, opt);
    return AVERROR(EINVAL);
  }
  spec++;
  switch (spec[0]) {
  case 'a':
    audio_codec_name = arg;
    break;
  case 's':
    subtitle_codec_name = arg;
    break;
  case 'v':
    video_codec_name = arg;
    break;
  default:
    av_log(NULL, AV_LOG_ERROR, "Invalid media specifier '%s' in option '%s'\n", spec, opt);
    return AVERROR(EINVAL);
  }
  return 0;
}

/*!
 * @brief 参数解析：视频过滤器 vfilters_list
 * @param optctx 
 * @param opt 
 * @param arg 
 * @return 
 */
static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
  int ret = GROW_ARRAY(vfilters_list, nb_vfilters);
  if (ret < 0)
    return ret;

  vfilters_list[nb_vfilters - 1] = arg;
  return 0;
}

static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    {"x", HAS_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
    {"y", HAS_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
    {"fs", OPT_BOOL, {&is_full_screen}, "force full screen"},
    {"an", OPT_BOOL, {&audio_disable}, "disable audio"},
    {"vn", OPT_BOOL, {&video_disable}, "disable video"},
    {"sn", OPT_BOOL, {&subtitle_disable}, "disable subtitling"},
    {"ast", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]}, "select desired audio stream", "stream_specifier"},
    {"vst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]}, "select desired video stream", "stream_specifier"},
    {"sst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]}, "select desired subtitle stream", "stream_specifier"},
    {"ss", HAS_ARG | OPT_TIME, {&start_time}, "seek to a given position in seconds", "pos"},
    {"t", HAS_ARG | OPT_TIME, {&duration}, "play  \"duration\" seconds of audio/video", "duration"},
    {"bytes", OPT_INT | HAS_ARG, {&seek_by_bytes}, "seek by bytes 0=off 1=on -1=auto", "val"},
    {"seek_interval", OPT_FLOAT | HAS_ARG, {&seek_interval}, "set seek interval for left/right keys, in seconds", "seconds"},
    {"nodisp", OPT_BOOL, {&display_disable}, "disable graphical display"},
    {"noborder", OPT_BOOL, {&borderless}, "borderless window"},
    {"alwaysontop", OPT_BOOL, {&alwaysontop}, "window always on top"},
    {"volume", OPT_INT | HAS_ARG, {&startup_volume}, "set startup volume 0=min 100=max", "volume"},
    {"f", HAS_ARG, {.func_arg = opt_format}, "force format", "fmt"},
    {"stats", OPT_BOOL | OPT_EXPERT, {&show_status}, "show status", ""},
    {"fast", OPT_BOOL | OPT_EXPERT, {&fast}, "non spec compliant optimizations", ""},
    {"genpts", OPT_BOOL | OPT_EXPERT, {&genpts}, "generate pts", ""},
    {"drp", OPT_INT | HAS_ARG | OPT_EXPERT, {&decoder_reorder_pts}, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    {"lowres", OPT_INT | HAS_ARG | OPT_EXPERT, {&lowres}, "", ""},
    {"sync", HAS_ARG | OPT_EXPERT, {.func_arg = opt_sync}, "set audio-video sync. type (type=audio/video/ext)", "type"},
    {"autoexit", OPT_BOOL | OPT_EXPERT, {&autoexit}, "exit at the end", ""},
    {"exitonkeydown", OPT_BOOL | OPT_EXPERT, {&exit_on_keydown}, "exit on key down", ""},
    {"exitonmousedown", OPT_BOOL | OPT_EXPERT, {&exit_on_mousedown}, "exit on mouse down", ""},
    {"loop", OPT_INT | HAS_ARG | OPT_EXPERT, {&loop}, "set number of times the playback shall be looped", "loop count"},
    {"framedrop", OPT_BOOL | OPT_EXPERT, {&framedrop}, "drop frames when cpu is too slow", ""},
    {"infbuf", OPT_BOOL | OPT_EXPERT, {&infinite_buffer}, "don't limit the input buffer size (useful with realtime streams)", ""},
    {"window_title", OPT_STRING | HAS_ARG, {&window_title}, "set window title", "window title"},
    {"left", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_left}, "set the x position for the left of the window", "x pos"},
    {"top", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_top}, "set the y position for the top of the window", "y pos"},
    {"vf", OPT_EXPERT | HAS_ARG, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
    {"af", OPT_STRING | HAS_ARG, {&afilters}, "set audio filters", "filter_graph"},
    {"rdftspeed", OPT_INT | HAS_ARG | OPT_AUDIO | OPT_EXPERT, {&rdftspeed}, "rdft speed", "msecs"},
    {"showmode", HAS_ARG, {.func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode"},
    {"i", OPT_BOOL, {&dummy}, "read specified file", "input_file"},
    {"codec", HAS_ARG, {.func_arg = opt_codec}, "force decoder", "decoder_name"},
    {"acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&audio_codec_name}, "force audio decoder", "decoder_name"},
    {"scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&subtitle_codec_name}, "force subtitle decoder", "decoder_name"},
    {"vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&video_codec_name}, "force video decoder", "decoder_name"},
    {"autorotate", OPT_BOOL, {&autorotate}, "automatically rotate video", ""},
    {"find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, {&find_stream_info}, "read and decode the streams to fill missing information with heuristics"},
    {"filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, {&filter_nbthreads}, "number of filter threads per graph"},
    {
        NULL,
    },
};

static void show_usage(void)
{
  av_log(NULL, AV_LOG_INFO, "Simple media player\n");
  av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
  av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
  av_log_set_callback(log_callback_help);
  show_usage();
  show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
  show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
  printf("\n");
  show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
  show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
  show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
  printf(
      "\nWhile playing:\n"
      "q, ESC              quit\n"
      "f                   toggle full screen\n"
      "p, SPC              pause\n"
      "m                   toggle mute\n"
      "9, 0                decrease and increase volume respectively\n"
      "/, *                decrease and increase volume respectively\n"
      "a                   cycle audio channel in the current program\n"
      "v                   cycle video channel\n"
      "t                   cycle subtitle channel in the current program\n"
      "c                   cycle program\n"
      "w                   cycle video filters or show modes\n"
      "s                   activate frame-step mode\n"
      "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
      "down/up             seek backward/forward 1 minute\n"
      "page down/page up   seek backward/forward 10 minutes\n"
      "right mouse click   seek to percentage in file corresponding to fraction of width\n"
      "left double-click   toggle full screen\n");
}

/* Called from the main */
int main(int argc, char **argv)
{
  int flags, ret;
  VideoState *vs;

  init_dynload();

  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  parse_loglevel(argc, argv, options);

  /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
  avdevice_register_all();
#endif
  avformat_network_init();

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  show_banner(argc, argv, options);

  ret = parse_options(NULL, argc, argv, options, opt_input_file);
  if (ret < 0)
    exit(ret == AVERROR_EXIT ? 0 : 1);

  if (!input_filename) {
    show_usage();
    av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
    av_log(NULL, AV_LOG_FATAL,
           "Use -h to get full help or, even better, run 'man %s'\n", program_name);
    exit(1);
  }

  if (display_disable) {
    video_disable = 1;
  }
  flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (audio_disable)
    flags &= ~SDL_INIT_AUDIO;
  else {
    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
      SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
  }
  if (display_disable)
    flags &= ~SDL_INIT_VIDEO;
  if (SDL_Init(flags)) {
    av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }

  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  if (!display_disable) {
    int flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
      flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
      av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
    if (borderless)
      flags |= SDL_WINDOW_BORDERLESS;
    else
      flags |= SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (!renderer) {
        av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, 0);
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer, &renderer_info))
          av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
      }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
      do_exit(NULL);
    }
  }

  vs = stream_open(input_filename, file_iformat);
  if (!vs) {
    av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
    do_exit(NULL);
  }

  event_loop(vs);

  /* never returns */

  return 0;
}
