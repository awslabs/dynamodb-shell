// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// sessions.rs — per-session connection state (phase 6f, D-P6f-4).
//
// A browser tab is one ddbsh "process"; the proxy keys state by a session id the
// page mints at load and sends as `X-Ddbsh-Session`. Each session carries the
// per-session secret token (the driver guard), the resolved signing target
// (region + endpoint URL + host) that a CONNECT set, and the AWS credentials
// provider for that target. An idle TTL and a session cap bound the state so a
// long-running proxy cannot accumulate unbounded sessions.
//
// The store itself holds no secrets beyond the token and a credentials provider
// handle (the provider resolves/caches real credentials on demand); it never
// logs them. Token comparison is constant-time (see security::constant_time_eq).

use std::collections::HashMap;
use std::time::{Duration, Instant};

use aws_credential_types::provider::SharedCredentialsProvider;

/// The resolved signing target a CONNECT established, plus the credentials
/// provider for it. `None` target means the session was minted but not yet
/// connected — a /ddb before /connect is rejected.
#[derive(Clone, Debug)]
pub struct Target {
    /// SigV4 signing region (e.g. "us-east-1").
    pub signing_region: String,
    /// The full endpoint URL requests are POSTed to.
    pub endpoint_url: String,
    /// The `Host` header value (authority) that will be signed and sent.
    pub host: String,
    /// Credentials provider for this target (default chain or a named profile).
    pub credentials: SharedCredentialsProvider,
    /// CA-bundle file path for per-endpoint trust (None = platform default
    /// roots). Used to select the right outbound HTTP client.
    pub ca_bundle: Option<String>,
}

/// One live session.
#[derive(Debug)]
pub struct Session {
    pub token: String,
    pub target: Option<Target>,
    last_seen: Instant,
}

/// Bounded, TTL-evicting session store. Not internally synchronized — the caller
/// wraps it in a mutex (connect/ddb are infrequent relative to the signing work
/// done outside the lock).
pub struct SessionStore {
    sessions: HashMap<String, Session>,
    cap: usize,
    ttl: Duration,
}

/// Why a token/session validation failed, so the handler can choose the status.
#[derive(Debug, PartialEq, Eq)]
pub enum AuthError {
    UnknownSession,
    BadToken,
}

impl SessionStore {
    pub fn new(cap: usize, ttl: Duration) -> Self {
        Self {
            sessions: HashMap::new(),
            cap,
            ttl,
        }
    }

    /// Evict every session idle longer than the TTL. Called opportunistically on
    /// each mutating access; `now` is injected for testability.
    pub fn evict_expired(&mut self, now: Instant) {
        let ttl = self.ttl;
        self.sessions
            .retain(|_, s| now.duration_since(s.last_seen) < ttl);
    }

    /// Register a freshly minted (id, token). Evicts expired sessions first;
    /// returns false (caller -> 503) if the store is still at capacity.
    pub fn insert(&mut self, id: String, token: String, now: Instant) -> bool {
        self.evict_expired(now);
        if self.sessions.len() >= self.cap && !self.sessions.contains_key(&id) {
            return false;
        }
        self.sessions.insert(
            id,
            Session {
                token,
                target: None,
                last_seen: now,
            },
        );
        true
    }

    /// Validate a session id + token in constant time, and (on success) refresh
    /// the idle timer. Returns a mutable handle so the caller can read/set the
    /// target under the same lock.
    pub fn authenticate(
        &mut self,
        id: &str,
        token: &str,
        now: Instant,
    ) -> Result<&mut Session, AuthError> {
        self.evict_expired(now);
        let session = self.sessions.get_mut(id).ok_or(AuthError::UnknownSession)?;
        if !crate::security::constant_time_eq(&session.token, token) {
            return Err(AuthError::BadToken);
        }
        session.last_seen = now;
        Ok(session)
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.sessions.len()
    }

    #[cfg(test)]
    pub fn is_empty(&self) -> bool {
        self.sessions.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn store() -> SessionStore {
        SessionStore::new(3, Duration::from_secs(60))
    }

    #[test]
    fn insert_and_authenticate() {
        let mut s = store();
        let t0 = Instant::now();
        assert!(s.insert("sess1".into(), "tok1".into(), t0));
        assert!(s.authenticate("sess1", "tok1", t0).is_ok());
        assert_eq!(
            s.authenticate("sess1", "bad", t0).unwrap_err(),
            AuthError::BadToken
        );
        assert_eq!(
            s.authenticate("nope", "tok1", t0).unwrap_err(),
            AuthError::UnknownSession
        );
    }

    #[test]
    fn ttl_eviction() {
        let mut s = SessionStore::new(10, Duration::from_secs(60));
        let t0 = Instant::now();
        s.insert("sess1".into(), "tok1".into(), t0);
        let later = t0 + Duration::from_secs(61);
        assert_eq!(
            s.authenticate("sess1", "tok1", later).unwrap_err(),
            AuthError::UnknownSession
        );
        assert!(s.is_empty());
    }

    #[test]
    fn access_refreshes_idle_timer() {
        let mut s = SessionStore::new(10, Duration::from_secs(60));
        let t0 = Instant::now();
        s.insert("sess1".into(), "tok1".into(), t0);
        // Access at t+30 refreshes; at t+80 (50s after the refresh) still alive.
        assert!(s
            .authenticate("sess1", "tok1", t0 + Duration::from_secs(30))
            .is_ok());
        assert!(s
            .authenticate("sess1", "tok1", t0 + Duration::from_secs(80))
            .is_ok());
    }

    #[test]
    fn capacity_is_enforced_after_eviction() {
        let mut s = SessionStore::new(2, Duration::from_secs(60));
        let t0 = Instant::now();
        assert!(s.insert("a".into(), "t".into(), t0));
        assert!(s.insert("b".into(), "t".into(), t0));
        // At cap: a new id is refused.
        assert!(!s.insert("c".into(), "t".into(), t0));
        // But once an existing one expires, room frees up.
        let later = t0 + Duration::from_secs(61);
        assert!(s.insert("c".into(), "t".into(), later));
        assert_eq!(s.len(), 1);
    }
}
