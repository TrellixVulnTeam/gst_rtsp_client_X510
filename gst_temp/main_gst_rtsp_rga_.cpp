//
// Created by steve 21-09-16
//
//  gstreamer如何接入RTSP流（IP摄像头） h264/h265
//  uridecodebin videoconvert autovideosink 

#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gstpipeline.h>
#include <gst/gstcaps.h>

#include <gst/video/gstvideometa.h>
#include <gst/base/gstbasesink.h>
#include <gst/video/video-info.h>
#include <gst/video/video-format.h>
#include <gst/video/video-enumtypes.h>
#include <gst/video/video-tile.h>
#include <gst/base/base-prelude.h>
#include <gst/gstquery.h>

// extern "C" {
//   #include <rockchip/rockchip_rga.h>
// }
// #include <rga/rga.h>
// #include <rga/RgaUtils.h>
// #include <rga/RockchipRga.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core/mat.hpp>
#include <stdio.h>
#include <unistd.h>

// gst log
GST_DEBUG_CATEGORY_STATIC (rk_appsink_debug);
#define GST_CAT_DEFAULT rk_appsink_debug

#define PNAME   "index0"
#define RTSPCAM "rtsp://admin:shangqu2020@192.168.2.30:554/cam/realmonitor?channel=1&subtype=0"
#define DISPLAY FALSE

//video
#define VIDEO_FORMAT "BGR"
#define VIDEO_HEIGHT 1080
#define VIDEO_WIDTH 1920

//rga
#define BUFFER_WIDTH 1920
#define BUFFER_HEIGHT 1080
#define BUFFER_SIZE BUFFER_WIDTH*BUFFER_HEIGHT*3

inline static const char *
yesno (int yes)
{
  return yes ? "yes" : "no";
}

// CustomData
struct CustomData {
    GMainLoop *loop;
    GstElement *pipeline;
    GstElement *source;
    GstElement *tee;
    GstElement *queue_appsink;
    GstElement *queue_displaysink;
    GstElement *appsink;
    GstElement *displaysink;
    GstElement *videoconvert;

    pthread_t gst_thread;

    gint format;
    GstVideoInfo info;

    unsigned frame;
};

// appsink probe
static GstPadProbeReturn
pad_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  struct CustomData *dec = (struct CustomData *) user_data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstCaps *caps;

  (void) pad;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  gst_event_parse_caps (event, &caps);

  if (!caps) {
    GST_ERROR ("caps event without caps");
    return GST_PAD_PROBE_OK;
  }

  if (!gst_video_info_from_caps (&dec->info, caps)) {
    GST_ERROR ("caps event with invalid video caps");
    return GST_PAD_PROBE_OK;
  }

  switch (GST_VIDEO_INFO_FORMAT (&(dec->info))) {
    case GST_VIDEO_FORMAT_I420:
      dec->format = 2;
      break;
    case GST_VIDEO_FORMAT_NV12:
      dec->format = 23;
      break;
    case GST_VIDEO_FORMAT_YUY2:
      dec->format = 4;
      break;
    default:
      GST_ERROR ("unknown format\n");
      return GST_PAD_PROBE_OK;
  }

  return GST_PAD_PROBE_OK;
}

