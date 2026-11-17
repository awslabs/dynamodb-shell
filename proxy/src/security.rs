// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// security.rs — the two code-level defenses of ddbsh-web-proxy (phase 6f, §4):
//
//   1. The egress policy (D-P6f-6). A user-owned, startup-only allow/block list
//      of anchored host regexes. An endpoint is permitted iff its canonical host
//      matches at least one `allow` and zero `block` entries (block wins). The
//      default policy allows loopback plus the DynamoDB endpoint host families
//      across partitions, and blocks the IMDS address as cheap SSRF defense.
//
//   2. The driver guard (D-P6f-5). Loopback ingress stops other *machines*; this
//      stops a malicious page open in the *same browser* from driving the proxy
//      (confused-deputy / DNS-rebinding): every /session, /connect, /ddb request
//      must carry an `Origin` and `Host` that name the served page's own
//      loopback origin, and /connect + /ddb additionally require the
//      per-session token minted at /session.
//
// Both are pure logic with no I/O, unit-tested below. The signing/forwarding
// path (main.rs) is exercised by the real-DynamoDB Node harness.

use std::path::Path;

use regex::Regex;

/// The canonical host of a URL for policy matching: the lowercased host with any
/// userinfo and fragment stripped, and the port preserved (so a policy can pin a
/// port). Returns `None` if the URL has no host (e.g. a relative or opaque URL),
/// which the caller treats as "not permitted". Parsing (not string matching)
/// defeats userinfo/encoding bypasses such as `https://dynamodb.us-east-1.amazonaws.com@evil.com/`.
pub fn canonical_host(endpoint: &str) -> Option<String> {
    let parsed = url::Url::parse(endpoint).ok()?;
    let host = parsed.host_str()?.to_ascii_lowercase();
    match parsed.port() {
        Some(port) => Some(format!("{host}:{port}")),
        None => Some(host),
    }
}

/// One egress-policy rule: an anchored regex and whether it allows or blocks.
#[derive(Debug, Clone)]
struct Rule {
    regex: Regex,
    allow: bool,
}

/// The egress allow/block policy (D-P6f-6). An endpoint host is permitted iff it
/// matches >= 1 allow rule AND 0 block rules. Read once at startup; edit the
/// file and restart to apply.
#[derive(Debug, Clone)]
pub struct EgressPolicy {
    rules: Vec<Rule>,
}

impl EgressPolicy {
    /// The built-in default policy (§4): loopback hosts (any port) plus the
    /// DynamoDB endpoint host families across the aws / aws-us-gov / aws-cn
    /// partitions (FIPS and dual-stack `.api.aws` included), with the IMDS link-
    /// local address explicitly blocked. The families are expressed as anchored
    /// regexes derived from the DynamoDB endpoint naming scheme rather than a
    /// literal per-region list, so a new region needs no policy edit while the
    /// relay still cannot be pointed at an arbitrary host.
    pub fn default_policy() -> Self {
        Self::from_lines(DEFAULT_POLICY_TEXT).expect("built-in default policy must compile")
    }

    /// Load a policy from a file if it exists, else the built-in default. A
    /// present-but-malformed file is an error the caller surfaces at startup (a
    /// broken security policy must never silently fall back to a weaker one).
    pub fn load_or_default(path: &Path) -> Result<Self, String> {
        match std::fs::read_to_string(path) {
            Ok(text) => Self::from_lines(&text)
                .map_err(|e| format!("egress policy {}: {e}", path.display())),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Self::default_policy()),
            Err(e) => Err(format!("egress policy {}: {e}", path.display())),
        }
    }

    /// Parse policy text. Each non-empty, non-comment line is
    /// `allow <regex>` or `block <regex>`. The regex is anchored (`^...$`) before
    /// compilation so a rule matches the WHOLE canonical host, never a substring.
    pub fn from_lines(text: &str) -> Result<Self, String> {
        let mut rules = Vec::new();
        for (lineno, raw) in text.lines().enumerate() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let (kind, pattern) = line.split_once(char::is_whitespace).ok_or_else(|| {
                format!(
                    "line {}: expected `allow <regex>` or `block <regex>`",
                    lineno + 1
                )
            })?;
            let allow = match kind {
                "allow" => true,
                "block" => false,
                other => {
                    return Err(format!("line {}: unknown rule kind `{other}`", lineno + 1));
                }
            };
            let anchored = format!("^(?:{})$", pattern.trim());
            let regex = Regex::new(&anchored)
                .map_err(|e| format!("line {}: bad regex: {e}", lineno + 1))?;
            rules.push(Rule { regex, allow });
        }
        if !rules.iter().any(|r| r.allow) {
            return Err("policy has no `allow` rules; every endpoint would be denied".to_string());
        }
        Ok(Self { rules })
    }

    /// Whether `endpoint` is permitted. A URL with no parseable host, or one
    /// matching no allow rule, or one matching any block rule, is denied.
    pub fn permits(&self, endpoint: &str) -> bool {
        let Some(host) = canonical_host(endpoint) else {
            return false;
        };
        let mut allowed = false;
        for rule in &self.rules {
            if rule.regex.is_match(&host) {
                if rule.allow {
                    allowed = true;
                } else {
                    return false; // block wins, immediately
                }
            }
        }
        allowed
    }
}

