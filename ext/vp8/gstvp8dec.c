/* VP8 plugin
 * Copyright (C) 2006 David Schleef <ds@schleef.org>
 * Copyright (C) 2008,2009,2010 Entropy Wave Inc
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstbasevideodecoder.h>
#include <gst/video/gstbasevideoutils.h>
#include <gst/base/gstbasetransform.h>
#include <gst/base/gstadapter.h>
#include <gst/video/video.h>
#include <string.h>
#include <math.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

#include "gstvp8utils.h"

GST_DEBUG_CATEGORY (gst_vp8dec_debug);
#define GST_CAT_DEFAULT gst_vp8dec_debug

#define GST_TYPE_VP8_DEC \
  (gst_vp8_dec_get_type())
#define GST_VP8_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VP8_DEC,GstVP8Dec))
#define GST_VP8_DEC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VP8_DEC,GstVP8DecClass))
#define GST_IS_GST_VP8_DEC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VP8_DEC))
#define GST_IS_GST_VP8_DEC_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VP8_DEC))

typedef struct _GstVP8Dec GstVP8Dec;
typedef struct _GstVP8DecClass GstVP8DecClass;

struct _GstVP8Dec
{
  GstBaseVideoDecoder base_video_decoder;

  vpx_codec_ctx_t decoder;

  /* state */

  gboolean decoder_inited;
};

struct _GstVP8DecClass
{
  GstBaseVideoDecoderClass base_video_decoder_class;
};


/* GstVP8Dec signals and args */
enum
{
  LAST_SIGNAL
};

enum
{
  ARG_0
};

static void gst_vp8_dec_finalize (GObject * object);
static void gst_vp8_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_vp8_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_vp8_dec_start (GstBaseVideoDecoder * decoder);
static gboolean gst_vp8_dec_stop (GstBaseVideoDecoder * decoder);
static gboolean gst_vp8_dec_reset (GstBaseVideoDecoder * decoder);
static GstFlowReturn gst_vp8_dec_parse_data (GstBaseVideoDecoder * decoder,
    gboolean at_eos);
static GstFlowReturn gst_vp8_dec_handle_frame (GstBaseVideoDecoder * decoder,
    GstVideoFrame * frame);

GType gst_vp8_dec_get_type (void);

static GstStaticPadTemplate gst_vp8_dec_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-vp8")
    );

static GstStaticPadTemplate gst_vp8_dec_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_YUV ("I420"))
    );

GST_BOILERPLATE (GstVP8Dec, gst_vp8_dec, GstBaseVideoDecoder,
    GST_TYPE_BASE_VIDEO_DECODER);

static void
gst_vp8_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_vp8_dec_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_vp8_dec_sink_template));

  gst_element_class_set_details_simple (element_class,
      "On2 VP8 Decoder",
      "Codec/Decoder/Video",
      "Decode VP8 video streams", "David Schleef <ds@entropywave.com>");
}

static void
gst_vp8_dec_class_init (GstVP8DecClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseVideoDecoderClass *base_video_decoder_class;

  gobject_class = G_OBJECT_CLASS (klass);
  element_class = GST_ELEMENT_CLASS (klass);
  base_video_decoder_class = GST_BASE_VIDEO_DECODER_CLASS (klass);

  gobject_class->set_property = gst_vp8_dec_set_property;
  gobject_class->get_property = gst_vp8_dec_get_property;
  gobject_class->finalize = gst_vp8_dec_finalize;

  base_video_decoder_class->start = gst_vp8_dec_start;
  base_video_decoder_class->stop = gst_vp8_dec_stop;
  base_video_decoder_class->reset = gst_vp8_dec_reset;
  base_video_decoder_class->parse_data = gst_vp8_dec_parse_data;
  base_video_decoder_class->handle_frame = gst_vp8_dec_handle_frame;
}

static void
gst_vp8_dec_init (GstVP8Dec * gst_vp8_dec, GstVP8DecClass * klass)
{
  GST_DEBUG_OBJECT (gst_vp8_dec, "gst_vp8_dec_init");
}

