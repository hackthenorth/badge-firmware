export const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

export function viewMetrics(image, size) {
  const fit = Math.min(Math.max(1, size.width - 48) / image.width, Math.max(1, size.height - 48) / image.height);
  return { ...size, imageWidth: image.width, imageHeight: image.height, fit, maxZoom: Math.max(1, 4 / fit) };
}

export function constrainView(view, metrics) {
  const zoom = clamp(view.zoom, 1, metrics.maxZoom);
  const maxX = Math.max(0, (metrics.imageWidth * metrics.fit * zoom - metrics.width) / 2);
  const maxY = Math.max(0, (metrics.imageHeight * metrics.fit * zoom - metrics.height) / 2);
  return { zoom, x: clamp(view.x, -maxX, maxX), y: clamp(view.y, -maxY, maxY) };
}

// Points and offsets are measured from the viewport's centre.
export function zoomView(view, zoom, point, metrics) {
  const nextZoom = clamp(zoom, 1, metrics.maxZoom);
  const ratio = nextZoom / view.zoom;
  return constrainView({ zoom: nextZoom, x: point.x - (point.x - view.x) * ratio, y: point.y - (point.y - view.y) * ratio }, metrics);
}

export function gestureView(view, before, after, metrics) {
  if (before.length < 2) {
    return constrainView({ ...view, x: view.x + after[0].x - before[0].x, y: view.y + after[0].y - before[0].y }, metrics);
  }
  const midpoint = (points) => ({ x: (points[0].x + points[1].x) / 2, y: (points[0].y + points[1].y) / 2 });
  const distance = (points) => Math.hypot(points[1].x - points[0].x, points[1].y - points[0].y);
  const from = midpoint(before);
  const to = midpoint(after);
  const zoom = clamp(view.zoom * distance(after) / Math.max(1, distance(before)), 1, metrics.maxZoom);
  const ratio = zoom / view.zoom;
  return constrainView({ zoom, x: to.x - (from.x - view.x) * ratio, y: to.y - (from.y - view.y) * ratio }, metrics);
}

export function overviewRect(view, metrics) {
  const width = metrics.imageWidth * metrics.fit * view.zoom;
  const height = metrics.imageHeight * metrics.fit * view.zoom;
  const left = clamp((width / 2 - metrics.width / 2 - view.x) / width, 0, 1);
  const top = clamp((height / 2 - metrics.height / 2 - view.y) / height, 0, 1);
  return {
    left, top,
    width: clamp((width / 2 + metrics.width / 2 - view.x) / width, 0, 1) - left,
    height: clamp((height / 2 + metrics.height / 2 - view.y) / height, 0, 1) - top,
  };
}
