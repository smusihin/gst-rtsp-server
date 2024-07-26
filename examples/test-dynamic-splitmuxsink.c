#include <gst/gst.h>

#include <gst/rtsp-server/rtsp-server.h>

#include <time.h>


#define DEFAULT_RTSP_PORT "8554"
#define DEFAULT_DISABLE_RTCP FALSE

static char *port = (char *) DEFAULT_RTSP_PORT;
static gboolean disable_rtcp = DEFAULT_DISABLE_RTCP;

static GOptionEntry entries[] = {
  {"port", 'p', 0, G_OPTION_ARG_STRING, &port,
      "Port to listen on (default: " DEFAULT_RTSP_PORT ")", "PORT"},
  {"disable-rtcp", '\0', 0, G_OPTION_ARG_NONE, &disable_rtcp,
      "Whether RTCP should be disabled (default false)", NULL},
  {NULL}
};

typedef struct
{
  GstElement *pipeline;
  GstElement *tee;
  GstPad *teepad;

  GstElement *record_pipeline;
  GstElement *splitmuxsink;

  gboolean record_started;
  gboolean removing;

  char *record_pipeline_description;
} Context;

static gchararray
format_location_callback (GstElement * splitmux,
    guint fragment_id, gpointer udata)
{
  char buff[20];
  time_t now = time (NULL);
  strftime (buff, 20, "%Y-%m-%d %H:%M:%S", localtime (&now));

  return g_strdup_printf ("%s/%s.mkv", "/home/serge", buff);
}

static GstPadProbeReturn
unlink_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstPad *sinkpad;

  Context *ctx = user_data;

  if (!g_atomic_int_compare_and_exchange (&ctx->removing, FALSE, TRUE))
    return GST_PAD_PROBE_OK;

  sinkpad = gst_element_get_static_pad (ctx->record_pipeline, "sink");
  gst_pad_unlink (ctx->teepad, sinkpad);
  gst_object_unref (sinkpad);

  gst_bin_remove (GST_BIN (ctx->pipeline), ctx->record_pipeline);
  gst_bin_remove (GST_BIN (ctx->pipeline), ctx->splitmuxsink);

  gst_element_set_state (ctx->splitmuxsink, GST_STATE_NULL);
  gst_element_set_state (ctx->record_pipeline, GST_STATE_NULL);

  gst_object_unref (ctx->record_pipeline);
  gst_object_unref (ctx->splitmuxsink);

  gst_element_release_request_pad (ctx->tee, ctx->teepad);
  gst_object_unref (ctx->teepad);

  g_print ("Record stopped\n");

  ctx->record_started = FALSE;

  return GST_PAD_PROBE_REMOVE;
}


static gboolean
stop_record (Context * ctx)
{
  g_print ("stop\n");
  gst_pad_add_probe (ctx->teepad, GST_PAD_PROBE_TYPE_IDLE, unlink_cb, ctx,
      NULL);
  return TRUE;
}

static gboolean
start_record (Context * ctx)
{
  GstPad *sinkpad;
  GstPadTemplate *templ;

  GError *error = NULL;

  g_print ("Start record\n");

  templ =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (ctx->tee),
      "src_%u");

  ctx->teepad = gst_element_request_pad (ctx->tee, templ, NULL, NULL);
  ctx->record_pipeline =
      gst_parse_bin_from_description (ctx->record_pipeline_description, TRUE,
      &error);

  if (error != NULL) {
    g_print ("%s", error->message);
    return FALSE;
  }

  ctx->splitmuxsink = gst_element_factory_make ("splitmuxsink", NULL);
  ctx->removing = FALSE;

  g_object_set (ctx->splitmuxsink, "muxer-factory", "matroskamux", NULL);
  g_object_set (ctx->splitmuxsink, "async-finalize", TRUE, NULL);
  g_object_set (ctx->splitmuxsink, "max-size-time", 10000000000, NULL);

  g_signal_connect (ctx->splitmuxsink, "format_location",
      (GCallback) format_location_callback, NULL);


  gst_bin_add_many (GST_BIN (ctx->pipeline),
      gst_object_ref (ctx->record_pipeline),
      gst_object_ref (ctx->splitmuxsink), NULL);

  gst_element_link_many (ctx->record_pipeline, ctx->splitmuxsink, NULL);

  gst_element_sync_state_with_parent (ctx->record_pipeline);
  gst_element_sync_state_with_parent (ctx->splitmuxsink);

  sinkpad = gst_element_get_static_pad (ctx->record_pipeline, "sink");
  gst_pad_link (ctx->teepad, sinkpad);
  gst_object_unref (sinkpad);

  ctx->record_started = TRUE;

  return TRUE;
}

static gboolean
toggle_record (gpointer data)
{
  Context *ctx = data;

  if (ctx->record_started) {
    return stop_record (ctx);
  }
  return start_record (ctx);
}

static void
media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media,
    gpointer user_data)
{
  Context *ctx = user_data;

  ctx->pipeline = gst_rtsp_media_get_element (media);
  ctx->tee =
      gst_bin_get_by_name_recurse_up (GST_BIN (ctx->pipeline), "origin_tee");
  ctx->record_started = FALSE;

  g_timeout_add_seconds (3, toggle_record, ctx);
}



int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GstRTSPServer *server;
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactory *factory;
  GOptionContext *optctx;
  GError *error = NULL;

  Context ctx;

  optctx =
      g_option_context_new
      ("<launch line> - Test RTSP Server with record pipeline, Launch\n\n"
      "Example: \"( videotestsrc ! x264enc ! rtph264pay name=pay0 pt=96 )\" \"( queue ! videoconvert )\"");
  g_option_context_add_main_entries (optctx, entries, NULL);
  g_option_context_add_group (optctx, gst_init_get_option_group ());
  if (!g_option_context_parse (optctx, &argc, &argv, &error)) {
    g_printerr ("Error parsing options: %s\n", error->message);
    g_option_context_free (optctx);
    g_clear_error (&error);
    return -1;
  }
  g_option_context_free (optctx);

  ctx.record_pipeline_description = argv[2];

  loop = g_main_loop_new (NULL, FALSE);

  /* create a server instance */
  server = gst_rtsp_server_new ();
  g_object_set (server, "service", port, NULL);

  /* get the mount points for this server, every server has a default object
   * that be used to map uri mount points to media factories */
  mounts = gst_rtsp_server_get_mount_points (server);

  /* make a media factory for a test stream. The default media factory can use
   * gst-launch syntax to create pipelines.
   * any launch line works as long as it contains elements named pay%d. Each
   * element with pay%d names will be a stream */


  factory = gst_rtsp_media_factory_new ();
  gst_rtsp_media_factory_set_launch (factory, argv[1]);
  gst_rtsp_media_factory_set_shared (factory, TRUE);
  gst_rtsp_media_factory_set_enable_rtcp (factory, !disable_rtcp);

  /* notify when our media is ready, This is called whenever someone asks for
   * the media and a new pipeline with our appsrc is created */
  g_signal_connect (factory, "media-configure", (GCallback) media_configure,
      &ctx);

  /* attach the test factory to the /test url */
  gst_rtsp_mount_points_add_factory (mounts, "/test", factory);

  /* don't need the ref to the mapper anymore */
  g_object_unref (mounts);

  /* attach the server to the default maincontext */
  gst_rtsp_server_attach (server, NULL);

  /* start serving */
  g_print ("stream ready at rtsp://127.0.0.1:%s/test\n", port);
  g_main_loop_run (loop);

  return 0;
}
