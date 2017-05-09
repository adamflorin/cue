/**
* cue.c: Cue Max messages at specified transport times
*
* Copyright 2014-2017 Adam Florin
*/

#include "ext.h"
#include "ext_common.h"
#include "ext_obex.h"
#include "ext_time.h"
#include "ext_itm.h"
#include "ext_strings.h"
#include "ext_dictobj.h"
#include <math.h>

#define MAX_EXPIRATIONS_LENGTH 8

// External struct
//
typedef struct _cue {
  t_object object;
  void *event_outlet;
  void *scrub_outlet;
  t_object *timer;
  t_linklist  *queue;
  double expected_at_ticks;

  // attributes
  t_atom expirations[MAX_EXPIRATIONS_LENGTH];
  long expirations_length;
  t_dictionary *expirations_dictionary;
  t_symbol *name;
  char verbose;
} t_cue;

// Method headers
//
t_cue *cue_new(t_symbol *s, long argc, t_atom *argv);
void cue_free(t_cue *x);
void cue_assist(t_cue *x, void *b, long m, long a, char *s);
void cue_set_expirations(t_cue *x, void *attr, long argc, t_atom *argv);
void cue_parse_expirations(t_cue *x);
void cue_at(t_cue *x, t_symbol *msg, long argc, t_atom *argv);
void cue_cue(t_cue *x, t_symbol *msg, long argc, t_atom *argv);
void cue_clear(t_cue *x);
long cue_sort_list(void *left, void *right);
void cue_scrub_event(t_object *event_raw, double *delta);
void cue_timer_callback(t_cue *x);
void cue_iterate(t_cue *x, t_bool output_now);
void cue_schedule_next(t_cue *x, double desired_ticks, double now_ticks);

// External class
//
static t_class *s_cue_class = NULL;

static double const TICKS_PER_BEAT = 480.0;

/**
* main: proto-init.
*/
int C74_EXPORT main(void) {
  t_class *c = class_new(
    "cue",
    (method)cue_new,
    (method)cue_free,
    sizeof(t_cue),
    (method)0L,
    A_GIMME,
    0);

  // messages
  class_addmethod(c, (method)cue_assist, "assist", A_CANT, 0);
  class_addmethod(c, (method)cue_at, "at", A_GIMME, 0);
  class_addmethod(c, (method)cue_cue, "cue", A_GIMME, 0);
  class_addmethod(c, (method)cue_clear, "clear", 0);

  // attributes
  CLASS_ATTR_ATOM_VARSIZE(c, "expirations", ATTR_FLAGS_NONE, t_cue, expirations, expirations_length, MAX_EXPIRATIONS_LENGTH);
  CLASS_ATTR_ACCESSORS(c, "expirations", NULL, cue_set_expirations);
  CLASS_ATTR_SAVE(c, "expirations", ATTR_FLAGS_NONE);
  CLASS_ATTR_SYM(c, "name", ATTR_FLAGS_NONE, t_cue, name);
  CLASS_ATTR_CHAR(c, "verbose", 0, t_cue, verbose);
  CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Verbose");

  class_register(CLASS_BOX, c);

  s_cue_class = c;
  return 0;
}

/**
* Create external.
*/
t_cue *cue_new(t_symbol *s, long argc, t_atom *argv) {
  t_cue *x = (t_cue *)object_alloc(s_cue_class);

  if (!x) {
    return x;
  }

  // outlet
  x->scrub_outlet = floatout(x);
  x->event_outlet = listout(x);

  // time object (used to cue events)
  x->timer = (t_object*) time_new(
    (t_object *)x,
    gensym("delaytime"),
    (method)cue_timer_callback,
    TIME_FLAGS_TICKSONLY | TIME_FLAGS_USECLOCK
  );

  // queue
  x->queue = linklist_new();

  // attributes
  x->expirations_dictionary = dictionary_new();
  x->name = gensym("unnamed");
  x->verbose = 0;

  x->expected_at_ticks = -1.0;

  // load attributes
  attr_args_process(x, argc, argv);

  return x;
}

/**
* Destroy external.
*/
void cue_free(t_cue *x) {
  freeobject(x->timer);
  object_free(x->queue);
  object_free(x->expirations_dictionary);
}

/**
* Hardcode user tooltip prompts.
*/
void cue_assist(t_cue *x, void *b, long m, long a, char *s) {
  if (m == ASSIST_INLET) {
    switch (a) {
      case 0: sprintf(s, "at, cue, clear"); break;
    }
  } else {
    switch (a) {
      case 0: sprintf(s, "Event dispatch"); break;
      case 1: sprintf(s, "Scrub delta in ticks"); break;
    }
  }
}

/**
* Override setter for @expirations attribute.
*/
void cue_set_expirations(t_cue *x, void *attr, long argc, t_atom *argv) {
  // set
  x->expirations_length = argc;
  for (long i = 0; i < argc; i++) {
    x->expirations[i] = argv[i];
  }

  // parse
  cue_parse_expirations(x);
}

