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



typedef struct
{
    QdEventPtr		pending, pendtail;

    /* The ending time. This is used when we thaw (no interaction with the event queue). */
    Time		time;
} queue_data;



#if DEBUG_PIPELINE
/* return the count of events in event_list */
static int
queue_count_events(queue_data* event_list)
{
    int i = 0;
    QdEventPtr pending;

    for (pending = event_list->pending; pending; pending = pending->next)
	i++;
    return i;
}
#endif	/* DEBUG_PIPELINE */


/**
 * append to the end of linked list
 * (copied from ../dix/events.c  EnqueueEvent)
 * */
static void
queue_insert(queue_data *data, InternalEvent *ev, Bool owner)
{
    DeviceEvent *event = &ev->device_event;
    int eventlen = event->length;
    QdEventPtr qe;

    /* assert (is_device_event(ev)); */
    qe = (QdEventPtr)malloc(sizeof(QdEventRec) + eventlen);

    qe->next = (QdEventPtr)NULL;
    qe->event = (InternalEvent *)(qe + 1);
    memcpy(qe->event, event, eventlen);

    if (owner)
	free(event);

    if (!data->pending)
    {
	/* first element of the list -> create the list! */
	data->pending = qe;
	data->pendtail = qe;
    }
    else
    {
	(data->pendtail)->next = qe;
	data->pendtail = qe;
    }
}

static void
queue_process_key_event(PluginInstance* plugin, InternalEvent *event, Bool owner)
{
//    CHECKEVENT(event);
    if (plugin_frozen(plugin->next))
    {
	queue_insert(plugin->data, event, owner);
        plugin->wakeup_time = 0;
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
	}
	if (plugin_frozen(next)) {
	    ((queue_data*)plugin->data)->time = time;
	}
    } else
        plugin->wakeup_time = 0;
}


static void
queue_thaw(PluginInstance* plugin, Time time)
{
    PluginInstance* next = plugin->next;
    queue_data* data = (queue_data*) plugin->data;

    /* push from the queue. */
    while (! plugin_frozen(next) && (data->pending))
    {
	PluginClass(next)->ProcessEvent(next, data->pending->event,
					FALSE); /* we _don't_ pass the ownership! */
	{
	    /* remove from the list: */
	    QdEventPtr qe;
	    qe = data->pending->next;
	    free(data->pending);
	    data->pending = qe;
	}
    }

    if (! data->pending)
	data->pendtail = 0;

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
    plugin->data = calloc(1, sizeof(queue_data));

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