// uridecodebin -> src link tee -> sink 
static void 
on_src_tee_added(GstElement *element, GstPad *pad, gpointer data) {
    GstPad *sinkpad;
    struct CustomData *decoder = (struct CustomData *)data;
    /* We can now link this pad with the rtsp-decoder sink pad */
    g_print("Dynamic pad created, linking source/demuxer\n");
    sinkpad = gst_element_get_static_pad(decoder->tee, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

// bus
static gboolean
bus_watch_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  struct CustomData *dec = (struct CustomData *) user_data;

  (void) bus;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED:{
      gchar *dotfilename;
      GstState old_gst_state, cur_gst_state, pending_gst_state;

      /* Only consider state change messages coming from
       * the toplevel element. */
      if (GST_MESSAGE_SRC (msg) != GST_OBJECT (dec->pipeline))
        break;

      gst_message_parse_state_changed (msg, &old_gst_state, &cur_gst_state,
          &pending_gst_state);

      printf ("GStreamer state change:  old: %s  current: %s  pending: %s\n",
          gst_element_state_get_name (old_gst_state),
          gst_element_state_get_name (cur_gst_state),
          gst_element_state_get_name (pending_gst_state)
          );

      dotfilename = g_strdup_printf ("statechange__old-%s__cur-%s__pending-%s",
          gst_element_state_get_name (old_gst_state),
          gst_element_state_get_name (cur_gst_state),
          gst_element_state_get_name (pending_gst_state)
          );
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (dec->pipeline),
          GST_DEBUG_GRAPH_SHOW_ALL, dotfilename);
      g_free (dotfilename);

      break;
    }
    case GST_MESSAGE_REQUEST_STATE:{
      GstState requested_state;
      gst_message_parse_request_state (msg, &requested_state);
      printf ("state change to %s was requested by %s\n",
          gst_element_state_get_name (requested_state),
          GST_MESSAGE_SRC_NAME (msg)
          );
      gst_element_set_state (GST_ELEMENT (dec->pipeline), requested_state);
      break;
    }
    case GST_MESSAGE_LATENCY:{
      printf ("redistributing latency\n");
      gst_bin_recalculate_latency (GST_BIN (dec->pipeline));
      break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit (dec->loop);
      break;
    case GST_MESSAGE_INFO:
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR:{
      GError *error = NULL;
      gchar *debug_info = NULL;
      gchar const *prefix = "";

      switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_INFO:
          gst_message_parse_info (msg, &error, &debug_info);
          prefix = "INFO";
          break;
        case GST_MESSAGE_WARNING:
          gst_message_parse_warning (msg, &error, &debug_info);
          prefix = "WARNING";
          break;
        case GST_MESSAGE_ERROR:
          gst_message_parse_error (msg, &error, &debug_info);
          prefix = "ERROR";
          break;
        default:
          g_assert_not_reached ();
      }
      printf ("GStreamer %s: %s; debug info: %s", prefix, error->message,
          debug_info);

      g_clear_error (&error);
      g_free (debug_info);

      if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (dec->pipeline),
            GST_DEBUG_GRAPH_SHOW_ALL, "error");
      }
      // TODO: stop mainloop in case of an error

      break;
    }
    default:
      break;
  }

  return TRUE;
}

// appsink query
static GstPadProbeReturn
appsink_query_cb (GstPad * pad G_GNUC_UNUSED, GstPadProbeInfo * info,
    gpointer user_data G_GNUC_UNUSED)
{
  GstQuery *query = (GstQuery *) info->data;

  if (GST_QUERY_TYPE (query) != GST_QUERY_ALLOCATION)
    return GST_PAD_PROBE_OK;

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  return GST_PAD_PROBE_HANDLED;
}

// static int checkData(unsigned char *srcBuffer, unsigned char *dstBuffer) {
//     int num = 0;
//     for (int i = 0; i < BUFFER_SIZE; ++i) {
//         // if (srcBuffer[i] != dstBuffer[i]) {
//         //     printf("[%d] src=%u, dst=%u\n", i, srcBuffer[i], dstBuffer[i]);
//         // }
//         num = i;
//     }
//     return num;
// }

static void *
buffer_to_file (struct CustomData *dec, GstBuffer * buf)
{
  int ret;
  GstVideoMeta *meta = gst_buffer_get_video_meta (buf);
  guint nplanes = GST_VIDEO_INFO_N_PLANES (&(dec->info));
  guint width, height;
  GstMapInfo map_info;
  gchar filename[128];
  GstVideoFormat pixfmt;
  const char *pixfmt_str;

  pixfmt = GST_VIDEO_INFO_FORMAT (&(dec->info));
  pixfmt_str = gst_video_format_to_string (pixfmt);

  /* TODO: use the DMABUF directly */

  gst_buffer_map (buf, &map_info, GST_MAP_READ);

  width = GST_VIDEO_INFO_WIDTH (&(dec->info));
  height = GST_VIDEO_INFO_HEIGHT (&(dec->info));

  /* output some information at the beginning (= when the first frame is handled) */
  if (dec->frame == 0) {
    printf ("===================================\n");
    printf ("GStreamer video stream information:\n");
    printf ("  size: %u x %u pixel\n", width, height);
    printf ("  pixel format: %s  number of planes: %u\n", pixfmt_str, nplanes);
    printf ("  video meta found: %s\n", yesno (meta != NULL));
    printf ("===================================\n");
  }

  g_print( "*");

  // int resize_w = 1920, resize_h = 1080;
  // // static int frame_size = 0;
  // // unsigned char *frame_rgb = NULL;
  // // frame_size = resize_w * resize_h * 3;
  // // frame_rgb = (unsigned char *)malloc(frame_size);
  // cv::Mat img(resize_h , resize_w , CV_8UC3, (char *)map_info.data);
  // // if (!frame_rgb)
  // //   return 0;

  // cv::imwrite("test.jpg", img);
  // cv::waitKey(10);

  // g_print("num: %d \n", checkData(srcBuffer,dstBuffer));

  // cv::Mat img(BUFFER_HEIGHT, BUFFER_WIDTH , CV_8UC3, dstBuffer);
  // cv::imwrite("test.jpg", img); 
  // //unchar_to_Mat(dstBuffer); 
  // g_print( "*Rga ret = %d \n",ret);
  // if (!ret) {
	// 		///do something with frame_rgb
	// 		cv::imwrite("test.jpg", img);
	// 		cv::waitKey(10);
	// }

  //unchar_to_Mat(dstBuffer);  // 将 unsigned char BGR格式转换为 Mat BGR格式
  //cv::imwrite
  //cv::imshow("image",img);
  //cv::waitKey(20);

  // g_snprintf (filename, sizeof (filename), "img%05d.%s", dec->frame,
  //     pixfmt_str);
  // g_file_set_contents (filename, (char *) map_info.data, map_info.size, NULL);
  

  gst_buffer_unmap (buf, &map_info);

  return 0;
}

