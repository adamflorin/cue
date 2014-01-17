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

// 
// 
typedef struct sequencer
{
  t_object d_obj;
  void *d_outlet;
  t_object *d_timeobj;
  t_atomarray *storedAtoms;
} t_sequencer;

// 
// 
void *sequencer_new(t_symbol *s, long argc, t_atom *argv);
void sequencer_free(t_sequencer *x);
void sequencer_assist(t_sequencer *x, void *b, long m, long a, char *s);
void sequencer_list(t_sequencer *x, t_symbol *s, long argc, t_atom *argv);
void sequencer_anything(t_sequencer *x, t_symbol *msg, long argc, t_atom *argv);
void sequencer_bang(t_sequencer *x);
void sequencer_stop(t_sequencer *x);
void sequencer_tick(t_sequencer *x);

// 
// 
static t_class *s_sequencer_class = NULL;

/**
* main: proto-init.
*/
int C74_EXPORT main(void)
{
  t_class *c = class_new( "sequencer", (method)sequencer_new, (method)sequencer_free, sizeof(t_sequencer), (method)0L, A_GIMME, 0);

  class_addmethod(c, (method)sequencer_bang, "bang", 0);
  class_addmethod(c, (method)sequencer_stop, "stop", 0);
  class_addmethod(c, (method)sequencer_list, "list", A_GIMME, 0);
  class_addmethod(c, (method)sequencer_anything, "anything", A_GIMME, 0);
  class_addmethod(c, (method)sequencer_assist, "assist", A_CANT, 0);
 
  class_register(CLASS_BOX, c);

  s_sequencer_class = c;
  return 0;
}

/**
* Create external.
*/
void *sequencer_new(t_symbol *s, long argc, t_atom *argv)
{
  t_sequencer *x = (t_sequencer *)object_alloc(s_sequencer_class);
  long attrstart = attr_args_offset(argc, argv);
  t_atom a;
  
  // outlet
  x->d_outlet = listout(x);
  
  // time object (used to schedule events)
  x->d_timeobj = (t_object*) time_new((t_object *)x, gensym("delaytime"), (method)sequencer_tick, TIME_FLAGS_TICKSONLY | TIME_FLAGS_USECLOCK);

  // Set time object to fire at zero.
  atom_setfloat(&a, 0.);
  time_setvalue(x->d_timeobj, NULL, 1, &a);

  // Init atom storage
  x->storedAtoms = atomarray_new(0, 0L);

  return x;
}

/**
* Destroy external.
*/
void sequencer_free(t_sequencer *x) {
  freeobject(x->d_timeobj);
  object_free(x->storedAtoms);
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
* List input: Hand off to anything input.
*/
void sequencer_list(t_sequencer *x, t_symbol *s, long argc, t_atom *argv)
{
  sequencer_anything(x, NULL, argc, argv);
}

/**
* Anything input: Set internal internal atom array to input.
*
* Note: `msg` is discarded.
*/
void sequencer_anything(t_sequencer *x, t_symbol *msg, long argc, t_atom *argv)
{
  atomarray_setatoms(x->storedAtoms, argc, argv);
}

/**
* Bang input: schedule event at pre-defined time.
*/
void sequencer_bang(t_sequencer *x)
{
  time_schedule(x->d_timeobj, NULL);
}

/**
* 'stop' input: shut it down.
*/
void sequencer_stop(t_sequencer *x)
{
  time_stop(x->d_timeobj);
}

/**
* Time object callback.
*/
void sequencer_tick(t_sequencer *x)
{
  t_atom *outAtoms;
  long numOutAtoms;

  // populate outAtoms with stored atoms
  atomarray_getatoms(x->storedAtoms, &numOutAtoms, &outAtoms);
  outlet_list(x->d_outlet, 0L, numOutAtoms, outAtoms);
}
