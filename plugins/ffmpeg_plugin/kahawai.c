/*
 * Kahawai raw video demuxer
 * Copyright (c) 2022 Intel
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

#include <st_convert_api.h>
#include <st_pipeline_api.h>

#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"

typedef struct KahawaiFpsDecs {
  enum st_fps st_fps;
  unsigned int min;
  unsigned int max;
} KahawaiFpsDecs;

const static KahawaiFpsDecs fps_table[] = {
    {ST_FPS_P59_94, 5994 - 100, 5994 + 100},    {ST_FPS_P50, 5000 - 100, 5000 + 100},
    {ST_FPS_P29_97, 2997 - 100, 2997 + 100},    {ST_FPS_P25, 2500 - 100, 2500 + 100},
    {ST_FPS_P119_88, 11988 - 100, 11988 + 100},
};
typedef struct KahawaiDemuxerContext {
  const AVClass* class; /**< Class for private options. */

  char* port;
  char* local_addr;
  char* src_addr;
  int udp_port;
  int width;
  int height;
  char* pixel_format;
  AVRational framerate;
  int fb_cnt;

  st_handle dev_handle;
  st20p_rx_handle rx_handle;

#if 0
    struct st20_ext_frame ext_frames[KAHAWAI_FRAME_BUFFER_COUNT];
    AVBufferRef* av_buffers[KAHAWAI_FRAME_BUFFER_COUNT];
    AVBufferRef* av_buffers_keepers[KAHAWAI_FRAME_BUFFER_COUNT];

    struct st_frame *last_frame;
#endif

  pthread_cond_t get_frame_cond;
  pthread_mutex_t get_frame_mutex;

  int64_t frame_counter;
  struct st_frame* frame;
  size_t output_frame_size;

#if 0
    pthread_cond_t read_packet_cond;
    pthread_mutex_t read_packet_mutex;

    pthread_t frame_thread;
    bool stopped;
#endif
} KahawaiDemuxerContext;

static int rx_st20p_frame_available(void* priv) {
  KahawaiDemuxerContext* s = priv;

  pthread_mutex_lock(&(s->get_frame_mutex));
  pthread_cond_signal(&(s->get_frame_cond));
  pthread_mutex_unlock(&(s->get_frame_mutex));

  return 0;
}

#if 0
static void *rx_st20p_frame_thread(void* arg)
{
    KahawaiDemuxerContext *s = (KahawaiDemuxerContext *)arg;
    st20p_rx_handle handle = s->rx_handle;

    struct st_frame *frame = NULL;

    while (!s->stopped) {
        frame = st20p_rx_get_frame(s->rx_handle);
        if (!frame) {
            pthread_mutex_lock(&(s->get_frame_mutex));
            pthread_cond_wait(&(s->get_frame_cond), &(s->get_frame_mutex));
            pthread_mutex_unlock(&(s->get_frame_mutex));
        } else {
            pthread_mutex_lock(&(s->read_packet_mutex));
            s->frame = frame;
            pthread_cond_signal(&(s->read_packet_cond));
            pthread_mutex_unlock(&(s->read_packet_mutex));
        }
    }

    return NULL;
}
#endif

