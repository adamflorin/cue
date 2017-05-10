/**
* cue.c: Cue Max messages to be dispatched at specified transport times
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
void cue_process_queue(t_cue *x, t_bool dispatching);
void cue_dispatch_first_event(
  t_cue *x,
  t_atomarray *event_atoms,
  long num_out_atoms,
  t_atom *out_atoms
);
void cue_delete_first_event(t_cue *x);
void cue_schedule_next(t_cue *x, double desired_ticks, double now_ticks);
t_bool cue_check_for_scrub(t_cue *x, double now_ticks);

// External class
static t_class *s_cue_class = NULL;

/** Initialize external class */
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
  CLASS_ATTR_ATOM_VARSIZE(
    c,
    "expirations",
    ATTR_FLAGS_NONE,
    t_cue,
    expirations,
    expirations_length,
    MAX_EXPIRATIONS_LENGTH
  );
  CLASS_ATTR_ACCESSORS(c, "expirations", NULL, cue_set_expirations);
  CLASS_ATTR_SYM(c, "name", ATTR_FLAGS_NONE, t_cue, name);
  CLASS_ATTR_CHAR(c, "verbose", 0, t_cue, verbose);
  CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Verbose");

  class_register(CLASS_BOX, c);

  s_cue_class = c;
  return 0;
}

/** Initialize external instance */
t_cue *cue_new(t_symbol *s, long argc, t_atom *argv) {
  t_cue *x = (t_cue *)object_alloc(s_cue_class);

  if (!x) {
    return x;
  }

  // outlets
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

  x->expected_at_ticks = -1.0;

  // attributes
  x->expirations_dictionary = dictionary_new();
  x->name = gensym("unnamed");
  x->verbose = 0;

  // load attributes
  attr_args_process(x, argc, argv);

  return x;
}

/** Destroy external instance */
void cue_free(t_cue *x) {
  freeobject(x->timer);
  object_free(x->queue);
  object_free(x->expirations_dictionary);
}

/** Configure user tooltip prompts */
void cue_assist(t_cue *x, void *b, long m, long a, char *s) {
  if (m == ASSIST_INLET) {
    switch (a) {
      case 0: sprintf(s, "Messages: at, cue, clear"); break;
    }
  } else {
    switch (a) {
      case 0: sprintf(s, "Event dispatch"); break;
      case 1: sprintf(s, "Scrub delta in ticks"); break;
    }
  }
}

/** Override setter for @expirations attribute */
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
    object_error(
      (t_object *)x,
      "Failed to clear expirations @ '%s' (%d)",
      x->name->s_name,
      error
    );
    return;
  }

  // parse attribute
  for (i = 0; i < x->expirations_length; i += 2) {
    message = atom_getsym(x->expirations + i);
    value = atom_getfloat(x->expirations + i + 1);

    if (message == gensym("")) {
      object_warn(
        (t_object *)x,
        "Received invalid expiration @ '%s': message isn't a string",
        x->name->s_name
      );
      continue;
    }

    error = dictionary_appendfloat(x->expirations_dictionary, message, value);
    if (error) {
      object_error(
        (t_object *)x,
        "Error entering expirations @ '%s' (%d)",
        x->name->s_name,
        error
      );
      return;
    }
  }
}

/** 'at' message: Insert received event into queue according to sort order */
void cue_at(t_cue *x, t_symbol *msg, long argc, t_atom *argv) {
  long first_argument_type;
  t_max_err error;
  t_atomarray *event_atoms;
  t_atom_long queue_length;

  // validate time
  first_argument_type = atom_gettype(argv);
  if ((first_argument_type != A_LONG) && (first_argument_type != A_FLOAT)) {
    object_error(
      (t_object *)x,
      "Received invalid 'at' message @ '%s': time is not a number",
      x->name->s_name
    );
    return;
  }

  // validate message
  if (argc < 2) {
    object_error(
      (t_object *)x,
      "Received invalid 'at' message @ '%s': no message to cue",
      x->name->s_name
    );
    return;
  }

  // Build event from copy of args (atom array)
  event_atoms = atomarray_new(0, 0L);
  error = atomarray_setatoms(event_atoms, argc, argv);
  if (error) {
    object_error(
      (t_object *)x,
      "Failed to build event @ '%s' (%d)",
      x->name->s_name,
      error
    );
    return;
  }

  // push event onto queue
  queue_length = linklist_insert_sorted(x->queue, event_atoms, cue_sort_list);
  if (queue_length == -1) {
    object_error(
      (t_object *)x,
      "Failed to push event to queue @ '%s'",
      x->name->s_name
    );
    return;
  }

  if (x->verbose) {
    object_post(
      (t_object*)x,
      "Added event to queue @ '%s' (new size: %d)",
      x->name->s_name,
      queue_length + 1
    );
  }
}

