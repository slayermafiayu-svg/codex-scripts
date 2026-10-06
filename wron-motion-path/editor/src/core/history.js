// Undo/redo of editor transactions (document snapshots). One logical edit
// (a whole drag, one inspector change, one preset load) is one entry.
export class History {
  constructor(limit = 200) {
    this.limit = limit;
    this.undoStack = [];
    this.redoStack = [];
  }
  push(label, before, after, mergeKey = null) {
    const top = this.undoStack[this.undoStack.length - 1];
    // Consecutive nudges (same key, within 700 ms) merge into one step.
    if (mergeKey && top && top.mergeKey === mergeKey && performance.now() - top.time < 700) {
      top.after = after;
      top.time = performance.now();
    } else {
      this.undoStack.push({ label, before, after, mergeKey, time: performance.now() });
      if (this.undoStack.length > this.limit) this.undoStack.shift();
    }
    this.redoStack.length = 0;
  }
  canUndo() { return this.undoStack.length > 0; }
  canRedo() { return this.redoStack.length > 0; }
  undo() {
    const e = this.undoStack.pop();
    if (!e) return null;
    this.redoStack.push(e);
    return e;
  }
  redo() {
    const e = this.redoStack.pop();
    if (!e) return null;
    this.undoStack.push(e);
    return e;
  }
  clear() {
    this.undoStack.length = 0;
    this.redoStack.length = 0;
  }
}
