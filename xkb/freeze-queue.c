/* (c) 2004-2010 Michal Maruska <mmaruska@gmail.com>
 * 
 * Queue processor: implements the replacement of syncEvents */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "pipeline.h"

#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include "inputstr.h"
#include <events.h>
#include <eventstr.h>

#include "xkb.h"


#include "list.h"

#define TIME_FORMAT "%lu"
typedef struct
{
    // so now QdEventPtr is meant to be used inside Doubly-linked list.
    struct xorg_list pending;
    /* The ending time. This is used when we thaw (no interaction with
     * the event queue). */
    Time		time;
} queue_data;



#if DEBUG_PIPELINE
/* return the count of events in event_list */
static int
queue_count_events(queue_data* event_list)
{
    int i = 0;
    QdEventPtr pending;

    xorg_list_for_each_entry(pending, &event_list->pending, next)
        i++;
    return i;
}
#endif	/* DEBUG_PIPELINE */


/**
 * append to the end of linked list
 * (copied from ../dix/events.c  EnqueueEvent)
 * */
static void
queue_append(queue_data *data, InternalEvent *ev, Bool owner)
{
    DeviceEvent *event = &ev->device_event;
    int eventlen = event->length;
    QdEventPtr qe;

    /* assert (is_device_event(ev)); */
    qe = (QdEventPtr)malloc(sizeof(QdEventRec) + eventlen);

    xorg_list_append(&qe->next, &data->pending);

    qe->event = (InternalEvent *)(qe + 1);
    memcpy(qe->event, event, eventlen);

    if (owner)
	free(event);

}

static void
queue_process_key_event(PluginInstance* plugin, InternalEvent *event, Bool owner)
{
//    CHECKEVENT(event);
    if (plugin_frozen(plugin->next))
    {
	queue_append(plugin->data, event, owner);
        plugin->wakeup_time = 0;
#if DEBUG_PIPELINE
	ErrorF("%s: now %d events in queue\n", __FUNCTION__,
	       queue_count_events((queue_data*)plugin->data));
#endif
    } else {
	PluginClass(plugin->next)->ProcessEvent(plugin->next, event, owner);
	/* this plugin is not interested in time */
	plugin->wakeup_time = plugin->next->wakeup_time;
    }
}


static void
queue_accept_time(PluginInstance* plugin, Time time)
{

    PluginInstance* next = plugin->next;
    assert(next);

    if (!plugin_frozen(next)) {
	if (next->wakeup_time && (next->wakeup_time <= time)) {
            PluginClass(next)->ProcessTime(next, time);
            plugin->wakeup_time = next->wakeup_time;
	} else if (next->wakeup_time) {
            /* uselessly woken? */
#if DEBUG_PIPELINE
	    /* this is unexpected! */
            ErrorF("%s: too early for the next plugin. now " TIME_FORMAT
		   ", needed too early " TIME_FORMAT " (" TIME_FORMAT ")\n",
		   __FUNCTION__, time,
		   (next->wakeup_time - time), next->wakeup_time);
#endif
	}
	if (plugin_frozen(next)) {
#if DEBUG_PIPELINE
	    ErrorF("%s: ->pushing _our_ current time\n", __FUNCTION__);
#endif
	    ((queue_data*)plugin->data)->time = time;
	}
    } else {
        plugin->wakeup_time = 0;
#if DEBUG_PIPELINE
	ErrorF("%s: ->next is frozen\n", __FUNCTION__);
#endif
    }
}


static void
queue_thaw(PluginInstance* plugin, Time time)
{
    PluginInstance* next = plugin->next;
    queue_data* data = (queue_data*) plugin->data;

#if DEBUG_PIPELINE
    ErrorF("%s: %d events in the queue!\n", __FUNCTION__, queue_count_events(data));
#endif

    /* push from the queue. */
    while (! plugin_frozen(next) && !(xorg_list_is_empty(&data->pending))) {
        QdEventPtr event = xorg_list_first_entry(&data->pending,
                                                 QdEventRec, next);
	PluginClass(next)->ProcessEvent(next,
                                        event,
#if 0
                                        container_of(&data->pending,
                                                     QdEventRec, next),
#endif
					FALSE); /* we _don't_ pass the ownership! */
        /* fixme: why not? */
	{
	    /* remove from the list: */
#if 0
	    QdEventPtr qe;
	    qe = data->pending->next;

	    free(data->pending);
	    data->pending = qe;
#else
            // QdEventPtr qe = data->pending.next;
            xorg_list_del(&event->next);
            free(event);
#endif
	}
    }

#if 0
    if (! data->pending)
	data->pendtail = 0;
#else

#endif
    /* if still not frozen, push by time: */
    if (!plugin_frozen(next) && (data->time)) {
	PluginClass(next)->ProcessTime(next, data->time);
	data->time = 0;
    }

    plugin->wakeup_time = next->wakeup_time;
    /* Since we are never frozen, we don't signal the previous plugin!
     * like: PluginClass(previous)->Thaw(next, time);
     * Anyway, there is no previous, this is the head of the pipeline */
}



static PluginInstance*
freeze_queue_init(DeviceIntPtr dev, DevicePluginRec* plugin_class)
{
    PluginInstance* plugin = calloc(1, sizeof(PluginInstance));

    queue_data* queue = calloc(1, sizeof(queue_data));
    xorg_list_init(&queue->pending);
    plugin->data = queue;

    plugin->pclass = plugin_class;
    plugin->device = dev;

    return plugin;
}

DevicePluginRec queue_plugin =
{
    "never-freeze-queue",
    freeze_queue_init,
    queue_process_key_event,
    queue_accept_time,
    queue_thaw,
    NULL, NULL,                        /* config */
    NULL,
    NULL,
};