/// The default egress policy text, embedded so a fresh install is safe with no
/// config file. A user who needs a remote ExtendDB or a gamma/internal endpoint
/// adds a one-line `allow` to `~/.ddbsh_web_proxy_security` and restarts.
pub const DEFAULT_POLICY_TEXT: &str = r#"
# ddbsh-web-proxy default egress policy (phase 6f, D-P6f-6).
# Each line: `allow <regex>` or `block <regex>`, matched (anchored) against the
# canonical endpoint host. An endpoint is permitted iff it matches >= 1 allow and
# 0 block rules; block always wins. Edit this file and restart to apply.

# Loopback (any port) — local ExtendDB and the proxy's own health checks.
allow localhost
allow 127\.0\.0\.1
allow 127\.0\.0\.1:\d{1,5}
allow localhost:\d{1,5}
allow \[::1\]
allow \[::1\]:\d{1,5}

# Amazon DynamoDB endpoint host families (derived from the DynamoDB endpoint
# naming scheme; covers all regions in the aws / aws-us-gov partitions without a
# per-region list). FIPS variants and dual-stack (.api.aws) included.
allow dynamodb\.[a-z0-9-]+\.amazonaws\.com
allow dynamodb-fips\.[a-z0-9-]+\.amazonaws\.com
allow dynamodb\.[a-z0-9-]+\.api\.aws
allow dynamodb-fips\.[a-z0-9-]+\.api\.aws

# China partition (aws-cn).
allow dynamodb\.[a-z0-9-]+\.amazonaws\.com\.cn
allow dynamodb-fips\.[a-z0-9-]+\.amazonaws\.com\.cn

# Defense in depth: never let the credentialed relay be pointed at the instance
# metadata service, regardless of any allow rule above (block wins).
block 169\.254\.169\.254
block 169\.254\.169\.254:\d{1,5}
block \[fd00:ec2::254\]
"#;

/// The set of loopback origins and hosts the served page legitimately uses, for
/// the driver guard. Built from the bound port.
#[derive(Debug, Clone)]
pub struct LocalIdentity {
    hosts: Vec<String>,
    origins: Vec<String>,
}

impl LocalIdentity {
    pub fn new(port: u16) -> Self {
        let hosts = vec![
            format!("127.0.0.1:{port}"),
            format!("localhost:{port}"),
            format!("[::1]:{port}"),
        ];
        let origins = hosts.iter().map(|h| format!("http://{h}")).collect();
        Self { hosts, origins }
    }

    /// A request's `Host` header must name one of our loopback authorities
    /// (defeats DNS-rebinding, where `Origin` is the attacker's domain but the
    /// request resolves to us).
    pub fn host_ok(&self, host: Option<&str>) -> bool {
        match host {
            Some(h) => self.hosts.iter().any(|allowed| allowed == h),
            None => false,
        }
    }

    /// A request's `Origin` header must name the served page's own origin (a
    /// cross-origin page gets its own domain here, which fails). Absence fails
    /// closed: a legitimate browser `fetch()` to the proxy always sends `Origin`.
    pub fn origin_ok(&self, origin: Option<&str>) -> bool {
        match origin {
            Some(o) => self.origins.iter().any(|allowed| allowed == o),
            None => false,
        }
    }

    pub fn primary_origin(&self) -> &str {
        &self.origins[0]
    }
}

