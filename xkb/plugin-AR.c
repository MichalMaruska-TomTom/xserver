/* (c) 2005-2007,2010  Michal Maruska <mmaruska@gmail.com>
 * Reimplements key auto-repeat, using the new pipeline architecture.
 */

/* linked-list templates taken from timers code in ../os/WaitFor.c */

#include "pipeline.h"

#include "inputstr.h"
#include <events.h>
#include <eventstr.h>

/* If I could get the repeat-rate w/o using XKB... */

#define DEBUG_AUTOREPEAT 1

#include <X11/extensions/XKBsrv.h>

#include <inttypes.h>
#define TIME_FORMAT PRIu32

/* How it works:
 * the processor has a queue of keys to `repeat'. (single-linked list)
 * todo: with devices (keyboards).
 * This linked-list is  orderer by `time' (ascending) of when to run/generate
 * a repetition event:
 * each keycode can be only once!
 *
 * todo: Better to keep the previous event time, as the pause can be changed,
 * so we need to recompute the next event time (hence need the base)!
 */

typedef struct _key_repeat_info key_repeat_info;


/* Single-linked list */
struct _key_repeat_info
{
    // DeviceIntPtr device;
    Time    time;                   /* Time of next action.
                                       (press, or release/press or only release)*/
    KeyCode key;
    Bool    press;                 /* we could be interrupted in between the 2 events!
                                      By a freeze.
                                      todo: what if the AR is disabled in between, by XKB calls */
    key_repeat_info* next;
};

/* todo:  `press' is only relevant at the head! */
typedef struct
{
    InternalEvent* event;	/* accepted event, but not yet looked at,
                                 * since next is frozen. todo: avoid */
    key_repeat_info *repeats;   /* the list of keys down: to be AR-ed */
} ar_plugin_data;



/* When a processor/plugin needs to generate a new event, use this:
 * the next processor should be accepting (not frozen). */

static void
ar_synthesize_event(PluginInstance* plugin,
                    DeviceIntPtr	keybd,
                    BYTE	type,
                    BYTE	keyCode,
                    Time  time)
{
    DeviceEvent event;
#if 1
    /* how to init a DeviceEvent? For now I init the union-sibling. */
    init_raw(keybd, (RawDeviceEvent*) &event, time, type, keyCode);

    /* But it's not raw, it's keyboard. */
    if (type == KeyRelease) {
        event.device_event.type = ET_KeyRelease;
    } else {
        event.device_event.type = ET_KeyPress;
    }
#else
    memset(&event, 0, sizeof(DeviceEvent));
    event.header = ET_Internal;
    event.length = sizeof(DeviceEvent);
    event.time = time;
    event.sourceid = keybd->id;
    event.deviceid = keybd->id;
#endif

    event.type = type;
    event.detail.key = keyCode;
    event.key_repeat = TRUE;

    assert(!plugin_frozen(plugin->next));
    PluginClass(plugin->next)->ProcessEvent(plugin->next,(InternalEvent*) &event,
                                            CALLER_OWNS);
}


// next/first repeat.  But it might depend on devices frozen!
#define first_repeats(plugin)    ((ar_plugin_data*) plugin->data)->repeats

#define color_trace 1
#include "color-debug.h"
#ifdef color_trace
#endif


/* put in the ordered single-linked list */
static void
insert_into_repeats(PluginInstance* plugin, key_repeat_info* node)
{
    /* the "for" cycle is taken from ../os/WaitFor.c
       todo:  make it a function: inserts into an ordered linked list */

    Time expires = node->time;
    key_repeat_info **prev;

    // skip to the end:
    for (prev = &(((ar_plugin_data*) plugin->data)->repeats);
         *prev && (int) ((*prev)->time <=  expires);
         prev = &(*prev)->next)
        ;/*do nothing*/

    node->next = *prev;
    *prev = node;
}


/* Is the keycode already in list (of repeats)? */
inline static int
keycode_in_repeats_p(PluginInstance* plugin, KeyCode keycode)
{
    /* again, insert into an ordered linked list: */
    key_repeat_info **prev;

    for (prev = &(((ar_plugin_data*) plugin->data)->repeats);
         *prev;
         prev = &(*prev)->next)
        if ((*prev)->key == keycode)
            return 1;
    return 0;
}

/* synthesize AR event(s) for the Node.
 *
 * 1 or 2 events (release & press) are emitted, depending on the next processor being
 * frozen by the Release, for 1 node (= 1 keycode) at time NOW  */
