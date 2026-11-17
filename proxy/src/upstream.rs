// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// upstream.rs — SigV4-sign a DynamoDB request and forward it (phase 6f, §3).
//
// This is the whole "credentialed relay": take the AWS-JSON-1.0 body the browser
// produced (via ddbsh-wire) and the operation name, SigV4-sign it with the
// session's resolved credentials for the session's region, and POST it to the
// session's endpoint over the per-endpoint-trust HTTP client. The response
// (status + body) is relayed back verbatim — a DynamoDB service error arrives as
// a 4xx/5xx with a `__type` JSON document, which the browser hands straight to
// the ddbsh-wire codec to map to a faithful ddbsh error string (D-P6d-1).
//
// Credentials live only here and in the session store; they never cross the
// browser seam.

use std::time::SystemTime;

use aws_credential_types::Credentials;
use aws_sigv4::http_request::{sign, SignableBody, SignableRequest, SigningSettings};
use aws_sigv4::sign::v4;
use bytes::Bytes;

use crate::sessions::Target;

/// The AWS-JSON-1.0 content type DynamoDB speaks.
pub const DDB_CONTENT_TYPE: &str = "application/x-amz-json-1.0";

/// The DynamoDB X-Amz-Target service prefix (API version 2012-08-10).
const DDB_TARGET_PREFIX: &str = "DynamoDB_20120810.";

/// A relayed upstream response: the HTTP status and body exactly as DynamoDB /
/// ExtendDB returned them.
pub struct Relayed {
    pub status: reqwest::StatusCode,
    pub body: Bytes,
}

/// SigV4-sign `operation`/`body` for `target` with `credentials` and forward it
/// over `client`. Returns the relayed response, or a transport-level error
/// string (the endpoint was unreachable, TLS failed, the signer rejected the
/// inputs) that the caller surfaces as a 502 the browser treats as a transport
/// failure.
pub async fn sign_and_forward(
    client: &reqwest::Client,
    target: &Target,
    credentials: &Credentials,
    operation: &str,
    body: Bytes,
) -> Result<Relayed, String> {
    let x_amz_target = format!("{DDB_TARGET_PREFIX}{operation}");

    // --- Build the SigV4 signature over the exact headers we will send -------
    let identity = credentials.clone().into();
    let signing_settings = SigningSettings::default();
    let signing_params = v4::SigningParams::builder()
        .identity(&identity)
        .region(&target.signing_region)
        .name("dynamodb")
        .time(SystemTime::now())
        .settings(signing_settings)
        .build()
        .map_err(|e| format!("sigv4 params: {e}"))?;
    let signing_params = signing_params.into();

    // The signed header set. `host` must match exactly what reqwest sends from
    // the URL authority (see Target::host).
    let headers_to_sign = [
        ("host", target.host.as_str()),
        ("x-amz-target", x_amz_target.as_str()),
        ("content-type", DDB_CONTENT_TYPE),
    ];
    let signable = SignableRequest::new(
        "POST",
        &target.endpoint_url,
        headers_to_sign.iter().map(|(n, v)| (*n, *v)),
        SignableBody::Bytes(&body),
    )
    .map_err(|e| format!("sigv4 signable: {e}"))?;

    let (instructions, _signature) = sign(signable, &signing_params)
        .map_err(|e| format!("sigv4 sign: {e}"))?
        .into_parts();

    // --- Assemble and send the request --------------------------------------
    // reqwest sets Host from the URL authority (== target.host, which we signed).
    let mut request = client
        .post(&target.endpoint_url)
        .header("x-amz-target", &x_amz_target)
        .header("content-type", DDB_CONTENT_TYPE);
    // Apply the signer-produced headers (x-amz-date, authorization, and
    // x-amz-security-token when the credentials are temporary).
    for (name, value) in instructions.headers() {
        request = request.header(name, value);
    }
    request = request.body(body);

    let response = request
        .send()
        .await
        .map_err(|e| format!("forward to {}: {e}", target.endpoint_url))?;
    let status = response.status();
    let body = response
        .bytes()
        .await
        .map_err(|e| format!("read upstream body: {e}"))?;
    Ok(Relayed { status, body })
}
