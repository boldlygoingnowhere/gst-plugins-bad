/* GStreamer
 * Copyright (C) 2012 Fluendo S.A. <support@fluendo.com>
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
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "openslesringbuffer.h"

GST_DEBUG_CATEGORY_STATIC (opensles_ringbuffer_debug);
#define GST_CAT_DEFAULT opensles_ringbuffer_debug

static GstRingBufferClass *ring_parent_class = NULL;

static void
_do_init (GType type)
{
  GST_DEBUG_CATEGORY_INIT (opensles_ringbuffer_debug,
      "opensles_ringbuffer", 0, "OpenSL ES ringbuffer");
}

GST_BOILERPLATE_FULL (GstOpenSLESRingBuffer, gst_opensles_ringbuffer,
    GstRingBuffer, GST_TYPE_RING_BUFFER, _do_init);

#define PLAYER_QUEUE_SIZE 2
#define RECORDER_QUEUE_SIZE 2

/* Some generic helper functions */

static inline SLuint32
_opensles_sample_rate (guint rate)
{
  switch (rate) {
    case 8000:
      return SL_SAMPLINGRATE_8;
    case 11025:
      return SL_SAMPLINGRATE_11_025;
    case 12000:
      return SL_SAMPLINGRATE_12;
    case 16000:
      return SL_SAMPLINGRATE_16;
    case 22050:
      return SL_SAMPLINGRATE_22_05;
    case 24000:
      return SL_SAMPLINGRATE_24;
    case 32000:
      return SL_SAMPLINGRATE_32;
    case 44100:
      return SL_SAMPLINGRATE_44_1;
    case 48000:
      return SL_SAMPLINGRATE_48;
    case 64000:
      return SL_SAMPLINGRATE_64;
    case 88200:
      return SL_SAMPLINGRATE_88_2;
    case 96000:
      return SL_SAMPLINGRATE_96;
    case 192000:
      return SL_SAMPLINGRATE_192;
    default:
      return 0;
  }
}

static inline SLuint32
_opensles_channel_mask (GstRingBufferSpec * spec)
{
  /* FIXME: handle more than two channels */
  switch (spec->channels) {
    case 1:
      return (SL_SPEAKER_FRONT_CENTER);
    case 2:
      return (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
    default:
      return 0;
  }
}

static inline void
_opensles_format (GstRingBufferSpec * spec, SLDataFormat_PCM * format)
{
  format->formatType = SL_DATAFORMAT_PCM;
  format->numChannels = spec->channels;
  format->samplesPerSec = _opensles_sample_rate (spec->rate);
  format->bitsPerSample = spec->depth;
  format->containerSize = spec->width;
  format->channelMask = _opensles_channel_mask (spec);
  format->endianness =
      (spec->bigend ? SL_BYTEORDER_BIGENDIAN : SL_BYTEORDER_LITTLEENDIAN);
}

static void
_opensles_enqueue_cb (SLAndroidSimpleBufferQueueItf bufferQueue, void *context)
{
  GstRingBuffer *rb = GST_RING_BUFFER_CAST (context);
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;
  guint8 *ptr;
  gint seg;
  gint len;

  if (!gst_ring_buffer_prepare_read (rb, &seg, &ptr, &len)) {
    GST_WARNING_OBJECT (rb, "No segment available");
    return;
  }

  /* Enqueue a buffer */
  GST_LOG_OBJECT (thiz, "enqueue: %p size %d segment: %d", ptr, len, seg);
  result = (*thiz->bufferQueue)->Enqueue (thiz->bufferQueue, ptr, len);

  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "bufferQueue.Enqueue failed(0x%08x)",
        (guint32) result);
    return;
  }

  gst_ring_buffer_advance (rb, 1);
}

/* Recorder related functions */

