/* GStreamer
 * Copyright (C) 2017 Matthew Waters <matthew@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "gst_private.h"

#include "gstpromise.h"

#define GST_CAT_DEFAULT gst_promise_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

/**
 * SECTION:gstpromise
 * @title: GstRespsone
 * @short_description: a miniobject for future/promise-like functionality
 * @see_also:
 *
 * The #GstPromise object implements the container for values that may
 * be available later. i.e. a Future or a Promise in
 * <ulink url="https://en.wikipedia.org/wiki/Futures_and_promises">https://en.wikipedia.org/wiki/Futures_and_promises</ulink>
 *
 * A #GstPromise can be created with gst_promise_new(), replied to
 * with gst_promise_reply(), interrupted with gst_promise_interrupt() and
 * expired with gst_promise_expire(). A callback can also be installed for
 * result changes with gst_promise_set_change_callback().
 *
 * Each #GstPromise starts out with a #GstPromiseResult of
 * %GST_PROMISE_RESULT_PENDING and only ever transitions out of that result
 * into one of the other #GstPromiseResult.
 *
 * In order to support multi-threaded code, gst_promise_reply(),
 * gst_promise_interrupt() and gst_promise_expire() may all be from
 * different threads with some restrictions, the final result of the promise
 * is whichever call is made first.  There are two restrictions on ordering:
 *
 * 1. That gst_promise_reply() and gst_promise_interrupt() cannot be called
 * after gst_promise_expire()
 * 2. That gst_promise_reply() and gst_promise_interrupt()
 * cannot be called twice.
 */

#define GST_PROMISE_REPLY(p)            (((GstPromiseImpl *)(p))->reply)
#define GST_PROMISE_RESULT(p)           (((GstPromiseImpl *)(p))->result)
#define GST_PROMISE_LOCK(p)             (&(((GstPromiseImpl *)(p))->lock))
#define GST_PROMISE_COND(p)             (&(((GstPromiseImpl *)(p))->cond))
#define GST_PROMISE_CHANGE_FUNC(p)      (((GstPromiseImpl *)(p))->change_func)
#define GST_PROMISE_CHANGE_DATA(p)      (((GstPromiseImpl *)(p))->user_data)
#define GST_PROMISE_CHANGE_NOTIFY(p)    (((GstPromiseImpl *)(p))->notify)

typedef struct
{
  GstPromise promise;

  GstPromiseResult result;
  GstStructure *reply;

  GMutex lock;
  GCond cond;
  GstPromiseChangeFunc change_func;
  gpointer user_data;
  GDestroyNotify notify;
} GstPromiseImpl;

/**
 * gst_promise_set_reply_callback:
 * @promise: a #GstPromise
 * @func: a #GstPromiseChangeFunc
 * @user_data: (scope async): data to invoke @func with
 * @notify: a #GDestroyNotify to free @data when no longer needed
 */
/* XXX: do we want to cater for the case where the following could happen?
 * gst_promise_new()
 * gst_promise_reply()
 * gst_promise_set_change_callback () on different thread to gst_promise_reply().
 */
void
gst_promise_set_change_callback (GstPromise * promise,
    GstPromiseChangeFunc func, gpointer user_data, GDestroyNotify notify)
{
  GDestroyNotify prev_notify;
  gpointer prev_data;

  g_return_if_fail (promise != NULL);

  g_mutex_lock (GST_PROMISE_LOCK (promise));

  prev_notify = GST_PROMISE_CHANGE_NOTIFY (promise);
  prev_data = GST_PROMISE_CHANGE_DATA (promise);
  GST_PROMISE_CHANGE_FUNC (promise) = func;
  GST_PROMISE_CHANGE_DATA (promise) = user_data;
  GST_PROMISE_CHANGE_NOTIFY (promise) = notify;

  g_mutex_unlock (GST_PROMISE_LOCK (promise));

  if (prev_notify)
    prev_notify (prev_data);
}

/**
 * gst_promise_wait:
 * @promise: a #GstPromise
 *
 * Wait for @promise to move out of the %GST_PROMISE_RESULT_PENDING state.
 * If @promise is not in %GST_PROMISE_RESULT_PENDING then it will return
 * immediately with the current result.
 *
 * Returns: the result of the promise
 */