/// Constant-time string comparison for the per-session token, so a timing side
/// channel cannot be used to guess it byte by byte.
pub fn constant_time_eq(a: &str, b: &str) -> bool {
    let a = a.as_bytes();
    let b = b.as_bytes();
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

/// Validate the X-Amz-Target operation suffix supplied by the browser before it
/// is interpolated into a signed `X-Amz-Target` header. DynamoDB operation names
/// are ASCII alphanumerics (e.g. `GetItem`, `TransactWriteItems`); rejecting
/// anything else prevents header-injection / request-smuggling via a crafted op.
pub fn valid_operation(op: &str) -> bool {
    !op.is_empty()
        && op.len() <= 64
        && op.chars().all(|c| c.is_ascii_alphanumeric())
        && op.chars().next().is_some_and(|c| c.is_ascii_alphabetic())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonical_host_strips_userinfo_and_lowercases() {
        assert_eq!(
            canonical_host("https://DynamoDB.US-East-1.amazonaws.com/"),
            Some("dynamodb.us-east-1.amazonaws.com".to_string())
        );
        // A userinfo bypass attempt: the real host is evil.com.
        assert_eq!(
            canonical_host("https://dynamodb.us-east-1.amazonaws.com@evil.com/"),
            Some("evil.com".to_string())
        );
        assert_eq!(
            canonical_host("https://127.0.0.1:18443/"),
            Some("127.0.0.1:18443".to_string())
        );
        assert_eq!(canonical_host("not a url"), None);
    }

    #[test]
    fn default_policy_allows_dynamodb_and_loopback() {
        let p = EgressPolicy::default_policy();
        assert!(p.permits("https://dynamodb.us-east-1.amazonaws.com"));
        assert!(p.permits("https://dynamodb.us-west-2.amazonaws.com"));
        assert!(p.permits("https://dynamodb.eu-central-1.amazonaws.com"));
        assert!(p.permits("https://dynamodb-fips.us-east-1.amazonaws.com"));
        assert!(p.permits("https://dynamodb.us-east-1.api.aws"));
        assert!(p.permits("https://dynamodb.us-gov-west-1.amazonaws.com"));
        assert!(p.permits("https://dynamodb.cn-north-1.amazonaws.com.cn"));
        assert!(p.permits("https://127.0.0.1:18443"));
        assert!(p.permits("https://localhost:18443"));
    }

    #[test]
    fn default_policy_denies_arbitrary_and_imds() {
        let p = EgressPolicy::default_policy();
        assert!(!p.permits("https://evil.com"));
        assert!(!p.permits("https://dynamodb.us-east-1.amazonaws.com.evil.com"));
        assert!(!p.permits("https://s3.us-east-1.amazonaws.com"));
        // IMDS is blocked even though no allow rule names it anyway.
        assert!(!p.permits("http://169.254.169.254/latest/meta-data/"));
        // A userinfo bypass: real host evil.com, denied.
        assert!(!p.permits("https://dynamodb.us-east-1.amazonaws.com@evil.com/"));
    }

    #[test]
    fn block_wins_over_allow() {
        let p =
            EgressPolicy::from_lines("allow .*\nblock dynamodb\\.secret\\.example\\.com").unwrap();
        assert!(p.permits("https://anything.example.com"));
        assert!(!p.permits("https://dynamodb.secret.example.com"));
    }

    #[test]
    fn policy_requires_an_allow_rule() {
        assert!(EgressPolicy::from_lines("# nothing\nblock evil\\.com").is_err());
        assert!(EgressPolicy::from_lines("allow foo bar baz").is_ok()); // split_once: pattern = "foo bar baz" -> regex
    }

    #[test]
    fn policy_rejects_unknown_kind_and_bad_regex() {
        assert!(EgressPolicy::from_lines("permit localhost").is_err());
        assert!(EgressPolicy::from_lines("allow (").is_err());
    }

    #[test]
    fn anchored_match_is_whole_host() {
        let p = EgressPolicy::from_lines("allow dynamodb\\.us-east-1\\.amazonaws\\.com").unwrap();
        assert!(p.permits("https://dynamodb.us-east-1.amazonaws.com"));
        // A substring must not match: prefix/suffix attacks are rejected.
        assert!(!p.permits("https://xdynamodb.us-east-1.amazonaws.com"));
        assert!(!p.permits("https://dynamodb.us-east-1.amazonaws.com.evil.net"));
    }

    #[test]
    fn local_identity_guards_origin_and_host() {
        let id = LocalIdentity::new(8080);
        assert!(id.origin_ok(Some("http://127.0.0.1:8080")));
        assert!(id.origin_ok(Some("http://localhost:8080")));
        assert!(!id.origin_ok(Some("http://evil.com")));
        assert!(!id.origin_ok(None)); // fail closed
        assert!(id.host_ok(Some("127.0.0.1:8080")));
        assert!(id.host_ok(Some("localhost:8080")));
        assert!(!id.host_ok(Some("evil.com:8080")));
        assert!(!id.host_ok(None));
    }

    #[test]
    fn constant_time_eq_matches_semantics() {
        assert!(constant_time_eq("abc", "abc"));
        assert!(!constant_time_eq("abc", "abd"));
        assert!(!constant_time_eq("abc", "abcd"));
        assert!(!constant_time_eq("", "x"));
        assert!(constant_time_eq("", ""));
    }

    #[test]
    fn valid_operation_accepts_ops_rejects_injection() {
        assert!(valid_operation("GetItem"));
        assert!(valid_operation("TransactWriteItems"));
        assert!(!valid_operation(""));
        assert!(!valid_operation("Get Item"));
        assert!(!valid_operation("GetItem\r\nX-Evil: 1"));
        assert!(!valid_operation("Get.Item"));
        assert!(!valid_operation("1GetItem")); // must start with a letter
    }
}