static gboolean
_opensles_recorder_acquire (GstRingBuffer * rb, GstRingBufferSpec * spec)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;
  SLDataFormat_PCM format;

  /* Configure audio source */
  SLDataLocator_IODevice loc_dev = {
    SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
    SL_DEFAULTDEVICEID_AUDIOINPUT, NULL
  };
  SLDataSource audioSrc = { &loc_dev, NULL };

  /* Configure audio sink */
  SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, RECORDER_QUEUE_SIZE
  };
  SLDataSink audioSink = { &loc_bq, &format };

  const SLInterfaceID id[1] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
  const SLboolean req[1] = { SL_BOOLEAN_TRUE };

  /* Define the format in OpenSL ES terms */
  _opensles_format (spec, &format);

  /* Create audio recorder (requires the RECORD_AUDIO permission) */
  result = (*thiz->engineEngine)->CreateAudioRecorder (thiz->engineEngine,
      &thiz->recorderObject, &audioSrc, &audioSink, 1, id, req);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "engine.CreateAudioRecorder failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Realize the audio recorder */
  result =
      (*thiz->recorderObject)->Realize (thiz->recorderObject, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.Realize failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Get the record interface */
  result = (*thiz->recorderObject)->GetInterface (thiz->recorderObject,
      SL_IID_RECORD, &thiz->recorderRecord);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.GetInterface(Record) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Get the buffer queue interface */
  result =
      (*thiz->recorderObject)->GetInterface (thiz->recorderObject,
      SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &thiz->bufferQueue);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.GetInterface(BufferQueue) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Register callback on the buffer queue */
  result = (*thiz->bufferQueue)->RegisterCallback (thiz->bufferQueue,
      _opensles_enqueue_cb, rb);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "bufferQueue.RegisterCallback failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Define our ringbuffer in terms of number of buffers and buffer size. */
  spec->segsize = (spec->rate * spec->bytes_per_sample) >> 2;
  spec->segtotal = 16;

  return TRUE;

failed:
  return FALSE;
}

static gboolean
_opensles_recorder_start (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;
  gint i;

  /* in case already recording, stop recording and clear buffer queue */
  result =
      (*thiz->recorderRecord)->SetRecordState (thiz->recorderRecord,
      SL_RECORDSTATE_STOPPED);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.SetRecordState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }
  result = (*thiz->bufferQueue)->Clear (thiz->bufferQueue);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "bq.Clear failed(0x%08x)", (guint32) result);
    return FALSE;
  }

  /* Fill the queue by enqueing buffers */
  for (i = 0; i < RECORDER_QUEUE_SIZE; i++) {
    _opensles_enqueue_cb (NULL, rb);
  }

  /* start recording */
  result =
      (*thiz->recorderRecord)->SetRecordState (thiz->recorderRecord,
      SL_RECORDSTATE_RECORDING);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.SetRecordState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }
  return TRUE;
}

static gboolean
_opensles_recorder_stop (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;

  result =
      (*thiz->recorderRecord)->SetRecordState (thiz->recorderRecord,
      SL_RECORDSTATE_STOPPED);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "recorder.SetRecordState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }
  return TRUE;
}

/* Player related functions */

static gboolean
_opensles_player_change_volume (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  SLresult result;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  if (thiz->playerVolume) {
    gint millibel = (1.0 - thiz->volume) * -5000.0;
    result =
        (*thiz->playerVolume)->SetVolumeLevel (thiz->playerVolume, millibel);
    if (result != SL_RESULT_SUCCESS) {
      GST_ERROR_OBJECT (thiz, "player.SetVolumeLevel failed(0x%08x)",
          (guint32) result);
      return FALSE;
    }
    GST_DEBUG_OBJECT (thiz, "changed volume to %d", millibel);
  }

  return TRUE;
}

static gboolean
_opensles_player_change_mute (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  SLresult result;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  if (thiz->playerVolume) {
    result = (*thiz->playerVolume)->SetMute (thiz->playerVolume, thiz->mute);
    if (result != SL_RESULT_SUCCESS) {
      GST_ERROR_OBJECT (thiz, "player.SetMute failed(0x%08x)",
          (guint32) result);
      return FALSE;
    }
    GST_DEBUG_OBJECT (thiz, "changed mute to %d", thiz->mute);
  }

  return TRUE;
}