GstPromiseResult
gst_promise_wait (GstPromise * promise)
{
  GstPromiseResult ret;

  g_return_val_if_fail (promise != NULL, GST_PROMISE_RESULT_EXPIRED);

  g_mutex_lock (GST_PROMISE_LOCK (promise));
  ret = GST_PROMISE_RESULT (promise);

  while (ret == GST_PROMISE_RESULT_PENDING) {
    GST_LOG ("%p waiting", promise);
    g_cond_wait (GST_PROMISE_COND (promise), GST_PROMISE_LOCK (promise));
    ret = GST_PROMISE_RESULT (promise);
  }
  GST_LOG ("%p waited", promise);

  g_mutex_unlock (GST_PROMISE_LOCK (promise));

  return ret;
}

/**
 * gst_promise_reply:
 * @promise: (allow-none): a #GstPromise
 * @s: (transfer full): a #GstStructure with the the reply contents
 *
 * Set a reply on @promise.  This will wake up any waiters with
 * %GST_PROMISE_RESULT_REPLIED.
 */
void
gst_promise_reply (GstPromise * promise, GstStructure * s)
{
  /* Caller requested that no reply is necessary */
  if (promise == NULL)
    return;

  g_mutex_lock (GST_PROMISE_LOCK (promise));
  if (GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_PENDING &&
      GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_INTERRUPTED) {
    GstPromiseResult result = GST_PROMISE_RESULT (promise);
    g_mutex_unlock (GST_PROMISE_LOCK (promise));
    g_return_if_fail (result == GST_PROMISE_RESULT_PENDING ||
        result == GST_PROMISE_RESULT_INTERRUPTED);
  }

  /* XXX: is this necessary and valid? */
  if (GST_PROMISE_REPLY (promise) && GST_PROMISE_REPLY (promise) != s)
    gst_structure_free (GST_PROMISE_REPLY (promise));

  /* Only reply iff we are currently in pending */
  if (GST_PROMISE_RESULT (promise) == GST_PROMISE_RESULT_PENDING) {
    GST_PROMISE_RESULT (promise) = GST_PROMISE_RESULT_REPLIED;
    GST_LOG ("%p replied", promise);

    GST_PROMISE_REPLY (promise) = s;

    if (GST_PROMISE_CHANGE_FUNC (promise))
      GST_PROMISE_CHANGE_FUNC (promise) (promise,
          GST_PROMISE_CHANGE_DATA (promise));
  } else {
    /* eat the value */
    if (s)
      gst_structure_free (s);
  }

  g_cond_broadcast (GST_PROMISE_COND (promise));
  g_mutex_unlock (GST_PROMISE_LOCK (promise));
}

/**
 * gst_promise_get_reply:
 * @promise: a #GstPromise
 *
 * Retrieve the reply set on @promise.  @promise must be in
 * %GST_PROMISE_RESULT_REPLIED
 *
 * Returns: (transfer none): The reply set on @promise
 */
const GstStructure *
gst_promise_get_reply (GstPromise * promise)
{
  g_return_val_if_fail (promise != NULL, NULL);

  g_mutex_lock (GST_PROMISE_LOCK (promise));
  if (GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_REPLIED) {
    GstPromiseResult result = GST_PROMISE_RESULT (promise);
    g_mutex_unlock (GST_PROMISE_LOCK (promise));
    g_return_val_if_fail (result == GST_PROMISE_RESULT_REPLIED, NULL);
  }

  g_mutex_unlock (GST_PROMISE_LOCK (promise));

  return GST_PROMISE_REPLY (promise);
}

/**
 * gst_promise_interrupt:
 * @promise: a #GstPromise
 *
 * Interrupt waiting for a @promise.  This will wake up any waiters with
 * %GST_PROMISE_RESULT_INTERRUPTED
 */
