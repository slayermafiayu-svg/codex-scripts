// Type definitions for @vegas3d/graph-editor

export type Fps = { num: number; den: number };
export type Interp = 'bezier' | 'linear' | 'hold';
export type TangentMode = 'auto' | 'unified' | 'broken';
export type Extrapolation = 'hold' | 'linear' | 'cycle' | 'pingpong';
export type TimeFormat = 'timecode' | 'frames' | 'seconds';
export type GraphMode = 'value' | 'speed';
export type HandlesVisibility = 'selected' | 'all' | 'none';
export type CollisionPolicy = 'block' | 'replace';

export interface Handle {
  /** Seconds relative to the key. `in.dt <= 0`, `out.dt >= 0`. */
  dt: number;
  /** Value units relative to the key. */
  dv: number;
}

export interface Keyframe {
  /** Stable, name-independent id. Generated when missing. */
  id: string;
  /** Seconds (canonical). Never rounded by the evaluator. */
  t: number;
  v: number;
  /** Interpolation of the segment leaving this key. */
  interp: Interp;
  tangents: TangentMode;
  in: Handle;
  out: Handle;
}

export interface KeyframeInput extends Partial<Keyframe> {
  t: number;
  v: number;
}

export interface Channel {
  id: string;
  name: string;
  /** Short label used in very narrow layouts. */
  shortName?: string;
  group?: string;
  unit?: string;
  color?: string;
  visible?: boolean;
  locked?: boolean;
  extrapolation?: { pre?: Extrapolation; post?: Extrapolation };
  keys: Keyframe[];
  [extra: string]: unknown;
}

export interface ChannelInput extends Omit<Partial<Channel>, 'keys'> {
  keys?: KeyframeInput[];
}

export interface GraphDocument {
  version?: number;
  fps: Fps;
  /** Event range in seconds. Displayed frames are [start, end). */
  range: { start: number; end: number };
  channels: Channel[];
  meta?: unknown;
}

export interface GraphDocumentInput {
  fps?: Fps | number;
  range?: { start: number; end: number };
  channels?: ChannelInput[];
  meta?: unknown;
}

export interface Snapshot {
  fps: Fps;
  range: { start: number; end: number };
  channels: Channel[];
}

export interface GraphEditorOptions {
  document?: GraphDocumentInput;
  fps?: Fps | number;
  range?: { start: number; end: number };
  mode?: GraphMode;
  normalize?: boolean;
  timeFormat?: TimeFormat;
  snapFrames?: boolean;
  snapKeys?: boolean;
  showHandles?: HandlesVisibility;
  collision?: CollisionPolicy;
  /** 'internal' keeps its own undo stack; 'external' only emits transactions. */
  undo?: 'internal' | 'external';
  locale?: 'en' | 'tr' | Record<string, unknown>;
  theme?: 'dark' | 'light' | 'auto';
  playhead?: number;
  showToolbar?: boolean;
  showInspector?: boolean;
  showNavigator?: boolean;
  showEasingInset?: boolean;
  showStatus?: boolean;
  reducedMotion?: boolean | null;
}

export interface ChangeEvent {
  label: string;
  kind: 'edit' | 'undo' | 'redo' | 'load' | 'external';
  document: GraphDocument;
}

export interface TransactionEvent {
  label: string;
  before: Snapshot;
  after: Snapshot;
}

export interface SelectionEvent {
  keys: string[];
  segment: { channelId: string; keyId: string } | null;
  activeChannelId: string | null;
}

export interface PlayheadEvent {
  time: number;
  source: 'user' | 'host' | string;
}

export interface ViewChangeEvent {
  t0: number;
  t1: number;
  pxPerSec: number;
}

export interface EventMap {
  change: ChangeEvent;
  transaction: TransactionEvent;
  selection: SelectionEvent;
  playhead: PlayheadEvent;
  viewchange: ViewChangeEvent;
  modechange: { mode: GraphMode };
  'request-undo': Record<string, never>;
  'request-redo': Record<string, never>;
}

export interface MoveResult {
  dt: number;
  dv: number;
  blocked: boolean;
}

