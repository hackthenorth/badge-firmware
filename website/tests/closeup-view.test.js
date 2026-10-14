import assert from "node:assert/strict";
import test from "node:test";
import { constrainView, gestureView, overviewRect, viewMetrics, zoomView } from "../src/closeup-view.js";

const image = { width: 1000, height: 1600 };
const metrics = viewMetrics(image, { width: 648, height: 648 });
const fit = { zoom: 1, x: 0, y: 0 };
const near = (actual, expected) => assert.ok(Math.abs(actual - expected) < 1e-8, `${actual} should equal ${expected}`);

test("fit shows the whole portrait with space around it on phone and desktop", () => {
  for (const size of [{ width: 288, height: 360 }, { width: 1280, height: 850 }]) {
    const info = viewMetrics(image, size);
    assert.ok(image.width * info.fit <= size.width - 48);
    assert.ok(image.height * info.fit <= size.height - 48);
    assert.deepEqual(overviewRect(fit, info), { left: 0, top: 0, width: 1, height: 1 });
    near(info.maxZoom * info.fit, 4);
  }
});

test("zoom keeps the image point under the cursor stationary", () => {
  const before = { zoom: 2, x: 10, y: -20 };
  const cursor = { x: 20, y: 30 };
  const after = zoomView(before, 4, cursor, metrics);
  near((cursor.x - before.x) / before.zoom, (cursor.x - after.x) / after.zoom);
  near((cursor.y - before.y) / before.zoom, (cursor.y - after.y) / after.zoom);
});

test("zoom cannot go below fit or above the detail limit", () => {
  assert.deepEqual(zoomView({ zoom: 4, x: 200, y: 100 }, 0, { x: 50, y: 60 }, metrics), fit);
  assert.equal(zoomView(fit, 1e6, { x: 0, y: 0 }, metrics).zoom, metrics.maxZoom);
});

test("all zoom inputs stop at 400 percent on phones and desktops", () => {
  for (const size of [{ width: 288, height: 360 }, { width: 1280, height: 850 }]) {
    const info = viewMetrics(image, size);
    const wheelOrButton = zoomView(fit, 1e6, { x: 0, y: 0 }, info);
    const pinch = gestureView(fit, [{ x: 0, y: 0 }, { x: 1, y: 0 }], [{ x: 0, y: 0 }, { x: 1e6, y: 0 }], info);
    near(wheelOrButton.zoom * info.fit, 4);
    near(pinch.zoom * info.fit, 4);
  }
});

test("panning stops at image edges, keeping the minimap inside the photo", () => {
  for (const sign of [-1, 1]) {
    const result = constrainView({ zoom: 4, x: sign * 1e6, y: sign * 1e6 }, metrics);
    const rect = overviewRect(result, metrics);
    near(result.x, sign * (image.width * metrics.fit * 4 - metrics.width) / 2);
    near(result.y, sign * (image.height * metrics.fit * 4 - metrics.height) / 2);
    if (sign > 0) { near(rect.left, 0); near(rect.top, 0); }
    else { near(rect.left + rect.width, 1); near(rect.top + rect.height, 1); }
  }
});

test("a single pointer pans by its movement", () => {
  assert.deepEqual(gestureView({ zoom: 4, x: 0, y: 0 }, [{ x: 20, y: 30 }], [{ x: 40, y: -10 }], metrics), { zoom: 4, x: 20, y: -40 });
});

test("pinch zoom follows the moving midpoint of two fingers", () => {
  const result = gestureView({ zoom: 2, x: 0, y: 0 }, [{ x: -50, y: 0 }, { x: 50, y: 0 }], [{ x: -75, y: 20 }, { x: 125, y: 20 }], metrics);
  assert.deepEqual(result, { zoom: 4, x: 25, y: 20 });
});

test("actual size maps one source pixel to one CSS pixel", () => {
  const result = zoomView(fit, 1 / metrics.fit, { x: 0, y: 0 }, metrics);
  near(result.zoom * metrics.fit, 1);
});

test("resize clamps an existing pan and preserves zoom", () => {
  const resized = viewMetrics(image, { width: 1400, height: 648 });
  const result = constrainView({ zoom: 2, x: 50, y: 200 }, resized);
  assert.deepEqual(result, { zoom: 2, x: 0, y: 200 });
});
