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

#ifndef __GST_PROMISE_H__
#define __GST_PROMISE_H__

#include <gst/gst.h>

G_BEGIN_DECLS

GST_EXPORT
GType gst_promise_get_type(void);
#define GST_TYPE_PROMISE            (gst_promise_get_type())
#define GST_PROMISE(obj)            ((GstPromise *) obj)

typedef struct _GstPromise GstPromise;

/**
 * GstPromiseResult:
 * @GST_PROMISE_RESULT_PENDING: Initial state
 * @GST_PROMISE_RESULT_INTERRUPTED: Interrupted by caller, other state changes ignored
 * @GST_PROMISE_RESULT_REPLIED: a receiver marked a reply
 * @GST_PROMISE_RESULT_EXPIRED: The promise expired (the carrying message lost all refs)
 *
 * The result of a #GstPromise
 */
typedef enum
{
  GST_PROMISE_RESULT_PENDING,
  GST_PROMISE_RESULT_INTERRUPTED,
  GST_PROMISE_RESULT_REPLIED,
  GST_PROMISE_RESULT_EXPIRED,
} GstPromiseResult;

/**
 * GstPromiseChangeFunc:
 * @promise: a #GstPromise
 * @result: the change
 * @user_data: (closure): user data
 */
typedef void (*GstPromiseChangeFunc) (GstPromise * promise, gpointer user_data);

/**
 * GstPromise:
 * @parent: parent #GstMiniObject
 * @result: #GstPromiseResult state the promise is in
 * @promise: #GstStructure for the data that has been replied
 */
struct _GstPromise
{
  GstMiniObject         parent;

  GstPromiseResult      result;
  GstStructure         *promise;

  /*< private >*/
  GMutex                lock;
  GCond                 cond;
  GstPromiseChangeFunc  change_func;
  gpointer              user_data;
  GDestroyNotify        notify;

  gpointer              _padding[GST_PADDING];
};

GST_EXPORT
GstPromise *            gst_promise_new                 (void);
GST_EXPORT
void                    gst_promise_set_change_callback (GstPromise * promise,
                                                         GstPromiseChangeFunc func,
                                                         gpointer user_data,
                                                         GDestroyNotify notify);
GST_EXPORT
GstPromiseResult        gst_promise_wait                (GstPromise * promise);
GST_EXPORT
void                    gst_promise_reply               (GstPromise * promise, GstStructure * s);
GST_EXPORT
void                    gst_promise_interrupt           (GstPromise * promise);
GST_EXPORT
void                    gst_promise_expire              (GstPromise * promise);

/**
 * gst_promise_ref:
 * @promise: a #GstRespone.
 *
 * Increases the refcount of the given @promise by one.
 *
 * Returns: (transfer full): @promise
 */
static inline GstPromise *
gst_promise_ref (GstPromise * promise)
{
  return (GstPromise *) gst_mini_object_ref (GST_MINI_OBJECT_CAST (promise));
}

/**
 * gst_promise_unref:
 * @promise: (transfer full): a #GstPromise.
 *
 * Decreases the refcount of the promise. If the refcount reaches 0, the
 * promise will be freed.
 */
static inline void
gst_promise_unref (GstPromise * promise)
{
  gst_mini_object_unref (GST_MINI_OBJECT_CAST (promise));
}

G_END_DECLS

#endif /* __GST_PROMISE_H__ */