/**
* Parse @expirations attribute and store in dictionary for lookup later.
* Attribute format consists of string/float pairs, e.g. `midi 5 ui 50`.
*/
void cue_parse_expirations(t_cue *x) {
  long i;
  t_max_err error;
  t_symbol *message;
  t_atom_float value;

  // clear dictionary
  error = dictionary_clear(x->expirations_dictionary);
  if (error) {
    object_error((t_object *)x, "Error clearing expirations dictionary. (%d)", error);
    return;
  }

  // parse attribute
  for (i = 0; i < x->expirations_length; i += 2) {
    message = atom_getsym(x->expirations + i);
    value = atom_getfloat(x->expirations + i + 1);

    if (message == gensym("")) {
      object_warn((t_object *)x, "Received invalid expiration: message isn't a string.");
      continue;
    }

    error = dictionary_appendfloat(x->expirations_dictionary, message, value);
    if (error) {
      object_error((t_object *)x, "Error entering expirations into dictionary. (%d)", error);
      return;
    }
  }
}

/**
* 'at' input: Insert received event into queue at appropriate place.
*/
void cue_at(t_cue *x, t_symbol *msg, long argc, t_atom *argv) {
  long first_argument_type;
  t_max_err error;
  t_atomarray *event_atoms;
  t_atom_long queue_length;

  // validate time
  first_argument_type = atom_gettype(argv);
  if ((first_argument_type != A_LONG) && (first_argument_type != A_FLOAT)) {
    object_error((t_object *)x, "Received invalid 'at' message: time is not a number.");
    return;
  }

  // validate message
  if (argc < 2) {
    object_error((t_object *)x, "Received invalid 'at' message: no message to cue.");
    return;
  }

  // Build event from copy of args (atom array)
  event_atoms = atomarray_new(0, 0L);
  error = atomarray_setatoms(event_atoms, argc, argv);
  if (error) {
    object_error((t_object *)x, "Error copying event message for %s. (%d)", x->name->s_name, error);
    return;
  }

  // push event onto queue
  queue_length = linklist_insert_sorted(x->queue, event_atoms, cue_sort_list);
  if (queue_length == -1) {
    object_error((t_object *)x, "Error adding event to queue for %s.", x->name->s_name);
    return;
  }

  if (x->verbose) object_post((t_object*)x, "Added event to %s. Queue size was %d.", x->name->s_name, queue_length);
}

/**
* 'cue' message: Set timer to fire for first event in queue.
*
* Assume queue is sorted.
*/
void cue_cue(t_cue *x, t_symbol *msg, long argc, t_atom *argv) {
  t_symbol *second_msg;

  // if 'at' message is included, process it first
  if (argc > 0) {
    second_msg = atom_getsym(argv);
    if (second_msg == gensym("at")) {
      cue_at(x, second_msg, argc - 1, argv + 1);
    }
  }

  cue_iterate(x, false);
}

/**
* 'clear' input: Stop timer, clear linked list.
*/
void cue_clear(t_cue *x) {
  time_stop(x->timer);
  linklist_clear(x->queue);
  x->expected_at_ticks = -1.0;
}

/**
* Given two events (atom arrays), determine which is greater,
* based on first value (i.e. "at" time in ticks).
*/
long cue_sort_list(void *left, void *right) {
  t_atomarray *left_event = (t_atomarray *)left;
  t_atomarray *right_event = (t_atomarray *)right;
  long left_event_length = 0;
  long right_event_length = 0;
  t_atom *left_event_atoms;
  t_atom *right_event_atoms;
  t_atom_float left_at;
  t_atom_float right_at;
  t_max_err error;

  error = atomarray_getatoms(left_event, &left_event_length, &left_event_atoms);
  error = atomarray_getatoms(right_event, &right_event_length, &right_event_atoms);

  left_at = atom_getfloat(left_event_atoms);
  right_at = atom_getfloat(right_event_atoms);

  return left_at < right_at;
}

/** Iterator to adjust 'at' time of event */
void cue_scrub_event(t_object *event_raw, double *delta) {
  long event_length = 0;
  t_atom *event_atoms;
  t_max_err error;

  error = atomarray_getatoms(
    (t_atomarray *)event_raw,
    &event_length,
    &event_atoms
  );
  error = atom_setfloat(
    event_atoms + 0,
    atom_getfloat(event_atoms + 0) + *delta
  );
}

/**
* Time object callback: Output all events in queue specified to fire at this time.
*
* If any future events remain in the queue, re-schedule them to fire at their time.
*/
void cue_timer_callback(t_cue *x) {
  // if (x->verbose) object_post((t_object*)x, "Callback fired in %s", x->name->s_name);

  cue_iterate(x, true);
}