static void
ar_process_and_reinsert(PluginInstance* plugin, key_repeat_info* node,
                        Time time)
{
    DeviceIntPtr keybd = plugin->device;
    KeyCode key = node->key;

    /* node is the currently first in scheduled_repeats, so it's scheduled_repeats itself. */
    assert(node == first_repeats(plugin)); /* scheduled_repeats */
    /* the keyboard is not frozen! */
    assert(!plugin_frozen(plugin->next));


    if (!node->press) {
        /* todo: this should use:
         *  if (_XkbWantsDetectableAutoRepeat(pClient))
         */
        /* The time does not change! So it remains at the head of the list! */
        ar_synthesize_event(plugin,
                            plugin->device,
                            KeyRelease, key, time);
        node->press = TRUE;
    }

    if (plugin_frozen(plugin->next)) {
        return;
    }

    ar_synthesize_event(plugin,
                        plugin->device, KeyPress,key, time);
    node->press = FALSE;

    {
        XkbSrvInfoPtr xkbi = keybd->key->xkbInfo;
        int delay = xkbi->desc->ctrls->repeat_interval;
        if (delay == 0)
            delay = 25;		/* fixme! */

        /* remove from the queue */
        first_repeats(plugin) = node->next; /* fixme! */
        /* schedule the release: */
        // if the xkb is change, we might need to re-calculate the time!
        /* but we should be keeping the past time.... bug!  */
        node->time += delay;
        insert_into_repeats(plugin, node);
    }
}


/**** `PlayReleasedEvents'   */

/* we know a time, and want to see if auto-repeat has any events to send, before TIME.
 * If so, send it until the dev is frozen.
 * returns TRUE iff an event was generated. (could return time!) */
static Bool
ar_push_events(PluginInstance* plugin, /* CARD32 month,*/ Time time)
{
    key_repeat_info *scheduled_repeats =  first_repeats(plugin);
    PluginInstance* next = plugin->next;

    /* CARD32 month = 0;*/

    /* do we have something in the queue? */
    /* but we don't know any time... do we? */
    if (scheduled_repeats) {
        int generated = 0;
        while (!plugin_frozen(next) && (time >= scheduled_repeats->time)) {
            ar_process_and_reinsert(plugin, scheduled_repeats,
                                    scheduled_repeats->time);
            generated++;
            scheduled_repeats = first_repeats(plugin);
        }
        return TRUE;
    } else {
        return FALSE;
    }
}


static void
ar_start_keycode(PluginInstance* plugin, KeyCode key, CARD32 now)
{
    DeviceIntPtr keybd = plugin->device;
    XkbSrvInfoPtr xkbi =  keybd->key->xkbInfo;
    XkbControlsPtr ctrls = xkbi->desc->ctrls;
    int delay;

    if (keycode_in_repeats_p(plugin, key)) {
    } else {
        /* insert into the list of `repeating' keys */
        key_repeat_info* node = (key_repeat_info*)malloc(sizeof(key_repeat_info));

        node->next = NULL;
        node->key = key;
        node->press = FALSE;

        delay = ctrls->repeat_delay; /* fixme: global */
        if (delay == 0)
            delay = 190;
        node->time = now + delay;
        insert_into_repeats(plugin, node);
    }
}

/* key is Released, do not auto-repeat anymore. */
static void
ar_cancel_key(PluginInstance* plugin, KeyCode key)
{
   key_repeat_info **prev;

   // assert (first_repeats(plugin));
   if (first_repeats(plugin))
     for (prev = &(((ar_plugin_data*) plugin->data)->repeats); *prev; prev = &(*prev)->next) {
         if ((*prev)->key == key) {
               key_repeat_info* tmp;
               tmp = (*prev)->next;
               free(*prev);
               *prev = tmp;
               return;
           }
       };
   {
       XkbSrvInfoPtr xkbi = plugin->device->key->xkbInfo;
       XkbDescPtr xkb = xkbi->desc;
       if (BitIsOn(xkb->ctrls->per_key_repeat, key))
           ErrorF("%s%s: bug. the key %d was not found, yet attempted to"
                  " remove from auto-repeat list%s\n",
                  repeat_color, __FUNCTION__, key, color_reset);
   }
}



#if DEBUG_AUTOREPEAT
static int
ar_count_events(key_repeat_info *repeats)
{
    int i = 0;
    key_repeat_info* cursor;
    for (cursor = repeats; cursor; cursor = cursor->next) {
        ErrorF("%s %d: %d  @ %" TIME_FORMAT "\n", __FUNCTION__, i, cursor->key, cursor->time);
        i++;
    }
    return i;
}
#endif


static void
ar_set_wakeup(PluginInstance *plugin)
{
    /* Calculate the wake-up time: */
    /* set timer. */
    key_repeat_info* repeats = first_repeats(plugin);

    if (repeats)
    {
        plugin->wakeup_time = repeats->time;
    } else
        plugin->wakeup_time = 0;
}


/* `Finish' a batch/turn of events. This means preparing for sleep & `Wake'! */
static void
ar_finish(PluginInstance* plugin)
{
    /* we copy the same. We can keep an event, but then the ->next is surely frozen,
       hence we are too. */
    plugin->frozen = plugin->next->frozen;

    if (plugin_frozen(plugin)) {
        /* mmc: We might accept an event if we don't keep one, but there is no use for it? */
        plugin->wakeup_time = 0;
    } else {
        ar_set_wakeup(plugin);
    }
}


