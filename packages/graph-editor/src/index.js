export { GraphEditor } from './view/editor.js';
export { GraphStore, normalizeDocument, COLLISION } from './core/model.js';
export * from './core/curve.js';
export * from './core/time.js';
export { STRINGS } from './view/strings.js';

import { GraphEditor } from './view/editor.js';

/**
 * Create a graph editor inside `container`.
 * @param {HTMLElement} container
 * @param {object} [options]
 */
export function createGraphEditor(container, options) {
  return new GraphEditor(container, options);
}
