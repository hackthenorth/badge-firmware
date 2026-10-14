import React, { useEffect, useMemo, useRef, useState } from "react";
import { clamp, constrainView, gestureView, overviewRect, viewMetrics, zoomView } from "./closeup-view.js";
import { loadCloseupPhoto } from "./closeup-cache.js";
import "./closeup.css";

// Upright dimensions. The original PNG pixels stay intact; the viewer rotates them clockwise.
const images = {
  front: { src: "/closeup/badge-front.7b080d290427.png", width: 3518, height: 5643 },
  back: { src: "/closeup/badge-back.4a6a27a4240c.png", width: 3516, height: 5640 },
};
const initialView = { zoom: 1, x: 0, y: 0 };
const centre = { x: 0, y: 0 };

function BadgePhoto({ image, onLoad, onError }) {
  return (
    <span className="closeup-photo">
      <img src={image.src} alt="" draggable="false" decoding="async" onLoad={onLoad} onError={onError}
        style={{ width: `${image.height / image.width * 100}%`, height: `${image.width / image.height * 100}%` }} />
    </span>
  );
}

export default function CloseupViewer() {
  const [side, setSide] = useState(() => new URLSearchParams(window.location.search).get("side") === "back" ? "back" : "front");
  const [view, setView] = useState(initialView);
  const [size, setSize] = useState({ width: 1, height: 1 });
  const [photo, setPhoto] = useState(null);
  const [dragging, setDragging] = useState(false);
  const viewport = useRef(null);
  const pointers = useRef(new Map());
  const image = images[side];
  const metrics = useMemo(() => viewMetrics(image, size), [image, size]);
  const current = constrainView(view, metrics);
  const scale = metrics.fit * current.zoom;
  const overview = overviewRect(current, metrics);
  const activePhoto = photo?.source === image.src ? photo : null;
  const status = activePhoto?.status || "loading";
  const displayImage = activePhoto?.url ? { ...image, src: activePhoto.url } : null;

  useEffect(() => {
    let cancelled = false;
    let objectUrl;
    loadCloseupPhoto(image.src).then((blob) => {
      if (cancelled) return;
      objectUrl = URL.createObjectURL(blob);
      setPhoto({ source: image.src, url: objectUrl, status: "loading" });
    }).catch(() => {
      if (!cancelled) setPhoto({ source: image.src, status: "error" });
    });
    return () => {
      cancelled = true;
      if (objectUrl) URL.revokeObjectURL(objectUrl);
    };
  }, [image.src]);

  function photoStatus(nextStatus, url) {
    setPhoto((previous) => previous?.url === url ? { ...previous, status: nextStatus } : previous);
  }

  useEffect(() => {
    const observer = new ResizeObserver(([entry]) => setSize({ width: entry.contentRect.width, height: entry.contentRect.height }));
    observer.observe(viewport.current);
    return () => observer.disconnect();
  }, []);

  useEffect(() => { setView((previous) => constrainView(previous, metrics)); }, [metrics]);

  useEffect(() => {
    const element = viewport.current;
    const onWheel = (event) => {
      event.preventDefault();
      const rect = element.getBoundingClientRect();
      const point = { x: event.clientX - rect.left - rect.width / 2, y: event.clientY - rect.top - rect.height / 2 };
      const delta = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? rect.height : 1);
      setView((previous) => zoomView(previous, previous.zoom * Math.exp(-delta * 0.002), point, metrics));
    };
    element.addEventListener("wheel", onWheel, { passive: false });
    return () => element.removeEventListener("wheel", onWheel);
  }, [metrics]);

  function changeSide(nextSide) {
    if (nextSide === side) return;
    setSide(nextSide);
    setPhoto(null);
    setView(initialView);
    pointers.current.clear();
    setDragging(false);
    const url = new URL(window.location.href);
    url.searchParams.set("side", nextSide);
    window.history.replaceState(null, "", url);
  }

  function zoomTo(zoom, point = centre) {
    setView((previous) => zoomView(previous, zoom, point, metrics));
  }

  function pointFromEvent(event) {
    const rect = viewport.current.getBoundingClientRect();
    return { x: event.clientX - rect.left - rect.width / 2, y: event.clientY - rect.top - rect.height / 2 };
  }

  function startPointer(event) {
    if (event.button !== 0) return;
    event.preventDefault();
    viewport.current.focus({ preventScroll: true });
    event.currentTarget.setPointerCapture(event.pointerId);
    pointers.current.set(event.pointerId, pointFromEvent(event));
    setDragging(true);
  }

  function movePointer(event) {
    if (!pointers.current.has(event.pointerId)) return;
    const before = [...pointers.current.values()];
    pointers.current.set(event.pointerId, pointFromEvent(event));
    const after = [...pointers.current.values()];
    setView((previous) => gestureView(previous, before, after, metrics));
  }

  function endPointer(event) {
    pointers.current.delete(event.pointerId);
    setDragging(pointers.current.size > 0);
  }

  function handleKey(event) {
    const step = event.shiftKey ? 150 : 60;
    const directions = { ArrowLeft: [step, 0], ArrowRight: [-step, 0], ArrowUp: [0, step], ArrowDown: [0, -step] };
    if (directions[event.key]) {
      event.preventDefault();
      const [x, y] = directions[event.key];
      setView((previous) => constrainView({ ...previous, x: previous.x + x, y: previous.y + y }, metrics));
    } else if (["+", "=", "-", "_", "0", "Home"].includes(event.key)) {
      event.preventDefault();
      if (event.key === "0" || event.key === "Home") setView(initialView);
      else zoomTo(current.zoom * (["+", "="].includes(event.key) ? 1.5 : 1 / 1.5));
    }
  }

  function moveOverview(event) {
    const rect = event.currentTarget.getBoundingClientRect();
    const x = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    const y = clamp((event.clientY - rect.top) / rect.height, 0, 1);
    setView((previous) => constrainView({
      ...previous,
      x: (0.5 - x) * image.width * metrics.fit * previous.zoom,
      y: (0.5 - y) * image.height * metrics.fit * previous.zoom,
    }, metrics));
  }

  return (
    <main id="content" className="closeup-page">
      <header className="closeup-heading">
        <div>
          <p className="eyebrow"><span>TAKE A CLOSER LOOK</span></p>
          <h1>Ultra Closeup<span>.</span></h1>
          <p>Every trace, every component, every tiny detail.</p>
        </div>
        <a className="closeup-home" href="/">← Back to the badge</a>
      </header>

      <section className="closeup-workspace" aria-label="Badge photo explorer">
        <div className="closeup-toolbar">
          <div className="closeup-sides" role="group" aria-label="Choose badge side">
            {["front", "back"].map((value) => <button key={value} type="button" aria-pressed={side === value} onClick={() => changeSide(value)}>{value === "front" ? "Front" : "Back"}</button>)}
          </div>
          <div className="closeup-zoom" role="group" aria-label="Zoom controls">
            <button type="button" aria-label="Zoom out" disabled={current.zoom <= 1} onClick={() => zoomTo(current.zoom / 1.5)}>−</button>
            <input type="range" aria-label="Zoom" aria-valuetext={`${Math.round(scale * 100)} percent of original size`} min="0" max="100" step="0.1"
              value={Math.log(current.zoom) / Math.log(metrics.maxZoom) * 100}
              onChange={(event) => zoomTo(metrics.maxZoom ** (Number(event.target.value) / 100))} />
            <button type="button" aria-label="Zoom in" disabled={current.zoom >= metrics.maxZoom} onClick={() => zoomTo(current.zoom * 1.5)}>+</button>
            <output className="closeup-zoom-value" aria-label="Current zoom">{Math.round(scale * 100)}%</output>
          </div>
        </div>

        <div className="closeup-canvas">
          <div ref={viewport} className={`closeup-viewport${dragging ? " is-dragging" : ""}`} role="region" tabIndex={0}
            aria-label={`Interactive ${side} of the badge`} aria-describedby="closeup-help" aria-busy={status === "loading"}
            onPointerDown={startPointer} onPointerMove={movePointer} onPointerUp={endPointer} onPointerCancel={endPointer} onLostPointerCapture={endPointer}
            onKeyDown={handleKey} onDoubleClick={(event) => zoomTo(current.zoom * 2, pointFromEvent(event))}>
            <div className="closeup-image" style={{ width: image.width, height: image.height, transform: `translate(${size.width / 2 + current.x - image.width * scale / 2}px, ${size.height / 2 + current.y - image.height * scale / 2}px) scale(${scale})` }}>
              {displayImage && <BadgePhoto key={displayImage.src} image={displayImage}
                onLoad={() => photoStatus("ready", displayImage.src)}
                onError={() => photoStatus("error", displayImage.src)} />}
            </div>
          </div>

          {status !== "ready" && <div className="closeup-status" role="status">
            {status === "loading" ? "Loading full-resolution photo…" : <>Couldn’t load this photo. <button type="button" onClick={() => window.location.reload()}>Try again</button></>}
          </div>}

          {status === "ready" && <div className="closeup-overview">
            <span>OVERVIEW</span>
            <button type="button" className="closeup-minimap" style={{ aspectRatio: `${image.width} / ${image.height}` }} aria-label="Badge overview. Drag to move the view, or use arrow keys."
              onPointerDown={(event) => { if (event.button !== 0) return; event.preventDefault(); event.currentTarget.focus({ preventScroll: true }); event.currentTarget.setPointerCapture(event.pointerId); moveOverview(event); }}
              onPointerMove={(event) => { if (event.currentTarget.hasPointerCapture(event.pointerId)) moveOverview(event); }}
              onKeyDown={handleKey} onClick={(event) => { if (event.detail === 0) setView((previous) => ({ ...previous, x: 0, y: 0 })); }}>
              <BadgePhoto image={displayImage} />
              <span className="closeup-minimap-window" style={{ left: `${overview.left * 100}%`, top: `${overview.top * 100}%`, width: `${overview.width * 100}%`, height: `${overview.height * 100}%` }} />
            </button>
          </div>}
        </div>

        <div id="closeup-help" className="closeup-help">
          <p>Drag to pan. Scroll or pinch to zoom. Double-click to get closer.</p>
          <p>Keyboard: <kbd>+</kbd> <kbd>−</kbd> to zoom, arrow keys to pan, <kbd>0</kbd> to fit.</p>
        </div>
      </section>
    </main>
  );
}