export declare class GraphStore {
  constructor(doc?: GraphDocumentInput, options?: { undo?: 'internal' | 'external'; maxUndo?: number; collision?: CollisionPolicy });
  readonly doc: GraphDocument;
  readonly fps: Fps;
  readonly range: { start: number; end: number };
  readonly channels: Channel[];
  readonly editableChannels: Channel[];
  selectedKeys: Set<string>;
  selectedSegment: { channelId: string; keyId: string } | null;
  activeChannelId: string | null;
  readonly canUndo: boolean;
  readonly canRedo: boolean;
  readonly isTransacting: boolean;
  on(type: 'change' | 'transaction' | 'selection', fn: (payload: any) => void): () => void;
  setDoc(doc: GraphDocumentInput, opts?: { keepHistory?: boolean }): void;
  exportDoc(): GraphDocument;
  setFps(fps: Fps | number): void;
  setRange(range: { start: number; end: number }): void;
  channel(id: string): Channel | null;
  keyRef(id: string): { channel: Channel; key: Keyframe; index: number } | null;
  setChannelProps(id: string, patch: Partial<Channel>): void;
  evalChannel(channelId: string, t: number): number;
  speedChannel(channelId: string, t: number): number;
  select(ids: string[], mode?: 'replace' | 'add' | 'toggle' | 'remove'): void;
  selectSegment(channelId: string, keyId: string | null): void;
  clearSelection(): void;
  selectAll(channelIds?: string[]): void;
  targetSegments(): Array<{ channel: Channel; k0: Keyframe; k1: Keyframe }>;
  begin(label?: string): void;
  commit(): void;
  cancel(): void;
  restoreTransactionStart(): void;
  transactionStart(): Snapshot | null;
  undo(): boolean;
  redo(): boolean;
  applySnapshot(snapshot: Snapshot): void;
  moveKeys(
    ids: string[],
    dt: number,
    dv: number,
    opts?: {
      snap?: boolean;
      anchorId?: string | null;
      timeOnly?: boolean;
      valueOnly?: boolean;
      collision?: CollisionPolicy;
      dvScale?: Map<string, number>;
    },
  ): MoveResult;
  setKeyTime(id: string, t: number, opts?: { snap?: boolean; collision?: CollisionPolicy }): boolean;
  setKeyValue(id: string, v: number): boolean;
  setHandle(id: string, side: 'in' | 'out', handle: Handle, opts?: { breakTangents?: boolean }): void;
  setInterp(ids: string[], interp: Interp): void;
  setTangents(ids: string[], mode: TangentMode): void;
  applyEasing(cp: [number, number, number, number], segments?: Array<{ channel: Channel; k0: Keyframe; k1: Keyframe }>): number;
  insertKey(channelId: string, t: number, opts?: { snap?: boolean; value?: number }): Keyframe | null;
  deleteKeys(ids: string[]): void;
  copyKeys(ids?: string[]): unknown;
  pasteKeys(clip: unknown, at: number, opts?: { snap?: boolean }): string[];
  scaleKeys(ids: string[], opts: { pivotT?: number; kT?: number; pivotV?: number; kV?: number; snap?: boolean }): void;
  duplicateKeys(ids: string[], dt?: number): string[];
}

export declare class GraphEditor {
  constructor(container: HTMLElement, options?: GraphEditorOptions);
  readonly root: HTMLElement;
  readonly store: GraphStore;
  readonly options: GraphEditorOptions;
  on<K extends keyof EventMap>(type: K, fn: (payload: EventMap[K]) => void): () => void;
  setDocument(doc: GraphDocumentInput): void;
  getDocument(): GraphDocument;
  setPlayhead(t: number, source?: string): void;
  getPlayhead(): number;
  setMode(mode: GraphMode): void;
  getMode(): GraphMode;
  setNormalize(on: boolean): void;
  setOption<K extends keyof GraphEditorOptions>(name: K, value: GraphEditorOptions[K]): void;
  setFps(fps: Fps | number): void;
  setRange(range: { start: number; end: number }): void;
  selectKeys(ids: string[], mode?: 'replace' | 'add' | 'toggle' | 'remove'): void;
  getSelection(): { keys: string[]; segment: { channelId: string; keyId: string } | null };
  applyEasing(idOrCp: string | [number, number, number, number]): number;
  copySelection(): unknown;
  pasteAtPlayhead(): string[];
  undo(): void;
  redo(): void;
  fitAll(): void;
  fitSelected(): void;
  fitRange(): void;
  jumpToKey(dir: 1 | -1): void;
  setLocale(locale: 'en' | 'tr' | Record<string, unknown>): void;
  setTheme(theme: 'dark' | 'light' | 'auto'): void;
  resize(): void;
  destroy(): void;
}

export declare function createGraphEditor(container: HTMLElement, options?: GraphEditorOptions): GraphEditor;

// Pure helpers (see src/core/time.js and src/core/curve.js)
export declare function makeFps(num: number, den?: number): Fps;
export declare function fpsFromNumber(value: number): Fps;
export declare function secondsToFrames(t: number, fps: Fps): number;
export declare function framesToSeconds(n: number, fps: Fps): number;
export declare function snapToFrame(t: number, fps: Fps): number;
export declare function visibleFrames(
  range: { start: number; end: number },
  fps: Fps,
): { count: number; firstFrame: number; lastFrame: number; boundary: number };
export declare function fitsInRange(t: number, range: { start: number; end: number }, fps: Fps): boolean;
export declare function formatTime(t: number, fps: Fps, mode?: TimeFormat, opts?: { showSubframe?: boolean }): string;
export declare function parseTime(text: string, fps: Fps, opts?: { mode?: TimeFormat; base?: number }): number | null;
export declare function parseNumber(text: string): number | null;
export declare function evalKeys(keys: Keyframe[], t: number, extrapolation?: { pre?: Extrapolation; post?: Extrapolation }): number;
export declare function speedKeys(keys: Keyframe[], t: number): number;
export declare function splitSegment(k0: Keyframe, k1: Keyframe, t: number, id?: string): { key: Keyframe; k0: Keyframe; k1: Keyframe };
export declare function retimeKeys(keys: Keyframe[], A: number, Ds: number, a: number, Dt: number): Keyframe[];
export declare function shiftKeys(keys: Keyframe[], A: number, a: number): Keyframe[];
export declare const EASING_PRESETS: ReadonlyArray<{ id: string; cp: [number, number, number, number] }>;
export declare const INTERP: { BEZIER: 'bezier'; LINEAR: 'linear'; HOLD: 'hold' };
export declare const TANGENTS: { AUTO: 'auto'; UNIFIED: 'unified'; BROKEN: 'broken' };
export declare const EXTRAPOLATION: { HOLD: 'hold'; LINEAR: 'linear'; CYCLE: 'cycle'; PINGPONG: 'pingpong' };
export declare const COLLISION: { BLOCK: 'block'; REPLACE: 'replace' };
