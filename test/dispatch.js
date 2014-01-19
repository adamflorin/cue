var BEAT = 480.0;

var eventsA = [
  {at: 0.0*BEAT, msg: ["notei", 70]},
  {at: 8.0*BEAT, msg: ["notei", 70]}
];

var eventsB = [
  {at: 4.0*BEAT, msg: ["notei", 62]},
  {at: 12.0*BEAT, msg: ["notei", 62]}
];

/**
* Set up event dictionary and output to [sequencer].
*/
function a() {
  sortem(eventsA);
}

/**
* Set up event dictionary and output to [sequencer].
*/
function b() {
  sortem(eventsB);
}

/**
* Make a new array of existing event Dicts (from events Dict) and
* new event Dicts (created from event objects); sort and output.
*/
function sortem(events) {
  var eventsDict = new Dict("events01");
  var eventDicts = [];

  // Create Dicts from all incoming events, store in array
  for (index in events) {
    var event = events[index];
    var eventDict = new Dict;
    eventDict.set("at", event.at);
    eventDict.set("msg", event.msg);
    eventDicts.push(eventDict);
  }

  // Pull existing events out of Dict, store in array.
  var keys = normalizedKeys(eventsDict);
  for (keyIndex in keys) {
    var key = keys[keyIndex];
    // TODO: sanity check: does key exist?
    var eventDict = eventsDict.get(key);
    // TODO: sanity check: is item in fact a dict?
    eventDicts.push(eventDict);
  }

  // Sort old and new events together
  eventDicts.sort(function(x, y) {
    return x.get("at") - y.get("at");
  });

  // Wipe Dict clean so that re-ordering "takes".
  eventsDict.clear();

  // Push back onto events Dict.
  for (index in eventDicts) {
    var eventDict = eventDicts[index];
    post("+ PUSH dict at " + eventDict.get("at") + " to index " + index + "\n");
    eventsDict.set(index, eventDicts[index]);
  }

  // notify [sequencer]
  outlet(0, ["dictionary", "events01"]);
}

/**
*
*/
function normalizedKeys(dict) {
  var keys = dict.getkeys();
  if (keys == null) keys = [];
  if (!(keys instanceof Array)) keys = [keys];
  return keys;
};
