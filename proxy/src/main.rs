// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// ddbsh-web-proxy — the native local companion for the ddbsh real-backend web
// UI (phase 6f, D-P6f-3; design of record: discussions/2026-11-17-P6f-01).
//
// A browser cannot read `~/.aws/credentials` and cannot call DynamoDB directly
// (no CORS), so this small, generic relay does both for it: it serves the static
// ddbsh-wasm bundle, holds per-session connection state, and on `POST /ddb`
// SigV4-signs the browser's AWS-JSON-1.0 body and forwards it to the session's
// DynamoDB / ExtendDB endpoint. Credentials never reach the browser.
//
// SECURITY MODEL (builder-security, followed as policy; §4):
//   * Ingress is LOOPBACK ONLY and not configurable — the proxy binds 127.0.0.1
//     (and ::1 when available); there is no wide-bind option, so it can never
//     become an open credentialed relay. Remote access is the user's SSH tunnel.
//   * A driver guard (origin + per-session token) stops a malicious page open in
//     the SAME browser from driving the proxy (confused-deputy / DNS-rebinding).
//   * A user-owned egress policy (anchored allow/block regexes, block wins)
//     bounds where the relay may be pointed; default = loopback + DynamoDB host
//     families, IMDS blocked.
//
// The binary contains NO ddbsh engine logic. The engine is the WASM artifact.

mod security;
mod sessions;
mod upstream;

use std::collections::HashMap;
use std::net::{Ipv4Addr, Ipv6Addr};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use axum::body::Bytes;
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::Router;
use rand::RngCore;
use serde::Deserialize;
use tokio::sync::Mutex;

use security::{EgressPolicy, LocalIdentity};
use sessions::{AuthError, SessionStore, Target};
use upstream::{sign_and_forward, DDB_CONTENT_TYPE};

// ---- configuration ---------------------------------------------------------

/// The default loopback port. Chosen high and uncommon; override with --port.
const DEFAULT_PORT: u16 = 8456;
/// Default idle session TTL and session cap (§3).
const DEFAULT_SESSION_TTL_SECS: u64 = 1800;
const DEFAULT_MAX_SESSIONS: usize = 256;

struct Config {
    port: u16,
    serve: bool,
    webroot: Option<PathBuf>,
    security_policy: PathBuf,
    session_ttl: Duration,
    max_sessions: usize,
}

fn default_policy_path() -> PathBuf {
    // ~/.ddbsh_web_proxy_security, matching the ddbsh config/history convention.
    // Resolve the home directory portably: HOME on Unix, USERPROFILE on Windows
    // (D-P6f-11 portability). Fall back to the current directory if neither is
    // set (the built-in default policy still applies when the file is absent).
    if let Some(home) = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")) {
        PathBuf::from(home).join(".ddbsh_web_proxy_security")
    } else {
        PathBuf::from(".ddbsh_web_proxy_security")
    }
}

const USAGE: &str = "\
ddbsh-web-proxy — local SigV4 signing relay for the ddbsh web UI

USAGE:
    ddbsh-web-proxy [OPTIONS]

OPTIONS:
    --port <N>              Loopback port to bind (default 8456). Ingress is
                            ALWAYS loopback only; there is no host/bind option.
    --serve                 Serve the static web bundle from --webroot.
    --webroot <DIR>         Directory of the ddbsh-wasm bundle to serve.
    --security-policy <F>   Egress policy file (default ~/.ddbsh_web_proxy_security;
                            built-in default used if absent).
    --session-ttl <SECS>    Idle session TTL (default 1800).
    --max-sessions <N>      Maximum concurrent sessions (default 256).
    -h, --help              Print this help.

Remote access is via an SSH local-forward tunnel (ssh -L); the proxy never binds
a non-loopback address.
";