static void *
video_frame_loop (void *arg)
{
  struct CustomData *dec = (struct CustomData *) arg;
  do {
    GstSample *samp;
    GstBuffer *buf;

    samp = gst_app_sink_pull_sample (GST_APP_SINK (dec->appsink));
    if (!samp) {
      GST_DEBUG ("got no appsink sample");
      if (gst_app_sink_is_eos (GST_APP_SINK (dec->appsink)))
        GST_DEBUG ("eos");
      return NULL;
    }

    buf = gst_sample_get_buffer (samp);
    buffer_to_file (dec, buf);

    gst_sample_unref (samp);
    dec->frame++;
    

  } while (1);

}

// rtsp init
static struct CustomData *
rtsp_init(const char *pname, const char *rtsp_uri) {

    struct CustomData *data;
    data = g_new0 (struct CustomData, 1);

    GstBus *bus;
    GstPad *apppad;
    GstPad *queue1_video_pad;
    GstPad *queue2_video_pad;
    GstPad *tee1_video_pad;
    GstPad *tee2_video_pad;

    GstCaps *new_src_caps;

    /* Build Pipeline */
    data->pipeline = gst_pipeline_new(pname);

    data->source = gst_element_factory_make ( "uridecodebin", "source");
    data->tee    = gst_element_factory_make ( "tee", "tee");

    if (DISPLAY) {
        data->queue_displaysink  = gst_element_factory_make ( "queue", "queue_displaysink");
        data->displaysink      = gst_element_factory_make ( "rkximagesink", "display_sink");
    }
    data->queue_appsink      = gst_element_factory_make ( "queue", "queue_appsink");
    data->appsink            = gst_element_factory_make ( "appsink", "app_sink");
    data->videoconvert       = gst_element_factory_make ( "videoconvert", "videoconvert");

    if (!DISPLAY) {
        if (!data->pipeline || !data->source || !data->tee || !data->queue_appsink || !data->appsink || !data->videoconvert) {
            g_printerr("One element could not be created.\n");
        }
    }else{
        if (!data->pipeline || !data->source || !data->tee || !data->queue_appsink || !data->queue_displaysink  || !data->displaysink || !data->appsink || !data->videoconvert ) {
            g_printerr("One element could not be created.\n");
        }
        // Configure rksink
        g_object_set (G_OBJECT (data->displaysink), "sync", FALSE, NULL);
    }

    // Config appsink
    g_object_set (G_OBJECT (data->appsink), "sync", FALSE, NULL);
    /* Implement the allocation query using a pad probe. This probe will
    * adverstize support for GstVideoMeta, which avoid hardware accelerated
    * decoder that produce special strides and offsets from having to
    * copy the buffers.
    */
    apppad = gst_element_get_static_pad (data->appsink, "sink");
    gst_pad_add_probe (apppad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        appsink_query_cb, NULL, NULL);
    gst_object_unref (apppad);

    gst_base_sink_set_max_lateness (GST_BASE_SINK (data->appsink), 70 * GST_MSECOND);
    gst_base_sink_set_qos_enabled (GST_BASE_SINK (data->appsink), TRUE);

    g_object_set (G_OBJECT (data->appsink), "max-buffers", 2, NULL);

    gst_pad_add_probe (gst_element_get_static_pad (data->appsink, "sink"),
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, pad_probe, data, NULL);
    //g_object_set (G_OBJECT (data->appsink), "emit-signals", TRUE, NULL );
    //g_signal_connect ( G_OBJECT (data->appsink), "new-sample", G_CALLBACK (new_sample), pipeline);

    //create_uri(url,url_size, ip_address, port);
    g_object_set(GST_OBJECT(data->source), "uri", RTSPCAM, NULL);
    //"rtsp://<ip>:554/live/ch00_0"

    //videoconvert caps
    new_src_caps = gst_caps_new_simple("video/x-raw",
                                                      "framerate", GST_TYPE_FRACTION, 25, 1,
                                                      "format", G_TYPE_STRING, VIDEO_FORMAT,
                                                      "width", G_TYPE_INT, VIDEO_WIDTH,
                                                      "height", G_TYPE_INT, VIDEO_HEIGHT,
                                                      NULL);

    if (DISPLAY) {
        gst_bin_add_many(GST_BIN(data->pipeline), data->queue_displaysink, data->displaysink, NULL);
        // queue -> rkximagesink
        if (!gst_element_link_many (data->queue_displaysink, data->displaysink, NULL)) {
            g_printerr ("Elements could not be linked.\n");
            gst_object_unref (data->pipeline);
            return NULL;
        }
    }
    gst_bin_add_many(GST_BIN(data->pipeline), data->source, data->tee, NULL);
    gst_bin_add_many(GST_BIN(data->pipeline), data->queue_appsink, data->appsink, NULL);
    // gst_bin_add_many(GST_BIN(data->pipeline), data->queue_appsink, data->videoconvert, data->appsink, NULL);

    // queue -> rkximagesink  ||  queue -> appsink
    if (!gst_element_link_many (data->queue_appsink, data->appsink, NULL)) {
        g_printerr ("Elements could not be linked.\n");
        gst_object_unref (data->pipeline);
        return NULL;
    }

    // // queue -> rkximagesink  ||  queue -> appsink
    // if (!gst_element_link_many (data->queue_appsink, data->videoconvert, NULL)) {
    //     g_printerr ("Elements could not be linked.\n");
    //     gst_object_unref (data->pipeline);
    //     return NULL;
    // }

    // // link videoconvert appsink
    // if (gst_element_link_filtered(data->videoconvert, data->appsink, new_src_caps)==FALSE){
		// 	g_print("fail videoconvert link appsink and argbdec 0\n");
		// }
    gst_caps_unref((GstCaps *) new_src_caps);


    if (DISPLAY) {
        queue1_video_pad = gst_element_get_static_pad ( data->queue_displaysink, "sink");
        tee1_video_pad = gst_element_get_request_pad ( data->tee, "src_%u");
        if (gst_pad_link ( tee1_video_pad, queue1_video_pad) != GST_PAD_LINK_OK) {
            g_printerr ("tee link queue error. \n");
            gst_object_unref (data->pipeline);
            return NULL;
        }
        gst_object_unref (queue1_video_pad);
        gst_object_unref (tee1_video_pad);
    }
    //tee -> queue1 -> queue2
    queue2_video_pad = gst_element_get_static_pad ( data->queue_appsink, "sink");
    tee2_video_pad = gst_element_get_request_pad ( data->tee, "src_%u");
    if (gst_pad_link ( tee2_video_pad, queue2_video_pad) != GST_PAD_LINK_OK) {
        g_printerr ("tee link queue error. \n");
        gst_object_unref (data->pipeline);
        return NULL;
    }
    gst_object_unref (queue2_video_pad);
    gst_object_unref (tee2_video_pad);

    g_signal_connect(data->source, "pad-added", G_CALLBACK(on_src_tee_added), data);

    bus = gst_pipeline_get_bus( GST_PIPELINE (data->pipeline));
    gst_bus_add_watch (bus, bus_watch_cb, data);
    gst_object_unref ( GST_OBJECT (bus));

    // start playing
    g_print ("start playing \n");
    gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    data->loop = g_main_loop_new (NULL, FALSE);
    //g_main_loop_run (main_loop);

    pthread_create (&data->gst_thread, NULL, video_frame_loop, data);

    return data;
}

