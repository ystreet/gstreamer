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
 * with gst_promise_reply(), interrupted with gst_promise_reply() and
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

  g_mutex_lock (&promise->lock);

  prev_notify = promise->notify;
  prev_data = promise->user_data;
  promise->change_func = func;
  promise->user_data = user_data;
  promise->notify = notify;

  g_mutex_unlock (&promise->lock);

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

  g_mutex_lock (&promise->lock);
  ret = promise->result;

  while (ret == GST_PROMISE_RESULT_PENDING) {
    GST_LOG ("%p waiting", promise);
    g_cond_wait (&promise->cond, &promise->lock);
    ret = promise->result;
  }
  GST_LOG ("%p waited", promise);

  g_mutex_unlock (&promise->lock);

  return ret;
}

/**
 * gst_promise_reply:
 * @promise (allow-none): a #GstPromise
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

  g_mutex_lock (&promise->lock);
  if (promise->result != GST_PROMISE_RESULT_PENDING &&
      promise->result != GST_PROMISE_RESULT_INTERRUPTED) {
    GstPromiseResult result = promise->result;
    g_mutex_unlock (&promise->lock);
    g_return_if_fail (result == GST_PROMISE_RESULT_PENDING ||
        result == GST_PROMISE_RESULT_INTERRUPTED);
  }

  /* XXX: is this necessary and valid? */
  if (promise->promise && promise->promise != s)
    gst_structure_free (promise->promise);

  /* Only reply iff we are currently in pending */
  if (promise->result == GST_PROMISE_RESULT_PENDING) {
    promise->result = GST_PROMISE_RESULT_REPLIED;
    GST_LOG ("%p replied", promise);

    promise->promise = s;

    if (promise->change_func)
      promise->change_func (promise, promise->user_data);
  } else {
    /* eat the value */
    if (s)
      gst_structure_free (s);
  }

  g_cond_broadcast (&promise->cond);
  g_mutex_unlock (&promise->lock);
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

  g_mutex_lock (&promise->lock);
  if (promise->result != GST_PROMISE_RESULT_PENDING &&
      promise->result != GST_PROMISE_RESULT_REPLIED) {
    GstPromiseResult result = promise->result;
    g_mutex_unlock (&promise->lock);
    g_return_if_fail (result == GST_PROMISE_RESULT_PENDING ||
        result == GST_PROMISE_RESULT_REPLIED);
  }
  /* only interrupt if we are currently in pending */
  if (promise->result == GST_PROMISE_RESULT_PENDING) {
    promise->result = GST_PROMISE_RESULT_INTERRUPTED;
    g_cond_broadcast (&promise->cond);
    GST_LOG ("%p interrupted", promise);
    if (promise->change_func)
      promise->change_func (promise, promise->user_data);
  }
  g_mutex_unlock (&promise->lock);
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

  g_mutex_lock (&promise->lock);
  if (promise->result == GST_PROMISE_RESULT_PENDING) {
    promise->result = GST_PROMISE_RESULT_EXPIRED;
    g_cond_broadcast (&promise->cond);
    GST_LOG ("%p expired", promise);
    if (promise->change_func)
      promise->change_func (promise, promise->user_data);
  }
  g_mutex_unlock (&promise->lock);
}

static void
gst_promise_free (GstMiniObject * object)
{
  GstPromise *promise = (GstPromise *) object;

  /* the promise *must* be dealt with in some way before destruction */
  g_warn_if_fail (promise->result != GST_PROMISE_RESULT_PENDING);

  if (promise->notify)
    promise->notify (promise->user_data);

  if (promise->promise)
    gst_structure_free (promise->promise);
  g_mutex_clear (&promise->lock);
  g_cond_clear (&promise->cond);
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

  promise->promise = NULL;
  promise->result = GST_PROMISE_RESULT_PENDING;
  g_mutex_init (&promise->lock);
  g_cond_init (&promise->cond);
}

/**
 * gst_promise_new:
 *
 * Returns: a new #GstPromise
 */
GstPromise *
gst_promise_new (void)
{
  GstPromise *promise = g_new0 (GstPromise, 1);

  gst_promise_init (promise);
  GST_LOG ("new promise %p", promise);

  return promise;
}

GST_DEFINE_MINI_OBJECT_TYPE (GstPromise, gst_promise);
