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
typedef struct sequencer {
  t_object d_obj;
  void *d_outlet;
  t_object *d_timeobj;
  t_symbol *dictionary_name;
  t_bool verbose;
} t_sequencer;

// Method headers
// 
void *sequencer_new(t_symbol *s, long argc, t_atom *argv);
void sequencer_free(t_sequencer *x);
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s);
void sequencer_stop(t_sequencer *x);
void sequencer_dictionary(t_sequencer *x, t_symbol *s);
void sequencer_tick(t_sequencer *x);
void sequencer_schedule(t_sequencer *x, double at_ticks);

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
  class_addmethod(c, (method)sequencer_dictionary, "dictionary", A_SYM, 0);
  class_addmethod(c, (method)sequencer_stop, "stop", 0);
 
  class_register(CLASS_BOX, c);

  s_sequencer_class = c;
  return 0;
}

/**
* Create external.
*
* TODO:OPT: generate 'at' and 'msg' symbols.
*/
void *sequencer_new(t_symbol *s, long argc, t_atom *argv) {
  t_sequencer *x = (t_sequencer *)object_alloc(s_sequencer_class);
  
  // outlet
  x->d_outlet = listout(x);
  
  // time object (used to schedule events)
  x->d_timeobj = (t_object*) time_new(
    (t_object *)x,
    gensym("delaytime"),
    (method)sequencer_tick,
    TIME_FLAGS_TICKSONLY | TIME_FLAGS_USECLOCK);

  // debug mode
  x->verbose = true;

  return x;
}

/**
* Destroy external.
*/
void sequencer_free(t_sequencer *x) {
  freeobject(x->d_timeobj);
}

/**
* Hardcode user tooltip prompts.
*/
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s) {
  if (m == ASSIST_INLET) {
    switch (a) {
      case 0: sprintf(s, "Sequence bang"); break;
    }
  }
  else {
    switch (a) {
      case 0: sprintf(s, "Bang at time"); break;
    }
  }
}

/**
* 'dictionary' input.
* 
* Given a dictionary in a specific format (passed by reference), queue up next event.
*
* Assume that:
* - dictionary is not empty
* - first entry is at key "0"
* - entry values are dictionaries which include an 'at' key
*   (transport time in ticks of event)
*
* @params
*   symbol - dictionary name
*/
void sequencer_dictionary(t_sequencer *x, t_symbol *s) {
  t_dictionary *events;
  long num_events = 0;
  t_symbol **event_keys = NULL;
  t_dictionary *event;
  t_max_err error;
  double at_ticks;

  // store dict name immediately
  x->dictionary_name = s;

  // access 'events' dict
  events = dictobj_findregistered_retain(x->dictionary_name);

  // sanity check
  if (!events) {
    object_error((t_object*)x, "unable to reference dictionary named %s", x->dictionary_name->s_name);
    return;
  }

  // load event keys
  error = dictionary_getkeys(events, &num_events, &event_keys);
  if (error) {
    object_error((t_object *)x, "Error loading events from '%s'. (%d)", x->dictionary_name->s_name, error);
    return;
  }
  
  if (num_events > 0) {
    // dictionary is not empty
    if (x->verbose) object_post((t_object*)x, "First key in received dict %s is '%s'.", x->dictionary_name->s_name, event_keys[0]->s_name);

    // read first event 'at'
    error = dictionary_getdictionary(events, event_keys[0], (t_object **)&event);
    if (error) {
      object_error((t_object *)x, "Error loading event from '%s'. (%d)", x->dictionary_name->s_name, error);
      return;
    }

    error = dictionary_getfloat(event, gensym("at"), &at_ticks);
    if (error) {
      object_error((t_object *)x, "Error loading event time from '%s'. (%d)", x->dictionary_name->s_name, error);
      return;
    }

    // schedule event
    sequencer_schedule(x, at_ticks);
  } else {
    // dictionary is empty
    sequencer_stop(x);
  }

  // release dict
  error = dictobj_release(events);
  if (error) {
    object_error((t_object *)x, "Error releasing dict from '%s'. (%d)", x->dictionary_name->s_name, error);
    return;
  }
}