static int kahawai_read_header(AVFormatContext* ctx) {
  KahawaiDemuxerContext* s = ctx->priv_data;

  AVStream* st = NULL;

  enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
  unsigned int fps = 0;

  int packet_size = 0;

  int ret = 0;

  struct st_init_params param;
  struct st20p_rx_ops ops_rx;

  av_log(ctx, AV_LOG_VERBOSE, "kahawai_read_header triggered\n");

  memset(&param, 0, sizeof(param));
  memset(&ops_rx, 0, sizeof(ops_rx));

  if ((NULL == s->port) || (strlen(s->port) > ST_PORT_MAX_LEN)) {
    av_log(ctx, AV_LOG_ERROR, "Invalid port info\n");
    return AVERROR(EINVAL);
  }
  param.num_ports = 1;
  strncpy(param.port[ST_PORT_P], s->port, ST_PORT_MAX_LEN);
  ops_rx.port.num_port = 1;
  strncpy(ops_rx.port.port[ST_PORT_P], s->port, ST_PORT_MAX_LEN);

  if (NULL == s->local_addr) {
    av_log(ctx, AV_LOG_ERROR, "Invalid local IP address\n");
    return AVERROR(EINVAL);
  } else if (sscanf(s->local_addr, "%hhu.%hhu.%hhu.%hhu", &param.sip_addr[ST_PORT_P][0],
                    &param.sip_addr[ST_PORT_P][1], &param.sip_addr[ST_PORT_P][2],
                    &param.sip_addr[ST_PORT_P][3]) != ST_IP_ADDR_LEN) {
    av_log(ctx, AV_LOG_ERROR, "Failed to parse local IP address: %s\n", s->local_addr);
    return AVERROR(EINVAL);
  }

  param.flags = ST_FLAG_BIND_NUMA | ST_FLAG_DEV_AUTO_START_STOP;
  param.log_level = ST_LOG_LEVEL_INFO;  // log level. ERROR, INFO, WARNING
  param.priv = NULL;                    // usr crx pointer
  param.ptp_get_time_fn = NULL;
  param.rx_sessions_cnt_max = 1;
  param.tx_sessions_cnt_max = 0;
  param.lcores = NULL;

  if (NULL == s->src_addr) {
    av_log(ctx, AV_LOG_ERROR, "Invalid source IP address\n");
    return AVERROR(EINVAL);
  } else if (sscanf(
                 s->src_addr, "%hhu.%hhu.%hhu.%hhu", &ops_rx.port.sip_addr[ST_PORT_P][0],
                 &ops_rx.port.sip_addr[ST_PORT_P][1], &ops_rx.port.sip_addr[ST_PORT_P][2],
                 &ops_rx.port.sip_addr[ST_PORT_P][3]) != ST_IP_ADDR_LEN) {
    av_log(ctx, AV_LOG_ERROR, "Failed to parse source IP address: %s\n", s->src_addr);
    return AVERROR(EINVAL);
  }

  if ((s->udp_port < 0) || (s->udp_port > 0xFFFF)) {
    av_log(ctx, AV_LOG_ERROR, "Invalid UDP port: %d\n", s->udp_port);
    return AVERROR(EINVAL);
  }
  ops_rx.port.udp_port[ST_PORT_P] = s->udp_port;

  if (s->width <= 0) {
    av_log(ctx, AV_LOG_ERROR, "Invalid transport width: %d\n", s->width);
    return AVERROR(EINVAL);
  }
  ops_rx.width = s->width;

  if (s->height <= 0) {
    av_log(ctx, AV_LOG_ERROR, "Invalid transport height: %d\n", s->height);
    return AVERROR(EINVAL);
  }
  ops_rx.height = s->height;

  if ((pix_fmt = av_get_pix_fmt(s->pixel_format)) == AV_PIX_FMT_NONE) {
    av_log(ctx, AV_LOG_ERROR, "No such pixel format: %s.\n", s->pixel_format);
    return AVERROR(EINVAL);
  } else if (pix_fmt != AV_PIX_FMT_YUV422P10LE) {
    av_log(ctx, AV_LOG_ERROR, "Only yuv422p10le is supported\n");
    return AVERROR(EINVAL);
  }
  ops_rx.transport_fmt = ST20_FMT_YUV_422_10BIT;
  ops_rx.output_fmt = ST_FRAME_FMT_YUV422RFC4175PG2BE10;

  packet_size = av_image_get_buffer_size(pix_fmt, s->width, s->height, 1);
  if (packet_size < 0) {
    av_log(ctx, AV_LOG_ERROR, "av_image_get_buffer_size failed with %d\n", packet_size);
    return packet_size;
  }
  av_log(ctx, AV_LOG_VERBOSE, "packet size: %d\n", packet_size);

  fps = s->framerate.num * 100 / s->framerate.den;
  for (ret = 0; ret < sizeof(fps_table); ++ret) {
    if ((fps >= fps_table[ret].min) && (fps <= fps_table[ret].max)) {
      ops_rx.fps = fps_table[ret].st_fps;
      break;
    }
  }
  if (ret >= sizeof(fps_table)) {
    av_log(ctx, AV_LOG_ERROR, "Frame rate %0.2f is not supported\n", ((float)fps / 100));
    return AVERROR(EINVAL);
  }

  st = avformat_new_stream(ctx, NULL);
  if (!st) {
    return AVERROR(ENOMEM);
  }

  st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  st->codecpar->codec_id = ctx->iformat->raw_codec_id;
  st->codecpar->format = pix_fmt;
  st->codecpar->width = s->width;
  st->codecpar->height = s->height;
  avpriv_set_pts_info(st, 64, s->framerate.den, s->framerate.num);
  ctx->packet_size = packet_size;
  st->codecpar->bit_rate =
      av_rescale_q(ctx->packet_size, (AVRational){8, 1}, st->time_base);

  // Create device
  s->dev_handle = st_init(&param);
  if (!s->dev_handle) {
    av_log(ctx, AV_LOG_ERROR, "st_init failed\n");
    return AVERROR(EIO);
  }
  av_log(ctx, AV_LOG_VERBOSE, "st_init finished\n");

  ops_rx.name = "st20p";
  ops_rx.priv = s;                 // Handle of priv_data registered to lib
  ops_rx.port.payload_type = 112;  // RX_ST20_PAYLOAD_TYPE
  ops_rx.device = ST_PLUGIN_DEVICE_AUTO;
  ops_rx.notify_frame_available = rx_st20p_frame_available;
  ops_rx.framebuff_cnt = s->fb_cnt;

#if 0
    memset(s->ext_frames, 0, sizeof(s->ext_frames));
    memset(s->av_buffers, 0, sizeof(s->av_buffers));
    memset(s->av_buffers_keepers, 0, sizeof(s->av_buffers_keepers));
    for (int i = 0; i < KAHAWAI_FRAME_BUFFER_COUNT; ++i) {
        s->av_buffers[i] = av_buffer_allocz(ctx->packet_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!s->av_buffers[i]) {
            av_log(ctx, AV_LOG_ERROR, "av_buffer_allocz failed\n");
            st_uninit(s->dev_handle);
            s->dev_handle = NULL;

            for (int j = 0;  j < i; ++j) {
                av_buffer_unref(&s->av_buffers_keepers[j]);
                s->av_buffers_keepers[j] = NULL;

                av_buffer_unref(&s->av_buffers[j]);
                s->av_buffers[j] = NULL;
            }
            return AVERROR(ENOMEM);
        }
        s->ext_frames[i].buf_addr = s->av_buffers[i]->data;
        s->ext_frames[i].buf_len = ctx->packet_size;
        s->av_buffers_keepers[i] = av_buffer_ref(s->av_buffers[i]); // Make sure it's not freed

	 av_log(ctx, AV_LOG_ERROR, "Allocated Framebuf[%d]: 0x%lx\n", i, s->av_buffers[i]->data);
    }
    ops_rx.ext_frames = s->ext_frames;
#endif

  pthread_mutex_init(&(s->get_frame_mutex), NULL);
  pthread_cond_init(&(s->get_frame_cond), NULL);

  s->rx_handle = st20p_rx_create(s->dev_handle, &ops_rx);
  if (!s->rx_handle) {
    av_log(ctx, AV_LOG_ERROR, "st20p_rx_create failed\n");

    st_uninit(s->dev_handle);
    s->dev_handle = NULL;
    return AVERROR(EIO);
  }

  s->output_frame_size = st20p_rx_frame_size(s->rx_handle);
  if (s->output_frame_size <= 0) {
    av_log(ctx, AV_LOG_ERROR, "st20p_rx_frame_size failed\n");

    st20p_rx_free(s->rx_handle);
    s->rx_handle = NULL;

    st_uninit(s->dev_handle);
    s->dev_handle = NULL;

    return AVERROR(EINVAL);
  }

  av_log(ctx, AV_LOG_VERBOSE, "st20p_rx_create finished\n");

  s->frame_counter = 0;
  s->frame = NULL;

#if 0
    s->last_frame = NULL;
    s->stopped = false;

    pthread_mutex_init(&(s->read_packet_mutex), NULL);
    pthread_cond_init(&(s->read_packet_cond), NULL);

    ret = pthread_create(&(s->frame_thread), NULL, rx_st20p_frame_thread, s);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "rx_st20p_frame_thread creation failed with %d\n", ret);

        st20p_rx_free(s->rx_handle);
        s->rx_handle = NULL;

        st_uninit(s->dev_handle);
        s->dev_handle = NULL;

        return AVERROR(EINVAL);
    }
