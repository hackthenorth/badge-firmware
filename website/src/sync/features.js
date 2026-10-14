// Build-time release switches, never credentials. Also checked by the serial
// methods so enabling a disabled button in DevTools cannot start a write.
export function releaseFeatures(env = {}) {
  return Object.freeze({
    unlock: env.DEV === true || env.VITE_POST_EVENT_UNLOCK === "true",
    identity: env.DEV === true || env.VITE_POST_EVENT_IDENTITY === "true",
  });
}
export const FEATURES = releaseFeatures(import.meta.env);