static gboolean
_opensles_player_acquire (GstRingBuffer * rb, GstRingBufferSpec * spec)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;
  SLDataFormat_PCM format;

  /* Configure audio source */
  SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, PLAYER_QUEUE_SIZE
  };
  SLDataSource audioSrc = { &loc_bufq, &format };

  /* Configure audio sink */
  SLDataLocator_OutputMix loc_outmix = {
    SL_DATALOCATOR_OUTPUTMIX, thiz->outputMixObject
  };
  SLDataSink audioSink = { &loc_outmix, NULL };

  /* Create an audio player */
  const SLInterfaceID ids[2] = { SL_IID_BUFFERQUEUE, SL_IID_VOLUME };
  const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

  /* Define the format in OpenSL ES terms */
  _opensles_format (spec, &format);

  result = (*thiz->engineEngine)->CreateAudioPlayer (thiz->engineEngine,
      &thiz->playerObject, &audioSrc, &audioSink, 2, ids, req);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "engine.CreateAudioPlayer failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Realize the player */
  result =
      (*thiz->playerObject)->Realize (thiz->playerObject, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.Realize failed(0x%08x)", (guint32) result);
    goto failed;
  }

  /* Get the play interface */
  result = (*thiz->playerObject)->GetInterface (thiz->playerObject,
      SL_IID_PLAY, &thiz->playerPlay);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.GetInterface(Play) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Get the buffer queue interface */
  result = (*thiz->playerObject)->GetInterface (thiz->playerObject,
      SL_IID_BUFFERQUEUE, &thiz->bufferQueue);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.GetInterface(BufferQueue) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Register callback on the buffer queue */
  result = (*thiz->bufferQueue)->RegisterCallback (thiz->bufferQueue,
      _opensles_enqueue_cb, rb);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "bufferQueue.RegisterCallback failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Get the volume interface */
  result = (*thiz->playerObject)->GetInterface (thiz->playerObject,
      SL_IID_VOLUME, &thiz->playerVolume);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.GetInterface(Volume) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  /* Configure the volume and mute state */
  _opensles_player_change_volume (rb);
  _opensles_player_change_mute (rb);

  /* Define our ringbuffer in terms of number of buffers and buffer size. */
  spec->segsize = (spec->rate >> 4) * spec->bytes_per_sample;
  spec->segtotal = 2 << 4;

  return TRUE;

failed:
  return FALSE;
}

static gboolean
_opensles_player_start (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;
  gint i;

  /* Fill the queue by enqueing buffers */
  for (i = 0; i < PLAYER_QUEUE_SIZE; i++) {
    _opensles_enqueue_cb (NULL, rb);
  }

  result =
      (*thiz->playerPlay)->SetPlayState (thiz->playerPlay,
      SL_PLAYSTATE_PLAYING);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.SetPlayState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }

  return TRUE;
}

static gboolean
_opensles_player_pause (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;

  result =
      (*thiz->playerPlay)->SetPlayState (thiz->playerPlay, SL_PLAYSTATE_PAUSED);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.SetPlayState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }
  return TRUE;
}

static gboolean
_opensles_player_stop (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  SLresult result;

  result =
      (*thiz->playerPlay)->SetPlayState (thiz->playerPlay,
      SL_PLAYSTATE_STOPPED);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "player.SetPlayState failed(0x%08x)",
        (guint32) result);
    return FALSE;
  }
  return TRUE;
}

/* OpenSL ES ring buffer wrapper */

