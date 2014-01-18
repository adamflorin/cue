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
} t_sequencer;

// Method headers
// 
void *sequencer_new(t_symbol *s, long argc, t_atom *argv);
void sequencer_free(t_sequencer *x);
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s);
void sequencer_stop(t_sequencer *x);
void sequencer_dictionary(t_sequencer *x, t_symbol *s);
void sequencer_tick(t_sequencer *x);

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
  t_dictionary *event;
  t_max_err error;
  double at;
  t_atom at_atom;

  // store dict name immediately
  x->dictionary_name = s;

  // access 'events' dict
  events = dictobj_findregistered_retain(x->dictionary_name);

  // read first event 'at'
  error = dictionary_getdictionary(events, gensym("0"), (t_object **)&event);
  error = dictionary_getfloat(event, gensym("at"), &at);

  // schedule event
  atom_setfloat(&at_atom, at);
  time_setvalue(x->d_timeobj, NULL, 1, &at_atom);
  time_schedule(x->d_timeobj, NULL);

  // free memory
  dictobj_release(events);
}

/**
* 'stop' input: shut it down.
*/
void sequencer_stop(t_sequencer *x) {
  time_stop(x->d_timeobj);
}

/**
* Time object callback.
*
* TODO: delete event once dispatched?
* TODO: reschedule time object for time of next event.
*
* XTRA:TODO: sanity check on dict format
*/
void sequencer_tick(t_sequencer *x) {
  t_dictionary *events;
  t_symbol **event_keys = NULL;
  t_symbol *event_key;
  long num_events = 0;
  long event_index;
  t_dictionary *event;
  double event_at;
  double last_event_at = -1.0;
  long num_msg_atoms;
  t_atom *msg_atoms;
  t_max_err error;

  // load events
  events = dictobj_findregistered_retain(x->dictionary_name);

  // load event keys
  error = dictionary_getkeys(events, &num_events, &event_keys);

  // iterate through events
  for (event_index = 0; event_index < num_events; event_index++) {
    event_key = event_keys[event_index];

    // load event
    error = dictionary_getdictionary(events, event_key, (t_object **)&event);

    // Load event 'at', test against last event.
    // If this event is not at the same time, break.
    error = dictionary_getfloat(event, gensym("at"), &event_at);
    if ((last_event_at != -1.0) && (last_event_at != event_at)) {
      last_event_at = event_at;
      break;
    }
    last_event_at = event_at;

    // load event message
    error = dictionary_getatoms(event, gensym("msg"), &num_msg_atoms, &msg_atoms);

    // output event message
    outlet_list(x->d_outlet, 0L, num_msg_atoms, msg_atoms);

    // TODO: delete event once dispatched
    // error = dictionary_deleteentry (events, event_key);
  }

  // free memory
  if (event_keys) dictionary_freekeys(events, num_events, event_keys);
  dictobj_release(events);
}
