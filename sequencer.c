/**
* sequencer.c: Max external to sequence messages at specific transport times.
*
* Copyright 2014 Adam Florin
*/

#include "ext.h"
#include "ext_common.h"
#include "ext_obex.h"
#include "ext_time.h"
#include "ext_itm.h"
#include "ext_strings.h"
#include "ext_dictobj.h"

// External struct
// 
typedef struct _sequencer {
  t_object object;
  void *event_outlet;
  void *done_outlet;
  t_object *timer;
  t_linklist  *queue;
  t_symbol *name;
  t_symbol *critical_event_msg;
  t_symbol *important_event_msg;
  t_symbol *normal_event_msg;
  t_bool verbose;
  t_bool overrideNow;
} t_sequencer;

// Method headers
// 
t_sequencer *sequencer_new(t_symbol *s, long argc, t_atom *argv);
void sequencer_free(t_sequencer *x);
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s);
void sequencer_name(t_sequencer *x, t_symbol *name);
void sequencer_at(t_sequencer *x, t_symbol *msg, long argc, t_atom *argv);
void sequencer_schedule(t_sequencer *x);
void sequencer_stop(t_sequencer *x);
long sequencer_sort_list(void *left, void *right);
void sequencer_timer_callback(t_sequencer *x);
void sequencer_iterate(t_sequencer *x, t_bool output_now);
void sequencer_schedule_next(t_sequencer *x, double at_ticks);

// Grace period constants
// 
const double IMPORTANT_GRACE_PERIOD_TICKS = 5.0;
const double NORMAL_GRACE_PERIOD_TICKS = 50.0;

// External class
// 
static t_class *s_sequencer_class = NULL;

/**
* main: proto-init.
*/
int C74_EXPORT main(void) {
  t_class *c = class_new(
    "sequencer",
    (method)sequencer_new,
    (method)sequencer_free,
    sizeof(t_sequencer),
    (method)0L,
    A_GIMME,
    0);

  class_addmethod(c, (method)sequencer_assist, "assist", A_CANT, 0);
  class_addmethod(c, (method)sequencer_name, "name", A_SYM, 0);
  class_addmethod(c, (method)sequencer_at, "at", A_GIMME, 0);
  class_addmethod(c, (method)sequencer_schedule, "schedule", 0);
  class_addmethod(c, (method)sequencer_stop, "stop", 0);
 
  class_register(CLASS_BOX, c);

  s_sequencer_class = c;
  return 0;
}

/**
* Create external.
*/
t_sequencer *sequencer_new(t_symbol *s, long argc, t_atom *argv) {
  t_sequencer *x = (t_sequencer *)object_alloc(s_sequencer_class);
  t_itm *itm;
  
  // outlet
  x->done_outlet = bangout(x);
  x->event_outlet = listout(x);
  
  // time object (used to schedule events)
  x->timer = (t_object*) time_new(
    (t_object *)x,
    gensym("delaytime"),
    (method)sequencer_timer_callback,
    TIME_FLAGS_TICKSONLY | TIME_FLAGS_USECLOCK);

  // queue
  x->queue = linklist_new();

  // name
  x->name = NULL;

  // event levels: pre-generate symbols
  x->critical_event_msg = gensym("done");
  x->important_event_msg = gensym("midi");
  x->normal_event_msg = gensym("ui");

  // debug mode
  x->verbose = false;

  // set "now" override--if transport is stopped
  itm = (t_itm *)time_getitm(x->timer);
  x->overrideNow = (itm_getstate(itm) == 0);

  return x;
}

/**
* Destroy external.
*/
void sequencer_free(t_sequencer *x) {
  freeobject(x->timer);
  object_free(x->queue);
}

/**
* Hardcode user tooltip prompts.
*/
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s) {
  if (m == ASSIST_INLET) {
    switch (a) {
      case 0: sprintf(s, "Events, schedule, stop"); break;
    }
  } else {
    switch (a) {
      case 0: sprintf(s, "Event dispatch"); break;
      case 1: sprintf(s, "Bang on queue empty"); break;
    }
  }
}

/**
* Name instance for debugging purposes.
*/
void sequencer_name(t_sequencer *x, t_symbol *name) {
  if (x->verbose) object_post((t_object*)x, "Naming %s.", name->s_name);
  x->name = name;
}

/**
* 'at' input: Insert received event into queue at appropriate place.
*/
void sequencer_at(t_sequencer *x, t_symbol *msg, long argc, t_atom *argv) {
  t_max_err error;
  t_atomarray *event_atoms;
  t_atom_long queue_length;

  // Build event from copy of args (atom array)
  event_atoms = atomarray_new(0, 0L);
  error = atomarray_setatoms(event_atoms, argc, argv);
  if (error) {
    object_error((t_object *)x, "Error copying event message for %s. (%d)", x->name->s_name, error);
    return;
  }
  
  // push event onto queue
  queue_length = linklist_insert_sorted(x->queue, event_atoms, sequencer_sort_list);
  if (queue_length == -1) {
    object_error((t_object *)x, "Error adding event to queue for %s.", x->name->s_name);
    return;
  }

  if (x->verbose) object_post((t_object*)x, "Added event to %s. Queue size %d.", x->name->s_name, queue_length);
}

/**
* 'schedule' input: Set timer to fire for first event in queue.
* 
* Assume queue is sorted.
*/
void sequencer_schedule(t_sequencer *x) {
  sequencer_iterate(x, false);
}

/**
* 'stop' input: Stop timer, clear linked list.
*/
void sequencer_stop(t_sequencer *x) {
  time_stop(x->timer);
  linklist_clear(x->queue);
  x->overrideNow = true;
}