GstRingBuffer *
gst_opensles_ringbuffer_new (RingBufferMode mode)
{
  GstOpenSLESRingBuffer *thiz;

  g_return_val_if_fail (mode > RB_MODE_NONE && mode < RB_MODE_LAST, NULL);

  thiz = g_object_new (GST_TYPE_OPENSLES_RING_BUFFER, NULL);

  if (thiz) {
    thiz->mode = mode;
    if (mode == RB_MODE_SRC) {
      thiz->acquire = _opensles_recorder_acquire;
      thiz->start = _opensles_recorder_start;
      thiz->pause = _opensles_recorder_stop;
      thiz->stop = _opensles_recorder_stop;
      thiz->change_volume = NULL;
    } else if (mode == RB_MODE_SINK_PCM) {
      thiz->acquire = _opensles_player_acquire;
      thiz->start = _opensles_player_start;
      thiz->pause = _opensles_player_pause;
      thiz->stop = _opensles_player_stop;
      thiz->change_volume = _opensles_player_change_volume;
    }
  }

  GST_DEBUG_OBJECT (thiz, "ringbuffer created");
  return GST_RING_BUFFER (thiz);
}

void
gst_opensles_ringbuffer_set_volume (GstRingBuffer * rb, gfloat volume)
{
  GstOpenSLESRingBuffer *thiz;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  thiz->volume = volume;

  if (thiz->change_volume) {
    thiz->change_volume (rb);
  }
}

void
gst_opensles_ringbuffer_set_mute (GstRingBuffer * rb, gboolean mute)
{
  GstOpenSLESRingBuffer *thiz;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  thiz->mute = mute;

  if (thiz->change_mute) {
    thiz->change_mute (rb);
  }
}

static gboolean
gst_opensles_ringbuffer_open_device (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  SLresult result;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  /* Create engine */
  result = slCreateEngine (&thiz->engineObject, 0, NULL, 0, NULL, NULL);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "slCreateEngine failed(0x%08x)", (guint32) result);
    goto failed;
  }

  /* Realize the engine */
  result = (*thiz->engineObject)->Realize (thiz->engineObject,
      SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "engine.Realize failed(0x%08x)", (guint32) result);
    goto failed;
  }

  /* Get the engine interface, which is needed in order to
   * create other objects */
  result = (*thiz->engineObject)->GetInterface (thiz->engineObject,
      SL_IID_ENGINE, &thiz->engineEngine);
  if (result != SL_RESULT_SUCCESS) {
    GST_ERROR_OBJECT (thiz, "engine.GetInterface(Engine) failed(0x%08x)",
        (guint32) result);
    goto failed;
  }

  if (thiz->mode == RB_MODE_SINK_PCM) {
    /* Create an output mixer */
    result = (*thiz->engineEngine)->CreateOutputMix (thiz->engineEngine,
        &thiz->outputMixObject, 0, NULL, NULL);
    if (result != SL_RESULT_SUCCESS) {
      GST_ERROR_OBJECT (thiz, "engine.CreateOutputMix failed(0x%08x)",
          (guint32) result);
      goto failed;
    }

    /* Realize the output mixer */
    result = (*thiz->outputMixObject)->Realize (thiz->outputMixObject,
        SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
      GST_ERROR_OBJECT (thiz, "outputMix.Realize failed(0x%08x)",
          (guint32) result);
      goto failed;
    }
  }

  GST_DEBUG_OBJECT (thiz, "device opened");
  return TRUE;

failed:
  return FALSE;
}

static gboolean
gst_opensles_ringbuffer_close_device (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  /* Destroy output mix object */
  if (thiz->outputMixObject) {
    (*thiz->outputMixObject)->Destroy (thiz->outputMixObject);
    thiz->outputMixObject = NULL;
  }

  /* Destroy engine object, and invalidate all associated interfaces */
  if (thiz->engineObject) {
    (*thiz->engineObject)->Destroy (thiz->engineObject);
    thiz->engineObject = NULL;
    thiz->engineEngine = NULL;
  }

  thiz->bufferQueue = NULL;

  GST_DEBUG_OBJECT (thiz, "device closed");
  return TRUE;
}

static gboolean
gst_opensles_ringbuffer_acquire (GstRingBuffer * rb, GstRingBufferSpec * spec)
{
  GstOpenSLESRingBuffer *thiz;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);

  /* Instantiate and configure the OpenSL ES interfaces */
  if (!thiz->acquire (rb, spec)) {
    return FALSE;
  }

  /* Initialize our ringbuffer memory region */
  rb->data = gst_buffer_new_and_alloc (spec->segtotal * spec->segsize);
  memset (GST_BUFFER_DATA (rb->data), 0, GST_BUFFER_SIZE (rb->data));

  GST_DEBUG_OBJECT (thiz, "ringbuffer acquired");
  return TRUE;
}

