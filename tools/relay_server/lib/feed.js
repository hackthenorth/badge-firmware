// In-memory feed of recent box openings, for the live view at /.
//
// This is deliberately dumb: a module-level ring buffer. Vercel functions are
// stateless and can be spun up as multiple instances, so an event recorded by
// one instance is invisible to another, and a cold start wipes the buffer.
// Good enough to watch the demo work; not a record of anything.
// If this ever needs to be reliable, back it with Vercel KV instead.

const MAX_EVENTS = 50;

let seq = 0;
const events = [];

export function record(entry) {
  events.push({ id: ++seq, at: Date.now(), ...entry });
  if (events.length > MAX_EVENTS) events.shift();
  return seq;
}

// Everything newer than `sinceId`. The client passes back the last id it saw,
// so a poll that catches nothing new costs almost nothing.
export function since(sinceId) {
  const n = Number(sinceId) || 0;
  return events.filter((e) => e.id > n);
}

export function latestId() {
  return seq;
}