fn parse_args() -> Result<Config, String> {
    let mut cfg = Config {
        port: DEFAULT_PORT,
        serve: false,
        webroot: None,
        security_policy: default_policy_path(),
        session_ttl: Duration::from_secs(DEFAULT_SESSION_TTL_SECS),
        max_sessions: DEFAULT_MAX_SESSIONS,
    };
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                print!("{USAGE}");
                std::process::exit(0);
            }
            "--port" => {
                let v = args.next().ok_or("--port needs a value")?;
                cfg.port = v.parse().map_err(|_| format!("invalid --port: {v}"))?;
            }
            "--serve" => cfg.serve = true,
            "--webroot" => {
                cfg.webroot = Some(PathBuf::from(args.next().ok_or("--webroot needs a value")?));
            }
            "--security-policy" => {
                cfg.security_policy =
                    PathBuf::from(args.next().ok_or("--security-policy needs a value")?);
            }
            "--session-ttl" => {
                let v = args.next().ok_or("--session-ttl needs a value")?;
                cfg.session_ttl = Duration::from_secs(
                    v.parse()
                        .map_err(|_| format!("invalid --session-ttl: {v}"))?,
                );
            }
            "--max-sessions" => {
                let v = args.next().ok_or("--max-sessions needs a value")?;
                cfg.max_sessions = v
                    .parse()
                    .map_err(|_| format!("invalid --max-sessions: {v}"))?;
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    if cfg.serve && cfg.webroot.is_none() {
        return Err("--serve requires --webroot".to_string());
    }
    Ok(cfg)
}

// ---- shared state ----------------------------------------------------------

struct AppState {
    identity: LocalIdentity,
    policy: EgressPolicy,
    sessions: Mutex<SessionStore>,
    /// Outbound HTTP clients keyed by CA-bundle path ("" = platform defaults),
    /// so a per-endpoint trust setting is applied without rebuilding per request.
    clients: Mutex<HashMap<String, reqwest::Client>>,
    webroot: Option<PathBuf>,
}

impl AppState {
    /// Get-or-build the outbound client for a CA-bundle selection.
    async fn client_for(&self, ca_bundle: Option<&str>) -> Result<reqwest::Client, String> {
        let key = ca_bundle.unwrap_or("").to_string();
        if let Some(c) = self.clients.lock().await.get(&key) {
            return Ok(c.clone());
        }
        let client = build_http_client(ca_bundle)?;
        self.clients.lock().await.insert(key, client.clone());
        Ok(client)
    }
}