/**
* Iterate through queue, scheduling next events, deleting past events, and outputting current ones.
*
* @param output_now - if true, output events. If false, just schedule first event.
*/
void cue_iterate(t_cue *x, t_bool output_now) {
  t_max_err error;
  t_itm *itm;
  double now_ticks;
  double now_ticks_ish;
  t_atomarray *event_atoms;
  t_atom *out_atoms;
  long num_out_atoms;
  t_symbol *event_msg;
  t_atom_float event_ticks;
  t_bool event_expires;
  double expiration_ticks = -1;
  t_bool event_on_schedule;
  double last_event_ticks = -1.0;
  t_atom_long deleted_index = -1;

  // get current time
  itm = (t_itm *)time_getitm(x->timer);
  now_ticks = itm_getticks(itm);
  now_ticks_ish = now_ticks - 0.00001;

  // if it's not the time we expected (due to user scrubbing),
  // must then adjust all queued events accordingly
  if (output_now && (x->expected_at_ticks != -1.0)) {
    double scrub_delta = now_ticks - x->expected_at_ticks;

    if (fabs(scrub_delta) > 0.000001 && (scrub_delta != -0.0)) {
      // quantize scrub delta
      double desired_ticks = ceil(now_ticks) + fmod(x->expected_at_ticks, 1.0);
      if (desired_ticks > now_ticks + 1.0) {
        desired_ticks -= 1.0;
      }
      scrub_delta = desired_ticks - x->expected_at_ticks;

      // scrub all events
      linklist_funall(x->queue, (method)cue_scrub_event, &scrub_delta);

      // now output scrub_delta in beats for the benefit of others
      outlet_float(x->scrub_outlet, scrub_delta / TICKS_PER_BEAT);

      // if desired start time is in the future, re-schedule this call
      if (desired_ticks > now_ticks) {
        cue_schedule_next(x, desired_ticks, now_ticks);
        return;
      }
    }
  }

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
    event_ticks = atom_getfloat(out_atoms);

    event_expires = false;

    // get event message
    event_msg = atom_getsym(out_atoms + 1);

    // look up expiration period
    if (dictionary_hasentry(x->expirations_dictionary, event_msg)) {
      event_expires = true;
      error = dictionary_getfloat(x->expirations_dictionary, event_msg, &expiration_ticks);
      if (error) {
        object_error((t_object *)x, "Error fetching expiration. (%d)", error);
        return;
      }
    }

    if (x->verbose) {
      object_post((t_object*)x, "Event does %s expire, after %f ms.", event_expires ? "" : "not", expiration_ticks);
    }

    // check if event is on schedule
    event_on_schedule = (
      (!event_expires && (event_ticks >= now_ticks_ish)) ||
      (event_expires && (event_ticks >= (now_ticks_ish - expiration_ticks)))
    );

    // if (x->verbose) object_post((t_object*)x, "Event from %s claims to be at %f ticks.", x->name->s_name, event_ticks);

    if (x->verbose && !event_on_schedule) {
      object_warn(
        (t_object *)x,
        "[%s] MISSED %s EVENT scheduled at %f but now it's %f",
        x->name->s_name, event_msg->s_name, event_ticks, now_ticks);
    }

    // check if event should be output now
    if (
      // outputting now and event is on schedule and is first in queue or is at same time as first in queue
      (event_on_schedule && output_now && ((last_event_ticks == -1.0) || (last_event_ticks == event_ticks))) ||
      // critical message is behind schedule (outputting now or not)
      (!event_on_schedule && !event_expires)
    ) {
      // remove event from queue first so it doesn't get recursively
      // re-triggered if it's a "done" event.
      // "chuck" it so that it can be used for output, and freed afterwards.
      error = linklist_chuckindex(x->queue, 0);
      if (error) {
        object_error((t_object *)x, "Error chucking first event in queue for %s.", x->name->s_name);
        return;
      }

      // output event
      if (num_out_atoms > 1) {
        outlet_anything(x->event_outlet, event_msg, (short)(num_out_atoms-2), out_atoms+2);
      }

      // free event
      error = object_free(event_atoms);
      if (error) {
        object_error((t_object *)x, "Error freeing event for %s. (%d)", x->name->s_name, error);
        return;
      }

      // store time so that subsequent events at the same time may be output immediately
      last_event_ticks = event_ticks;

    } else if (
      // outputting now and event is behind schedule
      output_now && !event_on_schedule
    ) {
      // throw event out and keep moving
      deleted_index = linklist_deleteindex(x->queue, 0);
      if (deleted_index == -1) {
        object_error((t_object *)x, "Error deleting first event in queue for %s.", x->name->s_name);
        return;
      }

    } else {
      // set timer to output event in the future
      cue_schedule_next(x, event_ticks, now_ticks);
      break;
    }
  }
}

/**
* Schedule timer at time in ticks.
*
* This method is used internally--not exposed as an input.
*/
void cue_schedule_next(t_cue *x, double desired_ticks, double now_ticks) {
  t_atom next_event_at_atom;
  t_max_err error;

  // store expected callback time to compare with reality later
  // (in case user has scrubbed around)
  x->expected_at_ticks = desired_ticks;

  error = atom_setfloat(&next_event_at_atom, desired_ticks - now_ticks);
  if (error) {
    object_error((t_object *)x, "Error scheduling timer for %s. (%d)", x->name->s_name, error);
    return;
  }
  time_setvalue(x->timer, NULL, 1, &next_event_at_atom);
  time_schedule(x->timer, NULL);
}
