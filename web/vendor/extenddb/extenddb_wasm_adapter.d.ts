/* tslint:disable */
/* eslint-disable */

/**
 * A handle to an in-browser ExtendDB engine, backed by in-memory storage.
 *
 * Construct one with [`ExtendDb::create`], then call [`ExtendDb::submit`] for
 * each DynamoDB request.
 */
export class ExtendDb {
    private constructor();
    free(): void;
    [Symbol.dispose](): void;
    /**
     * Create an engine instance backed by a fresh in-browser in-memory SQLite
     * database.
     *
     * Installs [`console_error_panic_hook`] once so a panic inside the wasm
     * module surfaces as a readable `console.error` with a stack trace rather
     * than an unhelpful `RuntimeError: unreachable`.
     *
     * Async because opening the database and running the catalog migrations is
     * async; it resolves with the ready engine or rejects with an
     * initialization error. In JavaScript: `const db = await ExtendDb.create();`
     */
    static create(): Promise<ExtendDb>;
    /**
     * Submit a DynamoDB request and resolve with the response as a JSON string.
     *
     * Returns a `Promise<string>`. `operation` is the operation name (the
     * `X-Amz-Target` suffix, e.g. `"DescribeLimits"`); `body` is the request
     * body as a JSON string (`"{}"` when the operation takes no parameters).
     *
     * Errors are delivered as values, not rejections: an engine error (or a
     * malformed body) resolves the promise with a DynamoDB-shaped error
     * document carrying a `__type` member — the same JSON a real DynamoDB
     * endpoint returns. The promise rejects only on an adapter fault, which
     * the current surface does not produce.
     */
    submit(operation: string, body: string): Promise<any>;
}

export type InitInput = RequestInfo | URL | Response | BufferSource | WebAssembly.Module;

export interface InitOutput {
    readonly memory: WebAssembly.Memory;
    readonly __wbg_extenddb_free: (a: number, b: number) => void;
    readonly extenddb_create: () => any;
    readonly extenddb_submit: (a: number, b: number, c: number, d: number, e: number) => any;
    readonly rust_sqlite_wasm_host_sleep: (a: bigint, b: number) => void;
    readonly rust_sqlite_wasm_abort: () => void;
    readonly rust_sqlite_wasm_assert_fail: (a: number, b: number, c: number, d: number) => void;
    readonly rust_sqlite_wasm_calloc: (a: number, b: number) => number;
    readonly rust_sqlite_wasm_free: (a: number) => void;
    readonly rust_sqlite_wasm_getentropy: (a: number, b: number) => number;
    readonly rust_sqlite_wasm_host_epoch_timestamp_in_ms: (a: number) => number;
    readonly rust_sqlite_wasm_host_fill_entropy: (a: number, b: number) => number;
    readonly rust_sqlite_wasm_host_localtime: (a: bigint, b: number) => number;
    readonly rust_sqlite_wasm_host_random: (a: number, b: number) => number;
    readonly rust_sqlite_wasm_localtime: (a: number) => number;
    readonly rust_sqlite_wasm_malloc: (a: number) => number;
    readonly rust_sqlite_wasm_realloc: (a: number, b: number) => number;
    readonly sqlite3_os_end: () => number;
    readonly sqlite3_os_init: () => number;
    readonly wasm_bindgen_11c620aef56740ba___convert__closures_____invoke___js_sys_94566e8111f6f9f5___Function_fn_wasm_bindgen_11c620aef56740ba___JsValue_____wasm_bindgen_11c620aef56740ba___sys__Undefined___js_sys_94566e8111f6f9f5___Function_fn_wasm_bindgen_11c620aef56740ba___JsValue_____wasm_bindgen_11c620aef56740ba___sys__Undefined_______true_: (a: number, b: number, c: any, d: any) => void;
    readonly wasm_bindgen_11c620aef56740ba___convert__closures_____invoke___wasm_bindgen_11c620aef56740ba___JsValue__core_e845aeda8cd93bad___result__Result_____wasm_bindgen_11c620aef56740ba___JsError___true_: (a: number, b: number, c: any) => [number, number];
    readonly __wbindgen_exn_store: (a: number) => void;
    readonly __externref_table_alloc: () => number;
    readonly __wbindgen_externrefs: WebAssembly.Table;
    readonly __wbindgen_free: (a: number, b: number, c: number) => void;
    readonly __wbindgen_malloc: (a: number, b: number) => number;
    readonly __wbindgen_realloc: (a: number, b: number, c: number, d: number) => number;
    readonly __wbindgen_destroy_closure: (a: number, b: number) => void;
    readonly __externref_table_dealloc: (a: number) => void;
    readonly __wbindgen_start: () => void;
}

export type SyncInitInput = BufferSource | WebAssembly.Module;

/**
 * Instantiates the given `module`, which can either be bytes or
 * a precompiled `WebAssembly.Module`.
 *
 * @param {{ module: SyncInitInput }} module - Passing `SyncInitInput` directly is deprecated.
 *
 * @returns {InitOutput}
 */
export function initSync(module: { module: SyncInitInput } | SyncInitInput): InitOutput;

/**
 * If `module_or_path` is {RequestInfo} or {URL}, makes a request and
 * for everything else, calls `WebAssembly.instantiate` directly.
 *
 * @param {{ module_or_path: InitInput | Promise<InitInput> }} module_or_path - Passing `InitInput` directly is deprecated.
 *
 * @returns {Promise<InitOutput>}
 */
export default function __wbg_init (module_or_path?: { module_or_path: InitInput | Promise<InitInput> } | InitInput | Promise<InitInput>): Promise<InitOutput>;