fn build_http_client(ca_bundle: Option<&str>) -> Result<reqwest::Client, String> {
    let mut builder = reqwest::Client::builder()
        .user_agent(concat!("ddbsh-web-proxy/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(30));
    if let Some(path) = ca_bundle {
        let pem = std::fs::read(path).map_err(|e| format!("read CA bundle {path}: {e}"))?;
        let cert = reqwest::Certificate::from_pem(&pem)
            .map_err(|e| format!("parse CA bundle {path}: {e}"))?;
        builder = builder.add_root_certificate(cert);
    }
    builder
        .build()
        .map_err(|e| format!("build HTTP client: {e}"))
}

// ---- helpers ---------------------------------------------------------------

fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    rand::thread_rng().fill_bytes(&mut buf);
    let mut s = String::with_capacity(bytes * 2);
    for b in buf {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn header<'a>(headers: &'a HeaderMap, name: &str) -> Option<&'a str> {
    headers.get(name).and_then(|v| v.to_str().ok())
}

/// A proxy-side JSON error body. For /ddb this is shaped like a DynamoDB error
/// document so the ddbsh-wire codec can map it, but the proxy's own guard
/// failures are plain and the browser hook treats them as transport failures.
fn json_error(status: StatusCode, message: &str) -> Response {
    let body = serde_json::json!({ "message": message }).to_string();
    (status, [("content-type", "application/json")], body).into_response()
}

/// Apply the driver guard (origin + host). Returns `Some(response)` to
/// short-circuit the handler with a rejection, `None` when the request passes.
fn check_origin_host(state: &AppState, headers: &HeaderMap) -> Option<Response> {
    if !state.identity.origin_ok(header(headers, "origin")) {
        return Some(json_error(StatusCode::FORBIDDEN, "origin not permitted"));
    }
    if !state.identity.host_ok(header(headers, "host")) {
        return Some(json_error(StatusCode::FORBIDDEN, "host not permitted"));
    }
    None
}

// ---- handlers --------------------------------------------------------------

/// POST /session — mint a session id + per-session token. Origin/Host-guarded;
/// no token required (this is where the token is issued). A cross-origin page
/// fails the origin check and, even absent that, cannot READ the response (the
/// proxy sends no CORS headers).
async fn handle_session(State(state): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    if let Some(resp) = check_origin_host(&state, &headers) {
        return resp;
    }
    let id = random_hex(16);
    let token = random_hex(32);
    let now = Instant::now();
    let ok = state
        .sessions
        .lock()
        .await
        .insert(id.clone(), token.clone(), now);
    if !ok {
        return json_error(StatusCode::SERVICE_UNAVAILABLE, "session capacity reached");
    }
    // Report the proxy version so the browser glue can surface it in the engine's
    // SHOW CONNECTION report (D-P6f-9, descriptor enrichment). No secret.
    let body = serde_json::json!({
        "session": id,
        "token": token,
        "version": env!("CARGO_PKG_VERSION"),
    })
    .to_string();
    (StatusCode::OK, [("content-type", "application/json")], body).into_response()
}

#[derive(Deserialize)]
struct ConnectBody {
    region: String,
    #[serde(default)]
    endpoint: Option<String>,
    #[serde(default)]
    profile: Option<String>,
    #[serde(default)]
    ca_bundle: Option<String>,
}

/// POST /connect — set this session's signing target (region + optional endpoint
/// override + optional credential profile + optional CA bundle). Origin/Host +
/// token guarded; egress-policy checked before any credential resolution.
async fn handle_connect(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    if let Some(resp) = check_origin_host(&state, &headers) {
        return resp;
    }
    let (session_id, token) = match (
        header(&headers, "x-ddbsh-session"),
        header(&headers, "x-ddbsh-token"),
    ) {
        (Some(s), Some(t)) => (s.to_string(), t.to_string()),
        _ => return json_error(StatusCode::UNAUTHORIZED, "missing session/token"),
    };
    // Authenticate (and refresh the idle timer) up front.
    {
        let now = Instant::now();
        let mut store = state.sessions.lock().await;
        if let Err(e) = store.authenticate(&session_id, &token, now) {
            return auth_error_response(e);
        }
    }

    let req: ConnectBody = match serde_json::from_slice(&body) {
        Ok(r) => r,
        Err(e) => return json_error(StatusCode::BAD_REQUEST, &format!("bad connect body: {e}")),
    };
    if req.region.trim().is_empty() {
        return json_error(StatusCode::BAD_REQUEST, "region is required");
    }

    let endpoint_url = match &req.endpoint {
        Some(e) if !e.trim().is_empty() => e.trim().to_string(),
        _ => format!("https://dynamodb.{}.amazonaws.com", req.region.trim()),
    };

    // EGRESS POLICY — enforced before we resolve or attach any credentials, so a
    // disallowed endpoint never even reaches the credential chain.
    if !state.policy.permits(&endpoint_url) {
        return json_error(
            StatusCode::FORBIDDEN,
            &format!("endpoint not permitted by egress policy: {endpoint_url}"),
        );
    }

    let parsed = match url::Url::parse(&endpoint_url) {
        Ok(u) => u,
        Err(e) => return json_error(StatusCode::BAD_REQUEST, &format!("bad endpoint URL: {e}")),
    };
    let host = match parsed.host_str() {
        Some(h) => match parsed.port() {
            Some(p) => format!("{h}:{p}"),
            None => h.to_string(),
        },
        None => return json_error(StatusCode::BAD_REQUEST, "endpoint URL has no host"),
    };

    // Resolve the credentials provider for this target (default chain, or a
    // named profile for a non-AWS backend such as ExtendDB).
    let credentials = match resolve_credentials_provider(&req.region, req.profile.as_deref()).await
    {
        Ok(p) => p,
        Err(e) => {
            return json_error(
                StatusCode::BAD_GATEWAY,
                &format!("credential resolution: {e}"),
            )
        }
    };

    // Ensure the per-endpoint-trust HTTP client exists (fail fast on a bad CA).
    if let Err(e) = state.client_for(req.ca_bundle.as_deref()).await {
        return json_error(StatusCode::BAD_REQUEST, &e);
    }

    let target = Target {
        signing_region: req.region.trim().to_string(),
        endpoint_url: endpoint_url.clone(),
        host,
        credentials,
        ca_bundle: req.ca_bundle.clone(),
    };
    {
        let now = Instant::now();
        let mut store = state.sessions.lock().await;
        match store.authenticate(&session_id, &token, now) {
            Ok(session) => session.target = Some(target),
            Err(e) => return auth_error_response(e),
        }
    }
    let body = serde_json::json!({
        "ok": true,
        "endpoint": endpoint_url,
        "version": env!("CARGO_PKG_VERSION"),
    })
    .to_string();
    (StatusCode::OK, [("content-type", "application/json")], body).into_response()
}

/// POST /ddb — SigV4-sign the body for the session's target and forward it.
/// Origin/Host + token guarded; the session must already be connected.
async fn handle_ddb(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    if let Some(resp) = check_origin_host(&state, &headers) {
        return resp;
    }
    let (session_id, token) = match (
        header(&headers, "x-ddbsh-session"),
        header(&headers, "x-ddbsh-token"),
    ) {
        (Some(s), Some(t)) => (s.to_string(), t.to_string()),
        _ => return json_error(StatusCode::UNAUTHORIZED, "missing session/token"),
    };
    let operation = match header(&headers, "x-ddbsh-op") {
        Some(op) if security::valid_operation(op) => op.to_string(),
        Some(_) => return json_error(StatusCode::BAD_REQUEST, "invalid operation name"),
        None => return json_error(StatusCode::BAD_REQUEST, "missing X-Ddbsh-Op"),
    };

    // Pull the session's resolved target (clone the handles) under the lock.
    let target = {
        let now = Instant::now();
        let mut store = state.sessions.lock().await;
        match store.authenticate(&session_id, &token, now) {
            Ok(session) => match &session.target {
                Some(t) => t.clone(),
                None => {
                    return json_error(
                        StatusCode::BAD_REQUEST,
                        "session has no active connection; CONNECT first",
                    )
                }
            },
            Err(e) => return auth_error_response(e),
        }
    };

    // Resolve credentials (the provider caches internally) OUTSIDE the lock.
    let credentials = {
        use aws_credential_types::provider::ProvideCredentials;
        match target.credentials.provide_credentials().await {
            Ok(c) => c,
            Err(e) => return json_error(StatusCode::BAD_GATEWAY, &format!("credentials: {e}")),
        }
    };
    let client = match state.client_for(target.ca_bundle.as_deref()).await {
        Ok(c) => c,
        Err(e) => return json_error(StatusCode::BAD_GATEWAY, &e),
    };

    match sign_and_forward(&client, &target, &credentials, &operation, body).await {
        Ok(relayed) => {
            // Relay the upstream status + body verbatim. A DynamoDB service error
            // (4xx/5xx with a `__type` document) flows through unchanged for the
            // ddbsh-wire codec to map.
            (
                relayed.status,
                [("content-type", DDB_CONTENT_TYPE)],
                relayed.body,
            )
                .into_response()
        }
        Err(e) => json_error(StatusCode::BAD_GATEWAY, &format!("upstream: {e}")),
    }
}

fn auth_error_response(e: AuthError) -> Response {
    match e {
        AuthError::UnknownSession => json_error(StatusCode::UNAUTHORIZED, "unknown session"),
        AuthError::BadToken => json_error(StatusCode::FORBIDDEN, "bad session token"),
    }
}

/// GET /healthz — a loopback-only liveness probe (no secrets, no session).
async fn handle_health() -> Response {
    (
        StatusCode::OK,
        [("content-type", "application/json")],
        "{\"ok\":true}",
    )
        .into_response()
}

/// Resolve a credentials provider via the AWS default chain for `region`, or the
/// named `profile` (used by a non-AWS backend such as ExtendDB with its own
/// issued key). We accept, store, and mint no key ourselves (§3).
async fn resolve_credentials_provider(
    region: &str,
    profile: Option<&str>,
) -> Result<aws_credential_types::provider::SharedCredentialsProvider, String> {
    let mut loader = aws_config::defaults(aws_config::BehaviorVersion::latest())
        .region(aws_config::Region::new(region.trim().to_string()));
    if let Some(p) = profile {
        loader = loader.profile_name(p);
    }
    let config = loader.load().await;
    config
        .credentials_provider()
        .ok_or_else(|| "no credentials provider resolved from the AWS chain".to_string())
}

/// GET fallback — static file serving from --webroot (P6f-e wires the UI to it).
/// Guards against path traversal: the resolved path must stay under the webroot.
async fn handle_static(State(state): State<Arc<AppState>>, uri: axum::http::Uri) -> Response {
    let Some(webroot) = &state.webroot else {
        return (StatusCode::NOT_FOUND, "not found").into_response();
    };
    let path = uri.path();
    let rel = path.trim_start_matches('/');
    let rel = if rel.is_empty() { "index.html" } else { rel };

    // Reject any traversal or absolute component outright.
    let candidate = std::path::Path::new(rel);
    if candidate
        .components()
        .any(|c| !matches!(c, std::path::Component::Normal(_)))
    {
        return (StatusCode::BAD_REQUEST, "bad path").into_response();
    }
    let full = webroot.join(candidate);
    // Canonicalize and re-check containment (defeats symlink/`..` escapes).
    let (Ok(canon_root), Ok(canon_full)) = (webroot.canonicalize(), full.canonicalize()) else {
        return (StatusCode::NOT_FOUND, "not found").into_response();
    };
    if !canon_full.starts_with(&canon_root) {
        return (StatusCode::BAD_REQUEST, "bad path").into_response();
    }
    match tokio::fs::read(&canon_full).await {
        Ok(bytes) => {
            let content_type = content_type_for(&canon_full);
            (StatusCode::OK, [("content-type", content_type)], bytes).into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}

fn content_type_for(path: &std::path::Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()) {
        Some("html") => "text/html; charset=utf-8",
        Some("js") | Some("mjs") => "text/javascript; charset=utf-8",
        Some("css") => "text/css; charset=utf-8",
        Some("wasm") => "application/wasm",
        Some("json") => "application/json",
        Some("map") => "application/json",
        _ => "application/octet-stream",
    }
}

fn build_router(state: Arc<AppState>) -> Router {
    Router::new()
        .route("/session", post(handle_session))
        .route("/connect", post(handle_connect))
        .route("/ddb", post(handle_ddb))
        .route("/healthz", get(handle_health))
        .fallback(get(handle_static))
        .with_state(state)
}

#[tokio::main]
async fn main() {
    let cfg = match parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("ddbsh-web-proxy: {e}\n\n{USAGE}");
            std::process::exit(2);
        }
    };

    let policy = match EgressPolicy::load_or_default(&cfg.security_policy) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("ddbsh-web-proxy: {e}");
            std::process::exit(2);
        }
    };

    let state = Arc::new(AppState {
        identity: LocalIdentity::new(cfg.port),
        policy,
        sessions: Mutex::new(SessionStore::new(cfg.max_sessions, cfg.session_ttl)),
        clients: Mutex::new(HashMap::new()),
        webroot: cfg.webroot.clone(),
    });

    // INGRESS: loopback only, not configurable. Bind IPv4 loopback; also bind
    // IPv6 loopback when available. There is deliberately no option to bind a
    // non-loopback address — remote access is an SSH tunnel (§4, D-P6f-5).
    let v4 = match tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, cfg.port)).await {
        Ok(l) => l,
        Err(e) => {
            eprintln!("ddbsh-web-proxy: bind 127.0.0.1:{}: {e}", cfg.port);
            std::process::exit(1);
        }
    };
    let v6 = tokio::net::TcpListener::bind((Ipv6Addr::LOCALHOST, cfg.port))
        .await
        .ok();

    println!(
        "ddbsh-web-proxy {} listening on:",
        env!("CARGO_PKG_VERSION")
    );
    println!("  http://127.0.0.1:{}/  (loopback only)", cfg.port);
    if v6.is_some() {
        println!("  http://[::1]:{}/     (loopback only)", cfg.port);
    }
    println!("  origin guard: {}", state.identity.primary_origin());
    match &state.webroot {
        Some(dir) => println!("  serving webroot: {}", dir.display()),
        None => println!("  (no webroot; API only — pass --serve --webroot <dir> to serve the UI)"),
    }
    println!("  remote access: use an SSH tunnel (ssh -L); the proxy never binds a public address");

    let app = build_router(state);

    let v4_server = {
        let app = app.clone();
        tokio::spawn(async move {
            if let Err(e) = axum::serve(v4, app).await {
                eprintln!("ddbsh-web-proxy: v4 server error: {e}");
            }
        })
    };
    let v6_server = v6.map(|listener| {
        let app = app.clone();
        tokio::spawn(async move {
            if let Err(e) = axum::serve(listener, app).await {
                eprintln!("ddbsh-web-proxy: v6 server error: {e}");
            }
        })
    });

    // Shut down cleanly on Ctrl-C so a foreground run in a terminal exits.
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {
            println!("ddbsh-web-proxy: shutting down");
        }
        _ = v4_server => {}
        _ = async { if let Some(h) = v6_server { let _ = h.await; } else { std::future::pending::<()>().await } } => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn random_hex_is_right_length_and_hex() {
        let h = random_hex(16);
        assert_eq!(h.len(), 32);
        assert!(h.chars().all(|c| c.is_ascii_hexdigit()));
        // Two draws differ with overwhelming probability.
        assert_ne!(random_hex(16), random_hex(16));
    }

    #[test]
    fn content_types() {
        assert_eq!(
            content_type_for(std::path::Path::new("x.wasm")),
            "application/wasm"
        );
        assert_eq!(
            content_type_for(std::path::Path::new("x.mjs")),
            "text/javascript; charset=utf-8"
        );
        assert_eq!(
            content_type_for(std::path::Path::new("x.unknown")),
            "application/octet-stream"
        );
    }
}
