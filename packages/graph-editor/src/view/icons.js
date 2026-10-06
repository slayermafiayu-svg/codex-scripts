// Inline SVG icons: one stroke language (1.6px, round joins), 16px grid.
const wrap = (inner) =>
  `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${inner}</svg>`;

export const ICONS = {
  value: wrap('<path d="M2 12c3 0 3-8 6-8s3 8 6 8"/>'),
  speed: wrap('<path d="M2 11h3l2-6 2 8 2-5h3"/>'),
  fitAll: wrap('<path d="M2 6V2h4M10 2h4v4M14 10v4h-4M6 14H2v-4"/>'),
  fitSelected: wrap('<path d="M2 6V2h4M10 2h4v4M14 10v4h-4M6 14H2v-4"/><circle cx="8" cy="8" r="2"/>'),
  fitRange: wrap('<path d="M3 2v12M13 2v12M5 8h6M9 6l2 2-2 2"/>'),
  normalize: wrap('<path d="M2 13h12M2 3h12M4 10l3-4 2 3 3-5"/>'),
  snap: wrap('<path d="M4 2v12M12 2v12M8 5v6M6 8h4"/>'),
  magnet: wrap('<path d="M4 3v6a4 4 0 0 0 8 0V3M4 3h3v6M9 3h3v6"/>'),
  handles: wrap('<circle cx="8" cy="8" r="1.6"/><path d="M3 11l5-3 5-3"/><circle cx="3" cy="11" r="1.4"/><circle cx="13" cy="5" r="1.4"/>'),
  undo: wrap('<path d="M6 5H3V2M3.2 5A6 6 0 1 1 3 9.5"/>'),
  redo: wrap('<path d="M10 5h3V2M12.8 5A6 6 0 1 0 13 9.5"/>'),
  help: wrap('<circle cx="8" cy="8" r="6"/><path d="M6.2 6.2a1.8 1.8 0 1 1 2.6 1.6c-.6.4-.8.7-.8 1.4M8 11.6v.1"/>'),
  lock: wrap('<rect x="3.5" y="7" width="9" height="7" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2"/>'),
  unlock: wrap('<rect x="3.5" y="7" width="9" height="7" rx="1.5"/><path d="M5.5 7V5a2.5 2.5 0 0 1 4.6-1.3"/>'),
  eye: wrap('<path d="M1.5 8s2.5-4.5 6.5-4.5S14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/>'),
  bezier: wrap('<path d="M2 13C6 13 10 3 14 3"/><path d="M2 13l4-4M14 3l-4 4" stroke-dasharray="1.5 2"/>'),
  linear: wrap('<path d="M2 13L14 3"/>'),
  hold: wrap('<path d="M2 12h6V4h6"/>'),
  easing: wrap('<path d="M2 13C8 13 8 3 14 3"/><circle cx="8" cy="8" r="1.2"/>'),
  chevron: wrap('<path d="M5 6.5l3 3 3-3"/>'),
  more: wrap('<circle cx="4" cy="8" r="1.2"/><circle cx="8" cy="8" r="1.2"/><circle cx="12" cy="8" r="1.2"/>'),
  close: wrap('<path d="M4 4l8 8M12 4l-8 8"/>'),
  clock: wrap('<circle cx="8" cy="8" r="6"/><path d="M8 4.5V8l2.5 1.5"/>'),
};
