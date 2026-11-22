<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Hosting the browser edition

The browser edition is a static page: HTML, JavaScript, CSS and a WebAssembly
module. It has no server-side component. Visitors paste their own short-term
AWS credentials into it, and the page calls DynamoDB directly from their
browser (see [configuration 3](configurations/03-browser-aws.md)).

This guide publishes the page on AWS: a private S3 bucket that only a
CloudFront distribution can read, deployed with one CloudFormation template
(`web/hosting/aws/ddbsh-web-hosting.yaml`). The last section lists what any
other host has to provide.

## Contents

- [Before you start: where the page may live](#before-you-start-where-the-page-may-live)
- [What you need](#what-you-need)
- [1. Build and package the page](#1-build-and-package-the-page)
- [2. Create the bucket and the distribution](#2-create-the-bucket-and-the-distribution)
- [3. Upload the page](#3-upload-the-page)
- [4. Check the deployment](#4-check-the-deployment)
- [Use your own domain name](#use-your-own-domain-name)
- [Publish a new version](#publish-a-new-version)
- [Command history in the browser](#command-history-in-the-browser)
- [Remove everything](#remove-everything)
- [Cost](#cost)
- [Other hosts](#other-hosts)

## Before you start: where the page may live

The page handles AWS credentials, so where you host it matters more than for
an ordinary static site.

- **Give the page its own origin**: its own host name, such as
  `ddbsh.example.com` or the distribution's `d1234abcd.cloudfront.net` name.
  Every script that runs on the same origin can read what a visitor pastes
  into the page. Never put the page under a path of a blog, a CMS or another
  site (`example.com/ddbsh/`), where themes, plugins and analytics scripts
  share the origin. Never embed it in another page; the page refuses
  credentials when it is inside a frame or was opened by another page's
  script.
- **Whoever can change the files or the distribution can steal credentials.**
  Write access to the bucket, or to the CloudFront distribution's settings,
  lets someone serve a modified page that sends visitors' credentials
  elsewhere. Give that access to as few people and roles as possible, and
  protect them as you would the credentials themselves. The template can
  create a narrow deployer policy for this (see step 2). With your own domain
  name, the same holds for whoever controls the name's DNS records or your
  account at the DNS provider: they can point the name at a server of their
  own, get a certificate for it, and serve a modified page there.
- **No sign-in is needed.** The page is public, open-source content and holds
  no secrets. What a visitor can do is decided by the AWS credentials they
  paste, so they should paste short-term credentials scoped to the tables they
  need ([configuration 3](configurations/03-browser-aws.md#short-term-credentials)).

## What you need

- The build tools for the browser edition: CMake, flex, bison, the pinned
  Emscripten SDK and Node.js 20 or later
  ([BUILDING](BUILDING.md#build-the-browser-edition)).
- The AWS CLI version 2, with credentials and a default region configured
  (`aws configure`). The examples use bash; where PowerShell syntax differs,
  it is shown too (PowerShell 7 or later).
- Permission to create a CloudFormation stack with an S3 bucket, a bucket
  policy and CloudFront resources.

## 1. Build and package the page

From the repository root:

```sh
emcmake cmake --preset wasm-release
cmake --build build-preset/wasm-release --target ddbsh-web-package
```

The packaging step writes three things to `build-preset/wasm-release/dist/`:

| Output | What it is |
|---|---|
| `ddbsh-web-<version>/` | exactly the 11 files the page needs (listed in `web/bundle-files.txt`), and nothing else from the build folder |
| `ddbsh-web-<version>.zip` | the same folder as a zip |
| `SHA256SUMS` | the SHA-256 of every file in the folder and of the zip |

Publish only the `ddbsh-web-<version>/` folder. Check that it is intact,
which matters most when the folder was copied or handed to you:

```sh
cd build-preset/wasm-release/dist && sha256sum -c SHA256SUMS && cd -
```

On macOS use `shasum -a 256 -c SHA256SUMS`. In PowerShell:

```powershell
Push-Location build-preset/wasm-release/dist
Get-Content SHA256SUMS | ForEach-Object { $hash, $file = $_ -split '  ', 2; if ((Get-FileHash -Algorithm SHA256 $file).Hash -eq $hash) { "OK $file" } else { "MISMATCH $file" } }
Pop-Location
```

Every line must say `OK`.

## 2. Create the bucket and the distribution

```sh
aws cloudformation deploy --stack-name ddbsh-web --template-file web/hosting/aws/ddbsh-web-hosting.yaml
```

Pick a stack name of up to 60 characters; the template derives its resource
names from it, and the origin access control's name (`<stack name>-oac`) may
have at most 64. CloudFront and IAM names are unique across the whole account,
so do not use the same stack name for this template in two regions. The
command waits until the stack is ready, which takes several minutes because
CloudFront deploys the distribution worldwide. The stack can be in any
region.

The template creates:

- **A private S3 bucket**: all four Block Public Access settings on, object
  ownership enforced (no ACLs), SSE-S3 encryption, versioning on, and old
  object versions deleted after 90 days.
- **A bucket policy** that lets CloudFront read objects only for this
  distribution (`AWS:SourceArn`), and refuses any request made without TLS.
- **Origin Access Control**: CloudFront signs every request to the bucket.
- **A response headers policy** that adds these headers to every response:

  | Header | Value | Why |
  |---|---|---|
  | `Content-Security-Policy` | `frame-ancestors 'none'` | no page may frame this one. The page's full policy is in a `<meta>` tag in `index.html`, which cannot carry `frame-ancestors`; browsers enforce both. |
  | `X-Frame-Options` | `DENY` | the same, for older browsers |
  | `Strict-Transport-Security` | `max-age=31536000` | browsers use HTTPS only for a year |
  | `X-Content-Type-Options` | `nosniff` | each file is used only as its declared type |
  | `Referrer-Policy` | `no-referrer` | requests never reveal the page's address |
  | `Cross-Origin-Opener-Policy` | `same-origin` | a page on another origin that opens this one gets no handle to it |
  | `Cross-Origin-Resource-Policy` | `same-origin` | other sites cannot load these files |
  | `Permissions-Policy` | camera, microphone, geolocation, payment, USB and similar features off | the page uses none of them |

- **A CloudFront distribution**: HTTPS only (HTTP redirects to HTTPS),
  `index.html` as the default page, HTTP/2 and HTTP/3, compression on, and
  the edge locations of price class 100 (North America, Europe and Israel;
  visitors elsewhere are served from those).

Options, passed as `--parameter-overrides Name=value`:

| Parameter | Default | Meaning |
|---|---|---|
| `CreateDeployerPolicy` | `false` | `true` also creates an IAM managed policy, `<stack name>-deployer`, attached to no one. It allows only the upload in step 3: list the bucket, put and delete its objects, and invalidate the distribution. Attach it to the role or user that publishes the page, and give them the bucket name and distribution id (the policy does not allow reading the stack). Add `--capabilities CAPABILITY_NAMED_IAM` to the deploy command when you set it, and to every later deploy of the stack. The policy has been checked by the CloudFormation linter and IAM Access Analyzer but not yet deployed by this project's tests. |
| `DomainName`, `AcmCertificateArn` | empty | your own domain name; see [Use your own domain name](#use-your-own-domain-name) |
| `Purpose` | `hosting` | the value of the `purpose` tag on the bucket and the distribution (they also carry `project=ddbsh-web`) |

Access logging is off. Add it to the distribution in the template if you need
it.

Now read the stack's outputs into variables. `DIR` is the packaged folder from
step 1, `build-preset/wasm-release/dist/ddbsh-web-<version>`; the last line
finds it (the packaging step leaves exactly one there).

```sh
STACK=ddbsh-web
BUCKET=$(aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='BucketName'].OutputValue" --output text)
DIST_ID=$(aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='DistributionId'].OutputValue" --output text)
DOMAIN=$(aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='DistributionDomainName'].OutputValue" --output text)
BUCKET_DOMAIN=$(aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='BucketRegionalDomainName'].OutputValue" --output text)
DIR=$(ls -d build-preset/wasm-release/dist/ddbsh-web-*/)
```

```powershell
$STACK = "ddbsh-web"
$BUCKET = aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='BucketName'].OutputValue" --output text
$DIST_ID = aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='DistributionId'].OutputValue" --output text
$DOMAIN = aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='DistributionDomainName'].OutputValue" --output text
$BUCKET_DOMAIN = aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='BucketRegionalDomainName'].OutputValue" --output text
$DIR = (Get-ChildItem -Directory -Path build-preset/wasm-release/dist -Filter "ddbsh-web-*").FullName
```

## 3. Upload the page

These commands are the same in bash, zsh and PowerShell. Run them from the
repository root. In Git Bash on Windows, run the last one as
`MSYS_NO_PATHCONV=1 aws cloudfront create-invalidation …`: Git Bash otherwise
rewrites `/*` into a Windows path, and CloudFront rejects the invalidation.

```sh
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.html" --content-type "text/html; charset=utf-8" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.js" --include "*.mjs" --content-type "text/javascript" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.wasm" --content-type "application/wasm" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.css" --content-type "text/css; charset=utf-8" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.json" --content-type "application/json" --cache-control "no-cache"
aws s3 sync "$DIR" "s3://$BUCKET/" --delete --exclude "*" --include "*.txt" --content-type "text/plain; charset=utf-8" --cache-control "no-cache"
aws cloudfront create-invalidation --distribution-id "$DIST_ID" --paths "/*"
```

Why one command per file type:

- Each sets the `Content-Type` browsers need. A JavaScript module served with
  another type does not load, and `ddbsh.wasm` must be `application/wasm`.
- `--delete` removes files of that type that are no longer in the bundle.
- Every file is marked `no-cache`. A browser may keep a copy, but it checks
  with CloudFront before each use, and an unchanged file costs a short
  `304 Not Modified` reply. The file names carry no version, so with a longer
  cache time a returning visitor's browser could run a new `index.html` with
  an older `app.js`.
- `aws s3 sync` uploads a file when its size changed or the local copy is
  newer than the one in the bucket. The packaging step writes every file
  fresh, so upload right after packaging.

The invalidation clears CloudFront's cached copies; it finishes within a few
minutes. A visitor who loads the page while these commands run can get a mix
of old and new files. Once the invalidation has finished, a reload loads one
version. On Linux or macOS, `web/hosting/aws/deploy.sh "$DIR" "$BUCKET"
"$DIST_ID"` runs the same seven commands.

## 4. Check the deployment

Each command prints what to compare with the comment above it.

```sh
# 200, Content-Type text/html; charset=utf-8, Cache-Control no-cache, and the
# eight headers from step 2
curl -sSI "https://$DOMAIN/" | grep -iE "^(HTTP|content-type|cache-control|content-security-policy|x-frame-options|strict-transport-security|x-content-type-options|referrer-policy|cross-origin-opener-policy|cross-origin-resource-policy|permissions-policy)"
# application/wasm
curl -sSI "https://$DOMAIN/ddbsh.wasm" | grep -i "^content-type"
# text/javascript, and Cache-Control no-cache
curl -sSI "https://$DOMAIN/app.js" | grep -iE "^(content-type|cache-control)"
# 301, and a Location that starts with https://
curl -sSI "http://$DOMAIN/" | grep -iE "^(HTTP|location)"
# 403: the bucket is private; only CloudFront can read it
curl -sSI "https://$BUCKET_DOMAIN/index.html" | grep -i "^HTTP"
```

In PowerShell, replace `| grep -iE "<pattern>"` with
`| Select-String -Pattern "<pattern>"`. On Windows, write `curl.exe`, as
below, so that Windows PowerShell's `curl` alias for `Invoke-WebRequest` is
never used; on macOS and Linux, write `curl`.

```powershell
curl.exe -sSI "https://$DOMAIN/" | Select-String -Pattern "^(HTTP|content-type|cache-control|content-security-policy|x-frame-options|strict-transport-security|x-content-type-options|referrer-policy|cross-origin-opener-policy|cross-origin-resource-policy|permissions-policy)"
curl.exe -sSI "https://$DOMAIN/ddbsh.wasm" | Select-String -Pattern "^content-type"
curl.exe -sSI "https://$DOMAIN/app.js" | Select-String -Pattern "^(content-type|cache-control)"
curl.exe -sSI "http://$DOMAIN/" | Select-String -Pattern "^(HTTP|location)"
curl.exe -sSI "https://$BUCKET_DOMAIN/index.html" | Select-String -Pattern "^HTTP"
```

Then open `https://$DOMAIN/` in a browser. The header shows
`backend: Amazon DynamoDB (AWS SDK for JavaScript …)` once the page has
loaded. Paste short-term credentials and run `show connection;`.

The distribution answers 403, not 404, for a file that is not in the bundle:
CloudFront may not list the bucket, so S3 does not reveal whether a name
exists. A browser's developer tools may show one such 403 for `/favicon.ico`,
which the bundle does not include. Both are expected.

## Use your own domain name

A name such as `ddbsh.example.com` works with any DNS provider. It must be a
name that serves nothing else (see [where the page may
live](#before-you-start-where-the-page-may-live)). Whoever can change its DNS
records can point it at a server of their own and serve a modified page with a
valid certificate, so protect your DNS provider account as you protect the
bucket. The template's domain parameters have been checked by the
CloudFormation linter but not yet deployed by this project's tests.

1. Request a certificate. CloudFront accepts only certificates issued in
   us-east-1, whatever region your stack is in:

   ```sh
   aws acm request-certificate --domain-name ddbsh.example.com --validation-method DNS --region us-east-1
   ```

   The command prints the certificate ARN.

2. Show the DNS record that proves you control the name, and add it at your
   DNS provider as a CNAME:

   ```sh
   aws acm describe-certificate --certificate-arn <certificate-arn> --region us-east-1 --query "Certificate.DomainValidationOptions[0].ResourceRecord"
   ```

   Right after the request, this can print `null` for a few seconds; repeat it
   until it prints the record's `Name` and `Value`.

3. Wait until the certificate is issued:

   ```sh
   aws acm wait certificate-validated --certificate-arn <certificate-arn> --region us-east-1
   ```

4. Add the name to the distribution:

   ```sh
   aws cloudformation deploy --stack-name ddbsh-web --template-file web/hosting/aws/ddbsh-web-hosting.yaml --parameter-overrides DomainName=ddbsh.example.com AcmCertificateArn=<certificate-arn>
   ```

   With a domain name, viewers must use TLS 1.2 or later. If the stack was
   created with `CreateDeployerPolicy=true`, add
   `--capabilities CAPABILITY_NAMED_IAM` to this command: a later deploy keeps
   the parameters you do not repeat, so the stack still has the policy.

5. At your DNS provider, add a CNAME from `ddbsh.example.com` to the
   distribution's name (`$DOMAIN`, the `DistributionDomainName` output). A DNS
   provider that does not allow a CNAME at the top of a zone (`example.com`
   itself) needs its own alias or flattening feature, or use a subdomain.

6. Set `DOMAIN=ddbsh.example.com` (PowerShell: `$DOMAIN = "ddbsh.example.com"`)
   and run the checks in step 4 again.

## Publish a new version

1. Update the source (for example `git pull`), then build and package again
   with the two commands in step 1. The folder name carries the new version.
2. Check `SHA256SUMS` as in step 1.
3. Set `DIR` again with the last line of the step 2 variables (it finds the
   new folder), then run the seven commands in step 3. They upload the changed
   files, delete files the new version no longer has, and clear CloudFront's
   cache.
4. Run the checks in step 4.

If the template itself changed, run the `aws cloudformation deploy` command
from step 2 again first; it updates the stack in place and keeps the
parameters you set before. Add `--capabilities CAPABILITY_NAMED_IAM` if the
stack has the deployer policy.

To roll back, publish the earlier release the same way: check it out instead
of pulling (for example `git checkout v0.12.1`), then follow steps 1–4 of this
section. This works back to 0.12.1, the first release with the packaging step.
The files of one release work only together, so do not restore single files
from the bucket's earlier versions. Upload only a freshly packaged folder:
`aws s3 sync` skips a file whose size is unchanged and whose local copy is
older than the bucket's, so uploading an older copy of a folder can leave some
files at the newer release.

## Command history in the browser

The page saves the statements you run from the editor (up to 500) in the
browser's local storage for the page's origin, so the up arrow recalls them
after a reload. Statements loaded from a file are not saved. Credentials are
never saved anywhere.

Saved statements include any values you typed into them. To delete them,
press **Clear history** in the toolbar to the right of the editor, below
**Clear**. It also empties the history of the page's other open tabs, so none
of them saves the deleted statements again. **Clear** only empties the
transcript. Anything that runs on the page's origin can read local storage,
which is one more reason to give the page its own origin.

## Remove everything

CloudFormation cannot delete a bucket that still holds objects, so empty it
first, including the earlier versions that versioning keeps. With the
variables from step 2 set, list the object versions and delete them:

```sh
aws s3api list-object-versions --bucket "$BUCKET" --no-paginate --query "{Objects: [Versions, DeleteMarkers][].{Key: Key, VersionId: VersionId}}" --output json > versions.json
aws s3api delete-objects --bucket "$BUCKET" --delete file://versions.json
```

Each pass lists and deletes up to 1000 entries (object versions and delete
markers together), which is the most one `delete-objects` call accepts. A
bucket that has held a few releases empties in one pass; with more entries,
run both commands again until `versions.json` reads `"Objects": []`. When the
list is empty, `delete-objects` reports a `MalformedXML` error, which you can
ignore. (The S3 console's **Empty** button also empties a bucket.)

If you attached the deployer policy to a role, user or group, detach it
first: `aws iam detach-role-policy --role-name <role> --policy-arn <arn>`,
or `detach-user-policy --user-name <user>` or
`detach-group-policy --group-name <group>` with the same `--policy-arn`. The
ARN is the stack's `DeployerPolicyArn` output.

Then delete the stack and the list file:

```sh
aws cloudformation delete-stack --stack-name "$STACK"
aws cloudformation wait stack-delete-complete --stack-name "$STACK"
rm versions.json
```

All the commands in this section are the same in PowerShell 7, except
`rm versions.json`, which is `Remove-Item versions.json` there. Deleting the
stack takes several minutes while CloudFront removes the distribution. If you
requested a certificate for your own domain name, delete it afterwards with
`aws acm delete-certificate --certificate-arn <certificate-arn> --region
us-east-1`, and remove the DNS records.

## Cost

Hosting the page costs little, and nothing that grows with what visitors do in
DynamoDB: visitors' DynamoDB requests are billed to the AWS accounts whose
credentials they paste. Your bill covers:

- **S3 storage** for the bundle (about 2 MB per version) and its earlier
  versions, which expire after 90 days;
- **S3 requests**: CloudFront checks a file with S3 when its copy is more
  than a second old, so up to one small request per file for each visit,
  plus your uploads;
- **CloudFront** data transfer to visitors and requests: a first visit loads
  about 2 MB before compression; a later visit makes one request per file and
  downloads only the files that changed;
- **CloudFront invalidations** beyond the monthly free allowance (each upload
  uses one path, `/*`).

See the S3 and CloudFront pricing pages for current rates. Price class 100
keeps CloudFront to its lower-priced edge locations.

## Other hosts

Any host can serve the page if it meets these requirements:

1. **HTTPS.** The page accepts credentials only in a secure context.
2. **Its own origin**, as above.
3. **Exactly the packaged folder**, `ddbsh-web-<version>/`, and nothing else
   from the build folder.
4. **These content types:**

   | Files | Content-Type |
   |---|---|
   | `*.html` | `text/html; charset=utf-8` |
   | `*.js`, `*.mjs` | `text/javascript` |
   | `*.wasm` | `application/wasm` |
   | `*.css` | `text/css; charset=utf-8` |
   | `*.json` | `application/json` |
   | `*.txt` | `text/plain; charset=utf-8` |

5. **These response headers**, as in step 2: at least
   `Content-Security-Policy: frame-ancestors 'none'`; ideally all eight. Send
   only `frame-ancestors` in the header. The page's full Content Security
   Policy is in `index.html` and is too long for some hosts' header settings
   (CloudFront accepts at most 1783 characters for this header), and browsers
   apply the `<meta>` policy and the header policy together.
6. **`Cache-Control: no-cache` on every file**, or another setting that makes
   browsers check each file on every load. The file names carry no version,
   so a cached script can otherwise outlive the `index.html` it belongs to.

**GitHub Pages** serves HTTPS with the right content types, but it cannot send
custom response headers, so a page there gets none of the eight headers from
step 2. The most important of them, `frame-ancestors`, stops other sites from
framing the page; the page itself refuses credentials when it is framed or
opened by another page's script, which covers that risk. Three cautions apply:

- All the project sites of one GitHub user or organization share the origin
  `https://<owner>.github.io`, so pages from that owner's other repositories
  could read what visitors paste. Use a custom domain for the repository that
  hosts the page, or an owner with no other Pages sites.
- Without `frame-ancestors`, another site can still show the page in a frame
  (where it refuses credentials), for example to confuse visitors.
- GitHub Pages sends `Cache-Control: max-age=600` on every file, which you
  cannot change. For up to ten minutes after you publish, a returning
  visitor's browser can combine new and old files.
