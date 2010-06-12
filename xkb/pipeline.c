/* (c) 2004-2010 Michal Maruska <mmaruska@gmail.com>
 * 
 * Pipeline invocation
 */

/* Note: there should not be any freezing plugin at the beginning of the pipeline */

#include "pipeline.h"

/* `PLUGIN_API' ... means the function needs to be exported, so that plugins can use it! */
/* `MODULE_API' ....This is for modules (with a plugin). Add it to exported! */



/* mmc: is it still needed now that I use the X loader? */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>

#include "inputstr.h"
#include <events.h>
#include <eventstr.h>



#define DEBUG_timeout 0         /* block handler */
#define DEBUG 0

#define	XKBSRV_NEED_FILE_FUNCS
#define XKB_IN_SERVER 1
#include <xkbsrv.h>
#include "xkb.h"



/***  Advance declarations:  */
void xkb_pipeline_init(DeviceIntPtr pXDev);

static void start_plugin_pipeline(InternalEvent *event, DeviceIntPtr keybd);
static void thaw_pipeline_end(DeviceIntPtr keybd);
static void set_timeout(DeviceIntPtr keybd, struct timeval** wt, pointer mask, Time now);
static void push_time_on_keyboard(DeviceIntPtr keybd, Time upper_bound);


/* fixme: in pipeline.h?  */
extern DevicePluginRec queue_plugin;
extern DevicePluginRec core_plugin;
extern DevicePluginRec ar_plugin;

static Bool insert_plugin_around (DeviceIntPtr keybd, PluginInstance* plugin,
				  const char* name_of_precedent, Bool before);


/* Entry from xkbInit.c */
void
xkb_init_pipeline(DeviceIntPtr device)
{
    PluginInstance* plugin;

    assert(!device->pipeline);
    ErrorF("%s: configuring the pipe-line for ... %s\n", __FUNCTION__, device->name);

    /* let's build this pipeline:
     *
     *       queue -> AR -> core
     */
    device->pipeline = core_plugin.instantiate(device, &core_plugin);
    device->pipeline->id = device->pipeline_counter = 0;

    /* todo: Every time we instantiate, we have to increase the global counter! */

    plugin = queue_plugin.instantiate(device, &queue_plugin);
    plugin->id = ++(device->pipeline_counter);
    insert_plugin_around(device,
			 plugin,
			 "core", INSERT_BEFORE);

    plugin = ar_plugin.instantiate(device, &ar_plugin);
    plugin->id = ++(device->pipeline_counter);
    insert_plugin_around(device,
			 plugin,
			 "core", INSERT_BEFORE);

    /* now we have to re-route the processing: this overwrites values set in ../dix/devices.c
       in _AddInputDevice */
    device->public.processInputProc = device->public.enqueueInputProc =
	device->public.realInputProc = start_plugin_pipeline;
    device->public.thawProc = thaw_pipeline_end;
    device->public.pushTimeProc = push_time_on_keyboard;


    /* we need to interrupt the Select system call at the time requested by the first processor.
     * WaitForSomething consults only
     * timers, and blockHandlers to set such timeout. So i register one! */
    RegisterTimeBlockAndWakeupHandlers((TimeBlockHandlerProcPtr) set_timeout,
				       (TimeWakeupHandlerProcPtr) NoopDDA, device);
    ErrorF("...block handler set_timeout registered, %s exits (ok)\n", __FUNCTION__);

    // device->last_upper_bound = device->time = GetTimeInMillis();
}



static void
push_time_on_keyboard(DeviceIntPtr keybd, Time upper_bound)
{
    /* assert (keybd->pipeline);*/
    PluginInstance* plugin = keybd->pipeline;

    if (keybd->time < upper_bound) { /* no event  */
	if (plugin->wakeup_time <= upper_bound) /* recent */
            PluginClass(plugin)->ProcessTime (plugin, upper_bound);
	else {
#if DEBUG_timeout
	    ErrorF("%s: time is insufficient. wait %u, b/c the plugin wants %u, "
		   "we have (only) %u as upper bound of inactivity\n",
		   __FUNCTION__,
		   plugin->wakeup_time - upper_bound,
		   plugin->wakeup_time, upper_bound);
#endif
	}
    }
    /* ErrorF("%s: no need to push. Some event was processed this time\n",
     * __FUNCTION__); */
}