// destroy
static void
rtsp_destroy (struct CustomData *data)
{
    gst_element_set_state (data->pipeline, GST_STATE_NULL);
    pthread_join (data->gst_thread, 0);

    //gst_element_release_request_pad (data->tee,data->tee1_video_pad);
    //gst_element_release_request_pad (data->tee,data->tee2_video_pad);
    //gst_object_unref (data->tee1_video_pad);
    //gst_object_unref (data->tee2_video_pad);
    gst_object_unref (data->tee);
    gst_object_unref (data->source);
    if (DISPLAY) {
        gst_object_unref (data->queue_displaysink);
        gst_object_unref (data->displaysink);
    }
    gst_object_unref (data->queue_appsink);
    gst_object_unref (data->appsink);
    gst_object_unref (data->videoconvert);

    gst_object_unref (data->pipeline);
    g_main_loop_quit (data->loop);
    g_main_loop_unref (data->loop);

    g_free (data);
}

// main
int main(int argc, char *argv[]) {

    // public customData
    struct CustomData *data = NULL;

    gst_init (&argc, &argv);
    GST_DEBUG_CATEGORY_INIT (rk_appsink_debug, "rk_appsink", 2, "App sink");

    data = rtsp_init (PNAME, RTSPCAM);

    g_main_loop_run (data->loop);

    rtsp_destroy (data);

}