static void
ar_process_key_event(PluginInstance* plugin,
                     InternalEvent*	event,
                     Bool owner)
{
   PluginInstance* next = plugin->next;
   DeviceIntPtr keybd = plugin->device;
   XkbSrvInfoPtr xkbi =  keybd->key->xkbInfo;
   XkbDescPtr xkb = xkbi->desc;


   /* we might check against repeated, to verify the DDX does not repeat itself */
   /* produce all pre-events (until possibly frozen) */

#if 0
   if ((event->any.type == ET_KeyPress)
       || (event->any.type == ET_KeyRelease))
#endif
   ar_push_events(plugin, event->any.time);

   /* I don't insist on first playing the event, and only then inserting into the repeats. */

   if (event->any.type == ET_KeyPress) {
          KeyCode key = event->device_event.detail.key;

          if (BitIsOn(xkb->ctrls->per_key_repeat,
                      /*keybd->kbdfeed->ctrl.autoRepeats*/
                      key))
              ar_start_keycode(plugin, key, event->any.time);
      } else if (event->any.type == ET_KeyRelease) {
          KeyCode key = event->device_event.detail.key;
         /* the bit might have been changed. */
          /* bug! this can change! */
          if (BitIsOn(xkb->ctrls->per_key_repeat,key)) /* keybd->kbdfeed->ctrl.autoRepeats */
            ar_cancel_key(plugin, key);
      }

   if (!plugin_frozen(plugin->next)) {
      /* pass-through */
      PluginClass(next)->ProcessEvent(next, event, owner);
      /* note, that at this poin we don't own for sure. */
   } else {
       if (((ar_plugin_data*) plugin->data)->event) {
           ErrorF("%s: dropping event. AR is already holding one for the frozen next\n",
                  __FUNCTION__);
       } else {
           InternalEvent *ev = (InternalEvent*)malloc(event->any.length);
           //assert (! ((ar_plugin_data*) plugin->data)->event);
           ((ar_plugin_data*) plugin->data)->event = ev;
           memcpy (ev, event, event->any.length);
       }
       if (owner)
           free(event);
   }
   ar_finish(plugin);
}


static void
ar_accept_time(PluginInstance* plugin, Time time)
{
    // bug! assert ( ((ar_plugin_data*) plugin->data)->event != (xEvent) 0);

    /* we still have to push our kept event -> we are frozen,
     * and hence don't accept time */

    /* when could it be frozen? actively grabbed. we don't get any signal
     * about such events! */
    if (! plugin_frozen(plugin->next)) {
        ar_push_events(plugin, time);
    }
    ar_finish(plugin);
}


static void
ar_thaw(PluginInstance* plugin, Time now)
{
    ar_plugin_data* ar = (ar_plugin_data*)plugin->data;
    assert (!plugin_frozen(plugin->next));

    /* todo: get rid of this */
    if (ar->event) {
        ar_push_events(plugin, (ar->event->any.time));

        if (!plugin_frozen(plugin->next)) {
            /* send it! */
            PluginClass(plugin->next)->ProcessEvent(plugin->next,
                                                    ar->event,
                                                    FALSE);
            free (ar->event);
            ar->event = NULL;
        }
    }

    /* `now' is problematic: what time is that?
     * I think we have to ask the preceding plugin for the (current) time.*/
    if (now)
        ar_push_events(plugin, now);

    /* now resume normal handling, from the Top! So, set all data, and recurse. */
    ar_finish(plugin);

    if (!plugin_frozen(plugin) && PluginClass(plugin->prev)->NotifyThaw) {
        /* if !plugin->prev->NotifyThaw ... would be strange. */

        /* thaw the previous! */
        PluginClass(plugin->prev)->NotifyThaw(plugin->prev, now);
        /* I could move now to the time of our event. */
    }
}


static PluginInstance*
ar_init(DeviceIntPtr dev, DevicePluginRec* plugin_class)
{
    /* assert (device is keyboard) */
    PluginInstance* plugin = calloc(1, sizeof(PluginInstance));
    plugin->device = dev;
    plugin->frozen = FALSE;
    plugin->wakeup_time = 0;
    plugin->pclass = plugin_class;
    {
        ar_plugin_data* ar_data = calloc(1, sizeof(ar_plugin_data));
        ar_data->event = NULL;
        ar_data->repeats = NULL;
        plugin->data = ar_data;
    }
    return plugin;
}


/* this is the only variable `exported', right? */
DevicePluginRec ar_plugin =
{
   "xkb-auto-repeat",
   ar_init,
   ar_process_key_event,
   ar_accept_time,
   ar_thaw,
   NULL,NULL,                   /* config */
   NULL,
   NULL
};