/* this `blockHandler' looks at the pipeline's first processor to see what other
   time point in future is significant, so
 * the caller, WaitForSomething, sets such timeout in the Select system call.
 * the time is returned in the WTP. */

/* wtp can be NULL. Hence I keep `static' waittime! */
static void
set_timeout(DeviceIntPtr keybd, struct timeval** wtp, pointer mask, Time now)
{
    struct timeval* wt = *wtp;

    Time timeout;
    static struct timeval waittime;     /* (signed)  longs! */

    if (keybd->pipeline->wakeup_time) {
	/* fixme: This should be redesigned in os/WaitFor.c */
	if (wt == NULL)
	    *wtp = wt = &waittime;

	/* unsigned */
	timeout = (keybd->pipeline->wakeup_time <= now)? 0 :
            keybd->pipeline->wakeup_time - now;

#if DEBUG_timeout
        ErrorF ("%s: current time is %lu, pipeline needs a wakeup at %lu\n", __FUNCTION__,
                now, keybd->pipeline->wakeup_time);
        ErrorF ("%s: so, our timeout %lu,  previously was %ld : %ld\n", __FUNCTION__,
                timeout, wt->tv_sec, wt->tv_usec);
#endif

	/* copied from os/WaitFor.c */
	waittime.tv_sec = timeout / MILLI_PER_SECOND;
	waittime.tv_usec = (timeout % MILLI_PER_SECOND) *
	    (1000000 / MILLI_PER_SECOND);

#if DEBUG_timeout
        /* ErrorF ("mmc: timeout decomposition: %lu -> %ld : %ld\n", timeout,
           waittime.tv_sec, waittime.tv_usec); */
        ErrorF ("%s: our %ld, global %ld (seconds) : diff: %ld\n", __FUNCTION__,
                waittime.tv_sec,
                wt->tv_sec,
                waittime.tv_sec -  wt->tv_sec);
#endif
	if (wt->tv_sec > waittime.tv_sec) {
#if DEBUG_timeout
            ErrorF ("%s: hence correcting the timeout!\n", __FUNCTION__);
#endif
	    wt->tv_sec = waittime.tv_sec;
	    wt->tv_usec = waittime.tv_usec;
	} else if ((wt->tv_sec == waittime.tv_sec)
                   && (wt->tv_usec > waittime.tv_usec)) {
#if DEBUG_timeout
            ErrorF ("%s: correcting!\n", __FUNCTION__);
#endif
	    wt->tv_usec = waittime.tv_usec;
	}
    }
}



/*
 * given plugins/processors A and B, such that:
 *
 * and the pipeline:  .c <--> A <--> d......
 *
 * a<->b<->c or  d---b<--->a---c
 *
 * fixme: plugins should be told about a modification of the neighbour?
 */