void
gst_promise_interrupt (GstPromise * promise)
{
  g_return_if_fail (promise != NULL);

  g_mutex_lock (GST_PROMISE_LOCK (promise));
  if (GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_PENDING &&
      GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_REPLIED) {
    GstPromiseResult result = GST_PROMISE_RESULT (promise);
    g_mutex_unlock (GST_PROMISE_LOCK (promise));
    g_return_if_fail (result == GST_PROMISE_RESULT_PENDING ||
        result == GST_PROMISE_RESULT_REPLIED);
  }
  /* only interrupt if we are currently in pending */
  if (GST_PROMISE_RESULT (promise) == GST_PROMISE_RESULT_PENDING) {
    GST_PROMISE_RESULT (promise) = GST_PROMISE_RESULT_INTERRUPTED;
    g_cond_broadcast (GST_PROMISE_COND (promise));
    GST_LOG ("%p interrupted", promise);
    if (GST_PROMISE_CHANGE_FUNC (promise))
      GST_PROMISE_CHANGE_FUNC (promise) (promise,
          GST_PROMISE_CHANGE_DATA (promise));
  }
  g_mutex_unlock (GST_PROMISE_LOCK (promise));
}

/**
 * gst_promise_expire:
 * @promise: a #GstPromise
 *
 * Expire a @promise.  This will wake up any waiters with
 * %GST_PROMISE_RESULT_EXPIRED
 */
void
gst_promise_expire (GstPromise * promise)
{
  g_return_if_fail (promise != NULL);

  g_mutex_lock (GST_PROMISE_LOCK (promise));
  if (GST_PROMISE_RESULT (promise) == GST_PROMISE_RESULT_PENDING) {
    GST_PROMISE_RESULT (promise) = GST_PROMISE_RESULT_EXPIRED;
    g_cond_broadcast (GST_PROMISE_COND (promise));
    GST_LOG ("%p expired", promise);
    if (GST_PROMISE_CHANGE_FUNC (promise))
      GST_PROMISE_CHANGE_FUNC (promise) (promise,
          GST_PROMISE_CHANGE_DATA (promise));
  }
  g_mutex_unlock (GST_PROMISE_LOCK (promise));
}

/**
 * gst_promise_get_result:
 * @promise: a #GstPromise
 *
 * Retrieve the reply set on @promise.  @promise must be in
 * %GST_PROMISE_RESULT_REPLIED
 *
 * Returns: (transfer none): The reply set on @promise
 */
GstPromiseResult
gst_promise_get_result (GstPromise * promise)
{
  g_return_val_if_fail (promise != NULL, GST_PROMISE_RESULT_EXPIRED);

  return GST_PROMISE_RESULT (promise);
}

static void
gst_promise_free (GstMiniObject * object)
{
  GstPromise *promise = (GstPromise *) object;

  /* the promise *must* be dealt with in some way before destruction */
  g_warn_if_fail (GST_PROMISE_RESULT (promise) != GST_PROMISE_RESULT_PENDING);

  if (GST_PROMISE_CHANGE_NOTIFY (promise))
    GST_PROMISE_CHANGE_NOTIFY (promise) (GST_PROMISE_CHANGE_DATA (promise));

  if (GST_PROMISE_REPLY (promise))
    gst_structure_free (GST_PROMISE_REPLY (promise));
  g_mutex_clear (GST_PROMISE_LOCK (promise));
  g_cond_clear (GST_PROMISE_COND (promise));
  GST_LOG ("%p finalized", promise);

  g_free (promise);
}

static void
gst_promise_init (GstPromise * promise)
{
  static volatile gsize _init = 0;

  if (g_once_init_enter (&_init)) {
    GST_DEBUG_CATEGORY_INIT (gst_promise_debug, "gstpromise", 0, "gstpromise");
    g_once_init_leave (&_init, 1);
  }

  gst_mini_object_init (GST_MINI_OBJECT (promise), 0, GST_TYPE_PROMISE, NULL,
      NULL, gst_promise_free);

  GST_PROMISE_REPLY (promise) = NULL;
  GST_PROMISE_RESULT (promise) = GST_PROMISE_RESULT_PENDING;
  g_mutex_init (GST_PROMISE_LOCK (promise));
  g_cond_init (GST_PROMISE_COND (promise));
}

/**
 * gst_promise_new:
 *
 * Returns: a new #GstPromise
 */
GstPromise *
gst_promise_new (void)
{
  GstPromise *promise = GST_PROMISE (g_new0 (GstPromiseImpl, 1));

  gst_promise_init (promise);
  GST_LOG ("new promise %p", promise);

  return promise;
}

GST_DEFINE_MINI_OBJECT_TYPE (GstPromise, gst_promise);