#endif

  return 0;
}

static int kahawai_read_packet(AVFormatContext* ctx, AVPacket* pkt) {
  KahawaiDemuxerContext* s = ctx->priv_data;
  // int frame_num = 0;
  int ret = 0;

  av_log(ctx, AV_LOG_VERBOSE, "kahawai_read_packet triggered\n");

#if 0
    for (int i = 0; i < KAHAWAI_FRAME_BUFFER_COUNT; ++i) {
        av_log(ctx, AV_LOG_ERROR, "Before av_buffers[%d]: 0x%lx size %d\n", i, s->av_buffers[i]->data, s->av_buffers[i]->size);
        av_log(ctx, AV_LOG_ERROR, "Before av_buffers_keepers[%d]: 0x%lx size %d\n", i, s->av_buffers_keepers[i]->data, s->av_buffers_keepers[i]->size);
    }

    pthread_mutex_lock(&(s->read_packet_mutex));
    pthread_cond_wait(&(s->read_packet_cond), &(s->read_packet_mutex));
#endif

  s->frame = st20p_rx_get_frame(s->rx_handle);
  if (!s->frame) {
    pthread_mutex_lock(&(s->get_frame_mutex));
    pthread_cond_wait(&(s->get_frame_cond), &(s->get_frame_mutex));
    pthread_mutex_unlock(&(s->get_frame_mutex));

    s->frame = st20p_rx_get_frame(s->rx_handle);
    if (!s->frame) {
      av_log(ctx, AV_LOG_ERROR, "st20p_rx_get_frame failed\n");
      return AVERROR(EIO);
    }
  }

#if 0
    if (!s->frame && !s->stopped) {
        av_log(ctx, AV_LOG_ERROR, "Empty frame handle\n");

        s->stopped = true;
        pthread_mutex_unlock(&(s->read_packet_mutex));

        return AVERROR(EIO);
    }
#endif

  if (s->frame->data_size != s->output_frame_size) {
    av_log(ctx, AV_LOG_ERROR, "Unexpected frame size received: %lu (%lu expected)\n",
           s->frame->data_size, s->output_frame_size);

    st20p_rx_put_frame(s->rx_handle, s->frame);
    // s->stopped = true;
    // pthread_mutex_unlock(&(s->read_packet_mutex));

    return AVERROR(EIO);
  }

  ret = av_new_packet(pkt, ctx->packet_size);
  if (ret != 0) {
    av_log(ctx, AV_LOG_ERROR, "av_new_packet failed with %d\n", ret);

    st20p_rx_put_frame(s->rx_handle, s->frame);
    // s->stopped = true;
    // pthread_mutex_unlock(&(s->read_packet_mutex));

    return ret;
  }

  ret = st20_rfc4175_422be10_to_y210((struct st20_rfc4175_422_10_pg2_be*)(s->frame->addr),
                                     (uint16_t*)pkt->data, s->width, s->height);
  if (ret != 0) {
    av_log(ctx, AV_LOG_ERROR, "st20_rfc4175_422be10_to_y210 failed with %d\n", ret);

    st20p_rx_put_frame(s->rx_handle, s->frame);
    // s->stopped = true;
    // pthread_mutex_unlock(&(s->read_packet_mutex));

    return ret;
  }
  st20p_rx_put_frame(s->rx_handle, s->frame);
  // pthread_mutex_unlock(&(s->read_packet_mutex));

  pkt->pts = pkt->dts = s->frame_counter++;
  pkt->pos = -1;  // Unused

  av_log(ctx, AV_LOG_VERBOSE, "Got POC %ld\n", pkt->pts);

#if 0
    if (frame->data_size != ctx->packet_size) {
        av_log(ctx, AV_LOG_ERROR, "Unexpected frame size received: %u (%u)\n",
            frame->data_size, ctx->packet_size);

        if (s->last_frame) {
            st20p_rx_put_frame(s->rx_handle, s->last_frame);
        }
        s->last_frame = frame;

        return AVERROR(EIO);
    }

    while (frame_num < KAHAWAI_FRAME_BUFFER_COUNT) {
        av_log(ctx, AV_LOG_ERROR, "Checked Framebuf[%d]: 0x%lx\n",
            frame_num, s->av_buffers_keepers[frame_num]->data);
        if (s->av_buffers_keepers[frame_num]->data == frame->addr) {
            break;
        }
        ++frame_num;
    }

    if (frame_num >= KAHAWAI_FRAME_BUFFER_COUNT) {
        av_log(ctx, AV_LOG_ERROR, "Failed to match the received frame\n");

        if (s->last_frame) {
            st20p_rx_put_frame(s->rx_handle, s->last_frame);
        }
        s->last_frame = frame;

        return AVERROR(EIO);
    }

    av_log(ctx, AV_LOG_ERROR, "Checked Framebuf[%d]: 0x%lx\n", frame_num, s->av_buffers_keepers[frame_num]->data);

    pkt->buf = s->av_buffers_keepers[frame_num];
    pkt->data = s->av_buffers_keepers[frame_num]->data;
    pkt->size = frame->data_size;
    pkt->pts = pkt->dts = s->frame_counter++;
    pkt->pos = -1; //Unused

    if (s->last_frame) {
        av_log(ctx, AV_LOG_ERROR, "Put a frame: 0x%lx\n", s->last_frame->addr);
        st20p_rx_put_frame(s->rx_handle, s->last_frame);
    }
    s->last_frame = frame;

    av_log(ctx, AV_LOG_INFO, "kahawai_read_packet got POC %ld from Framebuf[%d]\n",
        pkt->pts, frame_num);

    for (int i = 0; i < KAHAWAI_FRAME_BUFFER_COUNT; ++i) {
        av_log(ctx, AV_LOG_ERROR, "After av_buffers[%d]: 0x%lx\n", i, s->av_buffers[i]->data);
        av_log(ctx, AV_LOG_ERROR, "After av_buffers_keepers[%d]: 0x%lx\n", i, s->av_buffers_keepers[i]->data);
        s->av_buffers_keepers[i]->data = s->av_buffers[i]->data;
    }
#endif

  return 0;
}