static Bool
insert_plugin_around (DeviceIntPtr keybd, PluginInstance* plugin,
		      const char* name_of_precedent, Bool before)
{
    PluginInstance* prev;
    // bug!    assert (plugin->dev == keybd);

    ErrorF ("%s: %s %s %s\n", __FUNCTION__, PLUGIN_NAME(plugin), before?"before":"after",
            name_of_precedent);
    /* find it... */
    assert (keybd->pipeline);


    prev = keybd->pipeline;
    while (prev && strcmp(PLUGIN_NAME(prev), name_of_precedent)) /* != 0*/
	prev = prev->next;

    if (prev) {
        ErrorF ("%s: inserting %s %s a new plugin %s\n", __FUNCTION__,
                before?"before":"after", PLUGIN_NAME(prev), PLUGIN_NAME(plugin));
        switch (before) {
	    case INSERT_BEFORE:
	    {
		PluginInstance* prev_prev;
		prev_prev = prev->prev;

		/*name_of_precedent */
		/* need a lock? */
		plugin->next = prev;
		prev->prev = plugin;

		if (prev_prev) {
		    prev_prev->next = plugin;
		    plugin->prev = prev_prev;
		} else {
                    ErrorF ("%s: %s is now the first plugin\n", __FUNCTION__, PLUGIN_NAME(plugin));
		    keybd->pipeline = plugin;
		    plugin->prev = NULL;
		}
		break;
	    }
	    case INSERT_AFTER:
	    {
		/*name_of_precedent */
		/* need a lock? */
		plugin->next = prev->next;
		prev->next = plugin;

		plugin->prev = prev;

		if (plugin->next)
		    plugin->next->prev = plugin;

		break;
	    }
	}; /* switch */
    } else {
	ErrorF ("%s: %s not found!\n", __FUNCTION__, name_of_precedent);
    }
    return (prev != NULL);
}

const char* event_names[] = {
    "KeyPress",
    "KeyRelease",
    "ButtonPress",
    "ButtonRelease",
    "Motion",
    "Enter",
    "Leave",
    "FocusIn",
    "FocusOut",
    "ProximityIn",
    "ProximityOut",
    "DeviceChanged",
    "Hierarchy",
    "DGAEvent",
    "RawKeyPress",
    "RawKeyRelease",
    "RawButtonPress",
    "RawButtonRelease",
    "RawMotion",
    "XQuartz"
};


/* this is the interface to the beginning of old key-event processing:
 * just when we take an event from xf86EventQueue */
static void
start_plugin_pipeline(InternalEvent *event, DeviceIntPtr keybd)
{
    PluginInstance* plugin = keybd->pipeline;
    ErrorF("%s: %s %s\n", __FUNCTION__, keybd->name, event_names[event->any.type - 2 ]);
#if 0
    if (keybd->last_upper_bound > event->u.keyButtonPointer.time)
	ErrorF("%s: processing event coming from time %d before last push %d!\n", __FUNCTION__,
	       (int) event->u.keyButtonPointer.time,
	       (int) keybd->last_upper_bound);

#if DEBUG_PIPELINE
    if (plugin)
	ErrorF("%s: beginning through %s%s. %s\n", __FUNCTION__,
	       PLUGIN_NAME(plugin), (plugin_frozen(plugin))?" frozen!":"",
	       (event->u.u.type==KeyRelease)?"release":"press");
#endif
#endif

    if (plugin && PluginClass(plugin)->ProcessEvent && !plugin_frozen(plugin))
	/* the event is still owned by us. */
	PluginClass(plugin)->ProcessEvent(plugin, event, CALLER_OWNS);
    /* neither we are the owner. it's in a static array! */
    else {
	assert (!plugin);
	ErrorF("%s: no keyboard pipeline processor available for events. Dropping them!\n",
	       __FUNCTION__);
      }
#if  DEBUG_PIPELINE
    ErrorF("---------------- end of pipeline activity (from a key event)\n");
#endif
}


/* interfaces the end of pipeline to the core key-event processing: AllowSome */
static void
thaw_pipeline_end(DeviceIntPtr keybd) /* CARD32 time */
{
  PluginInstance* plugin = keybd->pipeline;
#if  DEBUG_PIPELINE
   ErrorF("/------ A client-caused thaw: \n%s:\n", __FUNCTION__);
#endif
   assert (keybd->pipeline);

   /* go the the end of chain, and thaw it? */
   while (plugin->next)
      plugin = plugin->next;

   assert (PluginClass(plugin)->NotifyThaw);
   PluginClass(plugin)->NotifyThaw(plugin, 0); /* fixme: time! */
#if  DEBUG_PIPELINE
   ErrorF("_//////  ends %s:\n", __FUNCTION__);
#endif
}


#include "pipeline-proc.c"

void
pipeline_init_plugins(void)
{
    plugin_store = NULL;
}