static void
gst_vp8_dec_finalize (GObject * object)
{
  GstVP8Dec *gst_vp8_dec;

  GST_DEBUG_OBJECT (object, "finalize");

  g_return_if_fail (GST_IS_GST_VP8_DEC (object));
  gst_vp8_dec = GST_VP8_DEC (object);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vp8_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVP8Dec *src;

  g_return_if_fail (GST_IS_GST_VP8_DEC (object));
  src = GST_VP8_DEC (object);

  GST_DEBUG_OBJECT (object, "gst_vp8_dec_set_property");
  switch (prop_id) {
    default:
      break;
  }
}

static void
gst_vp8_dec_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstVP8Dec *src;

  g_return_if_fail (GST_IS_GST_VP8_DEC (object));
  src = GST_VP8_DEC (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


static gboolean
gst_vp8_dec_start (GstBaseVideoDecoder * decoder)
{
  GstVP8Dec *gst_vp8_dec = GST_VP8_DEC (decoder);

  GST_DEBUG_OBJECT (gst_vp8_dec, "start");
  decoder->packetized = TRUE;
  gst_vp8_dec->decoder_inited = FALSE;

  return TRUE;
}

static gboolean
gst_vp8_dec_stop (GstBaseVideoDecoder * base_video_decoder)
{
  GstVP8Dec *gst_vp8_dec = GST_VP8_DEC (base_video_decoder);

  GST_DEBUG_OBJECT (gst_vp8_dec, "stop");
  if (gst_vp8_dec->decoder_inited)
    vpx_codec_destroy (&gst_vp8_dec->decoder);
  gst_vp8_dec->decoder_inited = FALSE;
  return TRUE;
}

static gboolean
gst_vp8_dec_reset (GstBaseVideoDecoder * base_video_decoder)
{
  GstVP8Dec *decoder;

  GST_DEBUG_OBJECT (base_video_decoder, "reset");

  decoder = GST_VP8_DEC (base_video_decoder);

  if (decoder->decoder_inited)
    vpx_codec_destroy (&decoder->decoder);
  decoder->decoder_inited = FALSE;

  return TRUE;
}

static GstFlowReturn
gst_vp8_dec_parse_data (GstBaseVideoDecoder * decoder, gboolean at_eos)
{
  return GST_FLOW_OK;
}

static void
gst_vp8_dec_send_tags (GstVP8Dec * dec)
{
  GstTagList *list;

  list = gst_tag_list_new ();
  gst_tag_list_add (list, GST_TAG_MERGE_REPLACE,
      GST_TAG_VIDEO_CODEC, "VP8 video", NULL);

  gst_element_found_tags_for_pad (GST_ELEMENT (dec),
      GST_BASE_VIDEO_CODEC_SRC_PAD (dec), list);
}

static void
gst_vp8_dec_image_to_buffer (GstVP8Dec * dec, const vpx_image_t * img,
    GstBuffer * buffer)
{
  GstBaseVideoDecoder *decoder = (GstBaseVideoDecoder *) dec;
  int stride, w, h, i;
  guint8 *d;

  d = GST_BUFFER_DATA (buffer) +
      gst_video_format_get_component_offset (decoder->state.format, 0,
      decoder->state.width, decoder->state.height);
  stride =
      gst_video_format_get_row_stride (decoder->state.format, 0,
      decoder->state.width);
  h = gst_video_format_get_component_height (decoder->state.format, 0,
      decoder->state.height);
  h = MIN (h, img->h);
  w = gst_video_format_get_component_width (decoder->state.format, 0,
      decoder->state.width);
  w = MIN (w, img->w);

  for (i = 0; i < h; i++)
    memcpy (d + i * stride, img->planes[PLANE_Y] + i * img->stride[PLANE_Y], w);

  d = GST_BUFFER_DATA (buffer) +
      gst_video_format_get_component_offset (decoder->state.format, 1,
      decoder->state.width, decoder->state.height);
  stride =
      gst_video_format_get_row_stride (decoder->state.format, 1,
      decoder->state.width);
  h = gst_video_format_get_component_height (decoder->state.format, 1,
      decoder->state.height);
  h = MIN (h, img->h >> img->y_chroma_shift);
  w = gst_video_format_get_component_width (decoder->state.format, 1,
      decoder->state.width);
  w = MIN (w, img->w >> img->x_chroma_shift);
  for (i = 0; i < h; i++)
    memcpy (d + i * stride, img->planes[PLANE_U] + i * img->stride[PLANE_U], w);

  d = GST_BUFFER_DATA (buffer) +
      gst_video_format_get_component_offset (decoder->state.format, 2,
      decoder->state.width, decoder->state.height);
  /* Same stride, height, width as above */
  for (i = 0; i < h; i++)
    memcpy (d + i * stride, img->planes[PLANE_V] + i * img->stride[PLANE_V], w);
}

static GstFlowReturn
gst_vp8_dec_handle_frame (GstBaseVideoDecoder * decoder, GstVideoFrame * frame)
{
  GstVP8Dec *dec;
  GstFlowReturn ret;
  vpx_codec_err_t status;
  vpx_codec_iter_t iter = NULL;
  vpx_image_t *img;

  GST_DEBUG_OBJECT (decoder, "handle_frame");

  dec = GST_VP8_DEC (decoder);

  if (!dec->decoder_inited) {
    int flags = 0;
    vpx_codec_stream_info_t stream_info;

    memset (&stream_info, 0, sizeof (stream_info));
    stream_info.sz = sizeof (stream_info);

    status = vpx_codec_peek_stream_info (&vpx_codec_vp8_dx_algo,
        GST_BUFFER_DATA (frame->sink_buffer),
        GST_BUFFER_SIZE (frame->sink_buffer), &stream_info);

    if (status != VPX_CODEC_OK || !stream_info.is_kf) {
      GST_WARNING_OBJECT (decoder, "No keyframe, skipping");
      gst_base_video_decoder_skip_frame (decoder, frame);
      return GST_FLOW_OK;
    }

    /* should set size here */
    decoder->state.width = stream_info.w;
    decoder->state.height = stream_info.h;
    decoder->state.format = GST_VIDEO_FORMAT_I420;
    gst_vp8_dec_send_tags (dec);

    status =
        vpx_codec_dec_init (&dec->decoder, &vpx_codec_vp8_dx_algo, NULL, flags);
    if (status != VPX_CODEC_OK) {
      GST_ELEMENT_ERROR (dec, LIBRARY, INIT,
          ("Failed to initialize VP8 decoder"), ("%s",
              gst_vpx_error_name (status)));
      return GST_FLOW_ERROR;
    }
    dec->decoder_inited = TRUE;
  }

  if (!GST_BUFFER_FLAG_IS_SET (frame->sink_buffer, GST_BUFFER_FLAG_DELTA_UNIT))
    gst_base_video_decoder_set_sync_point (decoder);

#if 0
  if (GST_PAD_CAPS (GST_BASE_VIDEO_CODEC_SRC_PAD (decoder)) == NULL) {
    GstCaps *caps;

    caps = gst_video_format_new_caps (decoder->state.format,
        decoder->state.width, decoder->state.height,
        decoder->state.fps_n, decoder->state.fps_d,
        decoder->state.par_n, decoder->state.par_d);

    GST_DEBUG ("setting caps %" GST_PTR_FORMAT, caps);

    gst_pad_set_caps (GST_BASE_VIDEO_CODEC_SRC_PAD (decoder), caps);
  }
#endif

  ret = gst_base_video_decoder_alloc_src_frame (decoder, frame);
  if (ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (decoder, "failed to get buffer: %s",
        gst_flow_get_name (ret));
    goto out;
  }

  status = vpx_codec_decode (&dec->decoder,
      GST_BUFFER_DATA (frame->sink_buffer),
      GST_BUFFER_SIZE (frame->sink_buffer), NULL, 0);
  if (status) {
    GST_ELEMENT_ERROR (decoder, LIBRARY, ENCODE,
        ("Failed to decode frame"), ("%s", gst_vpx_error_name (status)));
    return GST_FLOW_ERROR;
  }

  img = vpx_codec_get_frame (&dec->decoder, &iter);
  if (img) {

    ret = gst_base_video_decoder_alloc_src_frame (decoder, frame);

    if (ret == GST_FLOW_OK)
      gst_vp8_dec_image_to_buffer (dec, img, frame->src_buffer);

    gst_base_video_decoder_finish_frame (decoder, frame);

    vpx_img_free (img);

    while ((img = vpx_codec_get_frame (&dec->decoder, &iter))) {
      GST_WARNING_OBJECT (decoder, "Multiple decoded frames... dropping");
      vpx_img_free (img);
    }
  } else {
    /* Invisible frame */
    gst_base_video_decoder_skip_frame (decoder, frame);
  }
out:

  return ret;
}