static int kahawai_read_close(AVFormatContext* ctx) {
  KahawaiDemuxerContext* s = ctx->priv_data;

  av_log(ctx, AV_LOG_VERBOSE, "kahawai_read_close triggered\n");

#if 0
    s->stopped = true;

    pthread_cond_signal(&(s->get_frame_cond));
    pthread_cond_signal(&(s->read_packet_cond));

    pthread_join(s->frame_thread, NULL);
    av_log(ctx, AV_LOG_VERBOSE, "rx_st20p_frame_thread finished\n");

    pthread_mutex_destroy(&s->read_packet_mutex);
    pthread_cond_destroy(&s->read_packet_cond);

    if (s->last_frame) {
        st20p_rx_put_frame(s->rx_handle, s->last_frame);
        s->last_frame = NULL;
    }
#endif

  st20p_rx_free(s->rx_handle);
  s->rx_handle = NULL;

  av_log(ctx, AV_LOG_VERBOSE, "st20p_rx_free finished\n");

  pthread_mutex_destroy(&s->get_frame_mutex);
  pthread_cond_destroy(&s->get_frame_cond);

  // Destroy device
  st_uninit(s->dev_handle);
  s->dev_handle = NULL;

  av_log(ctx, AV_LOG_VERBOSE, "st_uninit finished\n");

#if 0
    for (int i = 0; i < KAHAWAI_FRAME_BUFFER_COUNT; ++i) {
        if (s->av_buffers_keepers[i]) {
            av_buffer_unref(&s->av_buffers_keepers[i]);
            s->av_buffers_keepers[i] = NULL;
        }

        if (s->av_buffers[i]) {
            av_buffer_unref(&s->av_buffers[i]);
            s->av_buffers[i] = NULL;
        }
    }
#endif

  return 0;
}