/**
* Given two events (atom arrays), determine which is greater,
* based on first value (i.e. "at" time in ticks).
*/
long sequencer_sort_list(void *left, void *right) {
  t_atomarray *left_event = (t_atomarray *)left;
  t_atomarray *right_event = (t_atomarray *)right;
  long left_event_length = 0;
  long right_event_length = 0;
  t_atom *left_event_message;
  t_atom *right_event_message;
  t_atom_float left_at;
  t_atom_float right_at;
  t_max_err error;

  error = atomarray_getatoms(left_event, &left_event_length, &left_event_message);
  error = atomarray_getatoms(right_event, &right_event_length, &right_event_message);

  left_at = atom_getfloat(left_event_message);
  right_at = atom_getfloat(right_event_message);

  return left_at < right_at;
}

/**
* Time object callback: Output all events in queue specified to fire at this time.
*
* If any future events remain in the queue, re-schedule them to fire at their time.
*/
void sequencer_timer_callback(t_sequencer *x) {
  // if (x->verbose) object_post((t_object*)x, "Callback fired in %s", x->name->s_name);

  sequencer_iterate(x, true);
}


/**
* Iterate through queue, scheduling next events, deleting past events, and outputting current ones.
*
* @param output_now - if true, output events. If false, just schedule first event.
*/
void sequencer_iterate(t_sequencer *x, t_bool output_now) {
  t_max_err error;
  t_itm *itm;
  double now_ticks;
  t_atomarray *event_atoms;
  t_atom *out_atoms;
  long num_out_atoms;
  t_symbol *event_msg;
  t_atom_float event_at;
  t_atom_float event_from_now;
  t_bool event_on_schedule;
  t_atom_long deleted_index;
  double last_event_at = -1.0;

  // get current time
  itm = (t_itm *)time_getitm(x->timer);
  now_ticks = x->overrideNow ? 0.0 : itm_getticks(itm);

  // iterate through events
  while (linklist_getsize(x->queue) > 0) {

    // Pull first item off of queue
    event_atoms = (t_atomarray *)linklist_getindex(x->queue, 0);
    if (event_atoms == NULL) {
      object_error((t_object *)x, "No first event in queue for %s.", x->name->s_name);
      return;
    }

    // copy event atoms
    error = atomarray_getatoms(event_atoms, &num_out_atoms, &out_atoms);
    if (error) {
      object_error((t_object *)x, "Error copying event message for output for %s. (%d)", x->name->s_name, error);
      return;
    }

    // get event time
    event_at = atom_getfloat(out_atoms);
    event_from_now = event_at - now_ticks;

    // get event message to determine level
    event_msg = atom_getsym(out_atoms + 1);

    // check if event is on schedule--according to level
    event_on_schedule = (
      ((event_msg == x->critical_event_msg) && (event_at >= now_ticks)) ||
      ((event_msg == x->important_event_msg) && (event_at >= (now_ticks - IMPORTANT_GRACE_PERIOD_TICKS))) ||
      ((event_msg == x->normal_event_msg) && (event_at >= (now_ticks - NORMAL_GRACE_PERIOD_TICKS)))
    );

    // if (x->verbose) object_post((t_object*)x, "Event from %s claims to be at %f ticks.", x->name->s_name, event_at);

    if (!event_on_schedule && (event_msg != x->critical_event_msg)) {
      object_warn((t_object *)x, "MISSED EVENT FOR %s. Was supposed to be %f, now is %f", x->name->s_name, event_at, now_ticks);
    } else {
      // event is on schedule OR it is critical
      if (output_now) {
        // if this event is not at the same time as last event, schedule it for later.
        if (event_on_schedule && (last_event_at != -1.0) && (last_event_at != event_at)) {
          sequencer_schedule_next(x, event_from_now);
          break;
        }

        // output event message (minus first atom)
        if (num_out_atoms > 1) {
          outlet_list(x->event_outlet, 0L, num_out_atoms-1, out_atoms+1);
        }

        last_event_at = event_at;
      } else {
        // schedule next event
        if (event_on_schedule) {
          sequencer_schedule_next(x, event_from_now);
          break;
        } else if (event_msg == x->critical_event_msg) {
          // behind schedule, but this is a critical event--pass it along
          // output event message (minus first atom)
          if (num_out_atoms > 1) {
            outlet_list(x->event_outlet, 0L, num_out_atoms-1, out_atoms+1);
          }
        }
      }
    }

    // remove event from queue
    deleted_index = linklist_deleteindex(x->queue, 0);
    if (deleted_index == -1) {
      object_error((t_object *)x, "Error deleting first event in queue for %s.", x->name->s_name);
      return;
    }
  }

  // if queue is now empty, send bang out "done" outlet
  if (linklist_getsize(x->queue) == 0) {
    // if (x->verbose) object_post((t_object*)x, "Queue empty for %s. Send done event.", x->name->s_name);
    outlet_bang(x->done_outlet);
  }
}

/**
* Schedule timer at time in ticks.
*
* This method is used internally--not exposed as an input.
*/
void sequencer_schedule_next(t_sequencer *x, double at_ticks) {
  t_atom next_event_at_atom;
  t_max_err error;

  if (x->verbose) {
    object_post((t_object*)x, "Attempting to schedule timer for %s at %f.",
      x->name->s_name,
      at_ticks);
  }

  error = atom_setfloat(&next_event_at_atom, at_ticks);
  if (error) {
    object_error((t_object *)x, "Error scheduling timer for %s. (%d)", x->name->s_name, error);
    return;
  }
  time_setvalue(x->timer, NULL, 1, &next_event_at_atom);
  time_schedule(x->timer, NULL);

  // switch off override
  x->overrideNow = false;
}
