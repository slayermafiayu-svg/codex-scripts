// Inline SVG icons (stroke = currentColor), 16x16 viewBox.
const svg = (body) =>
  `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;

export const ICONS = {
  select: svg('<path d="M3 2l9 5-4 1.2L6.2 12z" fill="currentColor" stroke="none"/>'),
  add: svg('<path d="M2.5 13.5c3-1 4-4 5.5-7.5"/><circle cx="8.5" cy="5.5" r="1.6"/><path d="M12 9.5v5M9.5 12h5"/>'),
  remove: svg('<path d="M2.5 13.5c3-1 4-4 5.5-7.5"/><circle cx="8.5" cy="5.5" r="1.6"/><path d="M9.5 12h5"/>'),
  undo: svg('<path d="M5 3L2 6l3 3"/><path d="M2.5 6H10a4 4 0 010 8H6"/>'),
  redo: svg('<path d="M11 3l3 3-3 3"/><path d="M13.5 6H6a4 4 0 000 8h4"/>'),
  corner: svg('<path d="M2 13L8 3l6 10"/><rect x="6.2" y="1.2" width="3.6" height="3.6" fill="currentColor" stroke="none"/>'),
  smooth: svg('<path d="M2 12c2-8 10-8 12 0"/><circle cx="8" cy="6" r="1.9" fill="currentColor" stroke="none"/><path d="M3.5 6h9" stroke-dasharray="1.6 1.4"/>'),
  free: svg('<path d="M2 12c1-6 4-7 6-6M8 6c3-1 5 2 6 6"/><path d="M8 6L3.5 3.5M8 6l4.5-3" stroke-dasharray="1.6 1.4"/><path d="M8 4.4L9.6 6 8 7.6 6.4 6z" fill="currentColor" stroke="none"/>'),
  closed: svg('<ellipse cx="8" cy="8" rx="6" ry="4.5"/><circle cx="8" cy="3.5" r="1.4" fill="currentColor" stroke="none"/>'),
  reverse: svg('<path d="M2 5h10M9 2l3 3-3 3"/><path d="M14 11H4M7 8l-3 3 3 3"/>'),
  grid: svg('<path d="M2 5.5h12M2 10.5h12M5.5 2v12M10.5 2v12"/>'),
  snap: svg('<path d="M4 2v6a4 4 0 008 0V2"/><path d="M4 4.5h2.5M9.5 4.5H12"/>'),
  fitPath: svg('<path d="M2 6V2h4M10 2h4v4M14 10v4h-4M6 14H2v-4"/><path d="M4.5 11c2-5 5-5 7-6"/>'),
  fitFrame: svg('<rect x="2.5" y="4" width="11" height="8" rx="1"/><path d="M1 2.5h3M12 2.5h3M1 13.5h3M12 13.5h3"/>'),
  zoomIn: svg('<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14M5 7h4M7 5v4"/>'),
  zoomOut: svg('<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5L14 14M5 7h4"/>'),
  trash: svg('<path d="M3 4.5h10M6 4.5V3h4v1.5M4.5 4.5l.7 9h5.6l.7-9"/>'),
  settings: svg('<circle cx="8" cy="8" r="2.2"/><path d="M8 1.8v2M8 12.2v2M1.8 8h2M12.2 8h2M3.6 3.6l1.4 1.4M11 11l1.4 1.4M3.6 12.4L5 11M11 5l1.4-1.4"/>'),
};