/**
* 'stop' input: shut it down.
*
* TODO: clear Dict, too? Or will [js] be responsible for that?
*/
void sequencer_stop(t_sequencer *x) {
  time_stop(x->d_timeobj);
}

/**
* Time object callback.
*
* TODO: reset or anything?
*
* XTRA:TODO: sanity check on dict format
*/
void sequencer_tick(t_sequencer *x) {
  t_dictionary *events;
  t_symbol **event_keys = NULL;
  t_symbol *event_key;
  char *event_key_string;
  long num_events = 0;
  long event_index;
  t_dictionary *event;
  double event_at;
  double last_event_at = -1.0;
  long num_msg_atoms;
  t_atom *msg_atoms;
  t_max_err error;

  // get current time
  // itm = (t_itm *)itm_getglobal();
  // now_ticks = itm_getticks(itm);

  // return if transport is stopped
  // if (itm_getstate(itm) == 0) return;

  // load events
  events = dictobj_findregistered_retain(x->dictionary_name);

  // sanity check
  if (!events) {
    object_error((t_object*)x, "unable to reference dictionary named %s", x->dictionary_name->s_name);
    return;
  }

  // load event keys
  error = dictionary_getkeys(events, &num_events, &event_keys);
  if (error) {
    object_error((t_object *)x, "Error loading events from '%s'. (%d)", x->dictionary_name->s_name, error);
    return;
  }

  // iterate through events
  for (event_index = 0; event_index < num_events; event_index++) {
    event_key = event_keys[event_index];

    // For NO apparent reason, THIS line makes everything work
    event_key_string = event_key->s_name;

    // load event
    error = dictionary_getdictionary(events, event_key, (t_object **)&event);
    if (error) {
      object_error((t_object *)x, "Error loading event from '%s'. (%d)", x->dictionary_name->s_name, error);
      break;
    }

    // Load event time ('at')
    error = dictionary_getfloat(event, gensym("at"), &event_at);
    if (error) {
      object_error((t_object *)x, "Error loading event time from '%s'. (%d)", x->dictionary_name->s_name, error);
      return;
    }

    // Test even time against last event.
    // If this event is not at the same time, break,
    // and reschedule time object to fire at event time.
    if ((last_event_at != -1.0) && (last_event_at != event_at)) {
      sequencer_schedule(x, event_at);
      break;
    }
    last_event_at = event_at;

    // load event message
    error = dictionary_getatoms(event, gensym("msg"), &num_msg_atoms, &msg_atoms);
    if (error) {
      object_error((t_object *)x, "Error loading event message from '%s'. (%d)", x->dictionary_name->s_name, error);
      return;
    }

    // output event message
    outlet_list(x->d_outlet, 0L, num_msg_atoms, msg_atoms);

    // delete event
    error = dictionary_deleteentry(events, event_key);
    if (error) {
      object_error((t_object *)x, "Error deleting event from '%s'. (%d)", x->dictionary_name->s_name, error);
      break;
    }
  }

  // free memory
  if (event_keys) dictionary_freekeys(events, num_events, event_keys);
  error = dictobj_release(events);
  if (error) {
    object_error((t_object *)x, "Error releasing events from '%s'. (%d)", x->dictionary_name->s_name, error);
    return;
  }
}

/**
* Schedule timer at time in ticks.
*
* This method is used internally--not exposed as an input.
*/
void sequencer_schedule(t_sequencer *x, double at_ticks) {
  t_itm *itm;
  double now_ticks;
  t_atom next_event_at_atom;
  t_max_err error;

  // get current time
  itm = (t_itm *)itm_getglobal();
  now_ticks = itm_getticks(itm);

  if (x->verbose) object_post((t_object*)x, "Scheduling %s for %f (now: %f).", x->dictionary_name->s_name, at_ticks, now_ticks);

  if (at_ticks >= now_ticks) {
    error = atom_setfloat(&next_event_at_atom, at_ticks - now_ticks);
    if (error) {
      object_error((t_object *)x, "Error scheduling timer for '%s'. (%d)", x->dictionary_name->s_name, error);
      return;
    }
    time_setvalue(x->d_timeobj, NULL, 1, &next_event_at_atom);
    time_schedule(x->d_timeobj, NULL);
  }
}