static gboolean
gst_opensles_ringbuffer_release (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;

  thiz = GST_OPENSLES_RING_BUFFER (rb);

  /* Destroy audio player object, and invalidate all associated interfaces */
  if (thiz->playerObject) {
    (*thiz->playerObject)->Destroy (thiz->playerObject);
    thiz->playerObject = NULL;
    thiz->playerPlay = NULL;
    thiz->playerVolume = NULL;
  }

  /* Destroy audio recorder object, and invalidate all associated interfaces */
  if (thiz->recorderObject) {
    (*thiz->recorderObject)->Destroy (thiz->recorderObject);
    thiz->recorderObject = NULL;
    thiz->recorderRecord = NULL;
  }

  if (rb->data) {
    gst_buffer_unref (rb->data);
    rb->data = NULL;
  }
  GST_DEBUG_OBJECT (thiz, "ringbuffer released");
  return TRUE;
}

static gboolean
gst_opensles_ringbuffer_start (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  gboolean res;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  res = thiz->start (rb);

  GST_DEBUG_OBJECT (thiz, "ringbuffer %s started", (res ? "" : "not"));
  return res;
}

static gboolean
gst_opensles_ringbuffer_pause (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  gboolean res;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  res = thiz->pause (rb);

  GST_DEBUG_OBJECT (thiz, "ringbuffer %s paused", (res ? "" : "not"));
  return res;
}

static gboolean
gst_opensles_ringbuffer_stop (GstRingBuffer * rb)
{
  GstOpenSLESRingBuffer *thiz;
  gboolean res;

  thiz = GST_OPENSLES_RING_BUFFER_CAST (rb);
  res = thiz->stop (rb);

  GST_DEBUG_OBJECT (thiz, "ringbuffer %s stopped", (res ? " " : "not"));
  return res;
}


static guint
gst_opensles_ringbuffer_delay (GstRingBuffer * rb)
{
  return 0;
}

static void
gst_opensles_ringbuffer_dispose (GObject * object)
{
  G_OBJECT_CLASS (ring_parent_class)->dispose (object);
}

static void
gst_opensles_ringbuffer_finalize (GObject * object)
{
  G_OBJECT_CLASS (ring_parent_class)->finalize (object);
}

static void
gst_opensles_ringbuffer_base_init (gpointer g_class)
{
  /* Nothing to do right now */
}

static void
gst_opensles_ringbuffer_class_init (GstOpenSLESRingBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstRingBufferClass *gstringbuffer_class;

  gobject_class = (GObjectClass *) klass;
  gstringbuffer_class = (GstRingBufferClass *) klass;

  ring_parent_class = g_type_class_peek_parent (klass);

  gobject_class->dispose = gst_opensles_ringbuffer_dispose;
  gobject_class->finalize = gst_opensles_ringbuffer_finalize;

  gstringbuffer_class->open_device =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_open_device);
  gstringbuffer_class->close_device =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_close_device);
  gstringbuffer_class->acquire =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_acquire);
  gstringbuffer_class->release =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_release);
  gstringbuffer_class->start =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_start);
  gstringbuffer_class->pause =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_pause);
  gstringbuffer_class->resume =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_start);
  gstringbuffer_class->stop = GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_stop);
  gstringbuffer_class->delay =
      GST_DEBUG_FUNCPTR (gst_opensles_ringbuffer_delay);
}

static void
gst_opensles_ringbuffer_init (GstOpenSLESRingBuffer * thiz,
    GstOpenSLESRingBufferClass * g_class)
{
  thiz->mode = RB_MODE_NONE;
  thiz->engineObject = NULL;
  thiz->outputMixObject = NULL;
  thiz->playerObject = NULL;
  thiz->recorderObject = NULL;
}