/** 'cue' message: Process queue, checking for 'at' message first */
void cue_cue(t_cue *x, t_symbol *msg, long argc, t_atom *argv) {
  t_symbol *second_msg;

  // if 'at' message is included, process it first
  if (argc > 0) {
    second_msg = atom_getsym(argv);
    if (second_msg == gensym("at")) {
      cue_at(x, second_msg, argc - 1, argv + 1);
    }
  }

  cue_process_queue(x, false);
}

/** 'clear' input: Stop timer, clear linked list. */
void cue_clear(t_cue *x) {
  if (x->verbose) {
    object_post((t_object*)x, "Clearing queue @ '%s'", x->name->s_name);
  }

  time_stop(x->timer);
  linklist_clear(x->queue);
  x->expected_at_ticks = -1.0;
}

/** linklist sort handler: Sort by 'at' time in ticks (first value in array) */
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

  error = atomarray_getatoms(
    left_event,
    &left_event_length,
    &left_event_atoms
  );
  error = atomarray_getatoms(
    right_event,
    &right_event_length,
    &right_event_atoms
  );

  left_at = atom_getfloat(left_event_atoms);
  right_at = atom_getfloat(right_event_atoms);

  return left_at < right_at;
}

/** linklist iterator: Offset 'at' time of event */
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

/** timer callback: Process queue */
void cue_timer_callback(t_cue *x) {
  cue_process_queue(x, true);
}

/**
* Process events queue--dispatching, deleting, or cueing events as needed.
*
* Iterate through each event in queue, assuming that it is sorted by 'at' time.
*
* If 'dispatching' is true, assume that the first event, and any subsequent
* events cued at the same time, are ready for dispatch. (This is the timer
* callback scenario.)
*
* Events may be subject to expiration times, as configured by the @expirations
* attribute. By default, events never expire, and will still be dispatched, even
* if they not on time. Expired events are deleted.
*
* If 'dispatching' is false, or all eligible events have been dispatched or
* deleted, then the next upcoming event (if any) is cued.
*/
void cue_process_queue(t_cue *x, t_bool dispatching) {
  t_max_err error;
  t_itm *itm;
  double now_ticks;
  double now_ticks_ish;
  t_atomarray *event_atoms;
  long num_out_atoms;
  t_atom *out_atoms;
  t_symbol *event_msg;
  t_atom_float event_ticks;
  t_bool event_expires;
  double expiration_ticks = 0.0;
  double last_event_ticks = -1.0;
  t_bool on_time;
  t_bool at_dispatch_time;

  // get current time
  itm = (t_itm *)time_getitm(x->timer);
  now_ticks = itm_getticks(itm);
  now_ticks_ish = now_ticks - 0.00001;

  if (x->verbose) {
    object_post(
      (t_object *)x,
      "Processing queue to %s at %.3f ticks @ '%s'",
      (dispatching ? "dispatch" : "cue"),
      now_ticks,
      x->name->s_name
    );
  }

  // Check for scrub
  if (dispatching) {
    t_bool scrub_detected = cue_check_for_scrub(x, now_ticks);
    if (scrub_detected) {
      return;
    }
  }

  // iterate through events
  while (linklist_getsize(x->queue) > 0) {
    // pull first item off of queue
    event_atoms = (t_atomarray *)linklist_getindex(x->queue, 0);
    if (event_atoms == NULL) {
      object_error(
        (t_object *)x,
        "Failed to get event @ '%s'",
        x->name->s_name
      );
      return;
    }

    // copy event atoms
    error = atomarray_getatoms(event_atoms, &num_out_atoms, &out_atoms);
    if (error) {
      object_error(
        (t_object *)x,
        "Failed to get event contents @ '%s' (%d)",
        x->name->s_name,
        error
      );
      return;
    }

    // get event time
    event_ticks = atom_getfloat(out_atoms);

    // get event message
    event_msg = atom_getsym(out_atoms + 1);

    // look up expiration period
    event_expires = false;
    expiration_ticks = 0.0;
    if (dictionary_hasentry(x->expirations_dictionary, event_msg)) {
      event_expires = true;
      error = dictionary_getfloat(
        x->expirations_dictionary,
        event_msg,
        &expiration_ticks
      );
      if (error) {
        object_error(
          (t_object *)x,
          "Failed to look up expiration @ '%s' (%d)",
          x->name->s_name,
          error
        );
        return;
      }
    }

    // event is "on schedule" if it is not later than the expiration window
    on_time = (event_ticks >= (now_ticks_ish - expiration_ticks));

    // event is in "dispatch group" if it is either the first in queue *or*
    // cued at the same time as the first in queue
    at_dispatch_time = (
      (last_event_ticks == -1.0) ||
      (last_event_ticks == event_ticks)
    );

    if (x->verbose) {
      object_post(
        (t_object*)x,
        "Found '%s' event cued at %.3f ticks "
        "(+%.3f expiration ticks / %s on time / %s at dispatch time) @ '%s'",
        event_msg->s_name,
        event_ticks,
        expiration_ticks,
        (on_time ? "IS" : "is NOT"),
        (at_dispatch_time ? "IS" : "is NOT"),
        x->name->s_name
      );

      if (!on_time) {
        object_warn(
          (t_object *)x,
          "'%s' event is %.3f ticks late @ '%s'",
          event_msg->s_name,
          now_ticks - event_ticks,
          x->name->s_name
        );
      }
    }

    // determine whether to dispatch, delete, or cue
    if (
      (on_time && at_dispatch_time && dispatching) ||
      (!on_time && !event_expires)
    ) {
      // dispatch event
      if (x->verbose) {
        object_post((t_object*)x, "Dispatching event @ '%s'", x->name->s_name);
      }

      cue_dispatch_first_event(x, event_atoms, num_out_atoms, out_atoms);

      // store time so that all events at same time are output at once
      last_event_ticks = event_ticks;

    } else if (!on_time) {
      // delete expired event
      if (x->verbose) {
        object_post((t_object*)x, "Deleting event @ '%s'", x->name->s_name);
      }

      cue_delete_first_event(x);

    } else {
      // cue upcoming event
      if (x->verbose) {
        object_post((t_object*)x, "Cueing event @ '%s'", x->name->s_name);
      }

      cue_schedule_next(x, event_ticks, now_ticks);

      break;
    }
  }
}