#define OFFSET(x) offsetof(KahawaiDemuxerContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption kahawai_options[] = {
    {"port", "ST port", OFFSET(port), AV_OPT_TYPE_STRING, {.str = NULL}, .flags = DEC},
    {"local_addr",
     "Local IP address",
     OFFSET(local_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {"src_addr",
     "Source IP address",
     OFFSET(src_addr),
     AV_OPT_TYPE_STRING,
     {.str = NULL},
     .flags = DEC},
    {"udp_port",
     "UDP port",
     OFFSET(udp_port),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     DEC},
    {"width",
     "Video frame width",
     OFFSET(width),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     DEC},
    {"height",
     "Video frame height",
     OFFSET(height),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     INT_MAX,
     DEC},
    {"pixel_format",
     "Video frame format",
     OFFSET(pixel_format),
     AV_OPT_TYPE_STRING,
     {.str = "yuv422p10le"},
     .flags = DEC},
    {"framerate",
     "Video frame rate",
     OFFSET(framerate),
     AV_OPT_TYPE_VIDEO_RATE,
     {.str = "25"},
     0,
     INT_MAX,
     DEC},
    {"fb_cnt",
     "Frame buffer count",
     OFFSET(fb_cnt),
     AV_OPT_TYPE_INT,
     {.i64 = 8},
     3,
     8,
     DEC},
    {NULL},
};

static const AVClass kahawai_demuxer_class = {
    .class_name = "kahawai demuxer",
    .item_name = av_default_item_name,
    .option = kahawai_options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

AVInputFormat ff_kahawai_demuxer = {
    .name = "kahawai",
    .long_name = NULL_IF_CONFIG_SMALL("kahawai input device"),
    .priv_data_size = sizeof(KahawaiDemuxerContext),
    .read_header = kahawai_read_header,
    .read_packet = kahawai_read_packet,
    .read_close = kahawai_read_close,
    .flags = AVFMT_NOFILE,
    .extensions = "kahawai",
    .raw_codec_id = AV_CODEC_ID_RAWVIDEO,
    .priv_class = &kahawai_demuxer_class,
};