/** Dispatch first event in queue */
void cue_dispatch_first_event(
  t_cue *x,
  t_atomarray *event_atoms,
  long num_out_atoms,
  t_atom *out_atoms
) {
  t_max_err error;

  // remove event from queue first so it doesn't get recursively
  // re-triggered if it's a "done" event.
  // "chuck" it so that it can be used for output, and freed afterwards.
  error = linklist_chuckindex(x->queue, 0);
  if (error) {
    object_error(
      (t_object *)x,
      "Failed to chuck event @ '%s'",
      x->name->s_name
    );
    return;
  }

  // output event
  if (num_out_atoms > 1) {
    outlet_anything(
      x->event_outlet,
      atom_getsym(out_atoms + 1),
      (short)(num_out_atoms - 2),
      out_atoms + 2
    );
  }

  // free event
  error = object_free(event_atoms);
  if (error) {
    object_error(
      (t_object *)x,
      "Failed to free dispatched event @ '%s' (%d)",
      x->name->s_name,
      error
    );
  }
}

/** Delete event from queue, returning true upon success */
void cue_delete_first_event(t_cue *x) {
  t_atom_long deleted_index = -1;
  deleted_index = linklist_deleteindex(x->queue, 0);
  if (deleted_index == -1) {
    object_error(
      (t_object *)x,
      "Failed to delete event @ '%s'",
      x->name->s_name
    );
  }
}

/** Schedule timer to fire at desired_ticks */
void cue_schedule_next(t_cue *x, double desired_ticks, double now_ticks) {
  t_atom next_event_at_atom;
  t_max_err error;

  // store expected callback time to compare with reality later
  // (in case user has scrubbed around)
  x->expected_at_ticks = desired_ticks;

  error = atom_setfloat(&next_event_at_atom, desired_ticks - now_ticks);
  if (error) {
    object_error(
      (t_object *)x,
      "Failed to write timer value @ '%s' (%d)",
      x->name->s_name,
      error
    );
    return;
  }
  time_setvalue(x->timer, NULL, 1, &next_event_at_atom);
  time_schedule(x->timer, NULL);
}

/**
* Check now_ticks against expected_at_ticks to see if transport has been
* scrubbed (or looped). If so, offset queued events accordingly.
*
* @return true if scrub was dectected
*/
t_bool cue_check_for_scrub(t_cue *x, double now_ticks) {
  if (x->expected_at_ticks != -1.0) {
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

      if (x->verbose) {
        object_post(
          (t_object*)x,
          "Detected scrub of %.3f ticks @ '%s'",
          scrub_delta,
          x->name->s_name
        );
      }

      // now output scrub_delta in beats for the benefit of others
      outlet_float(x->scrub_outlet, scrub_delta);

      // if desired start time is in the future, re-schedule this call
      if (desired_ticks > now_ticks) {
        cue_schedule_next(x, desired_ticks, now_ticks);
        return true;
      }
    }
  }
  return false;
}
