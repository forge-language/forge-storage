# Forge Storage

Forge Storage publishes immutable files and downloads them by SHA-256. It contains a reusable Forge module (`storage.fg`), a native streaming I/O bridge, and a standalone HTTP service. Forge Platform uses it for package source archives and SDK artifacts.

This is an S3-like object distribution service, **not** an implementation of the S3 API. It uses content-addressed keys, one storage root, public reads, and a writer bearer token. It does not implement buckets, AWS signatures, private objects, multipart uploads, listing, deletion, lifecycle policies, or replication.

## Run the service

Build the image from this repository, then create an environment file outside source control containing a random writer token:

```sh
docker build -f Containerfile -t forge-storage:local .
mkdir -p .deployment
chmod 700 .deployment
printf 'STORAGE_TOKEN=%s\n' "$(openssl rand -hex 32)" > .deployment/storage.env
chmod 600 .deployment/storage.env
docker compose --env-file .deployment/storage.env up -d
curl -fsS http://127.0.0.1:18104/health
```

The service listens on port 8090 inside its container. Compose binds port 18104 to loopback and persists objects in a dedicated volume. Connect a reverse proxy or Cloudflare Tunnel to the loopback port for HTTPS. Keep the writer token on the server; browser clients must never receive it. Start-up rejects tokens shorter than 32 characters.

`STORAGE_ROOT` defaults to `/data`. Each object is a regular file named by its SHA-256. Uploads stream into temporary files on the same filesystem, are hashed, synced, and committed with an atomic hard link. Existing objects cannot be overwritten. The maximum object size is 256 MiB; a deployment proxy may impose a lower limit. Use the persistent volume for data, and back it up separately from this repository. The filesystem, local administrator, and reverse proxy are trusted.

## HTTP API

| Method | Path | Behavior |
| --- | --- | --- |
| GET | `/health` | Service health and implementation language |
| PUT | `/v1/objects/<sha256>` | Publish a raw binary body; requires `Authorization: Bearer <writer-token>` |
| GET | `/v1/objects/<sha256>` | Download a public object |
| HEAD | `/v1/objects/<sha256>` | Object headers without a body |
| GET | `/v1/objects/<sha256>/meta` | JSON containing `sha256` and `size` |

Keys contain exactly 64 lowercase hexadecimal characters. A first upload returns 201; an identical duplicate returns 200. A body whose checksum differs from its key returns 422, without publishing it. Unsupported methods return 405. Missing objects return 404, and invalid keys return 400. Missing or incorrect writer credentials return 401. Oversized uploads return 413.

Downloads use `application/octet-stream`, `Content-Disposition: attachment`, `X-Content-Type-Options: nosniff`, and `Cache-Control: public,max-age=31536000,immutable`. The quoted SHA-256 is the ETag. `If-None-Match` supports the exact ETag. Single byte ranges support explicit, open-ended, and suffix forms; valid ranges return 206, unsatisfiable or malformed ranges return 416. `If-Range` accepts the exact ETag; another value returns the full object. Multi-range and date-based validators are not supported.

```sh
hash=$(sha256sum artifact.tar.gz | cut -d ' ' -f 1)
curl --fail-with-body -X PUT \
  -H "Authorization: Bearer $STORAGE_TOKEN" \
  --data-binary @artifact.tar.gz \
  "http://127.0.0.1:18104/v1/objects/$hash"
curl --fail --output downloaded.tar.gz \
  "http://127.0.0.1:18104/v1/objects/$hash"
printf '%s  downloaded.tar.gz\n' "$hash" | sha256sum -c -
```

## Forge library

Import `storage` and link `forge_storage` plus `forge_web`. See [examples/client.fg](examples/client.fg) for upload, metadata, and verified download.

| Function | Result |
| --- | --- |
| `storage.valid_digest(hash)` | 1 for a valid key, otherwise 0 |
| `storage.digest(path)` | SHA-256 string, or an empty string on failure |
| `storage.object_url(base, hash)` | Object URL |
| `storage.put(base, token, path)` | Parsed metadata handle on success, otherwise 0 |
| `storage.get(base, hash, path)` | 1 after an atomic verified download, otherwise 0 |
| `storage.info(base, hash)` | `web.request` response handle with `status` and `data` fields |
| `storage.archive(url, base, token)` | Downloads a GitHub codeload archive and uploads it; metadata handle or 0 |

`get` downloads to a sibling temporary file, verifies SHA-256, and renames it into place. Failure leaves the destination unchanged. Client transfers have a 5-second connection timeout and a 180-second total timeout. Archive ingestion accepts only URLs beginning with `https://codeload.github.com/` and follows no redirects. Call library functions inside a Forge Web resource scope when integrating them into a long-running server; release that scope after processing each request.

Forge owns digest validation, object URL construction, authorization, routing, and library policy. Native C implements HTTP protocol handling, bounded streaming, filesystem primitives, SHA-256, constant-time secret comparison, and resource cleanup.

## Native build

Install a Forge SDK and the C build prerequisites: C compiler, CMake, pkg-config, and development packages for libmicrohttpd, json-c, libcurl, and OpenSSL. The default SDK root is `~/.forge/current`.

```sh
scripts/build.sh
# Explicit SDK/build locations are supported:
FORGE_ROOT=/path/to/sdk FORGE_BIN=/path/to/sdk/build/bin/forge \
FORGE_LIB_DIR=/path/to/sdk/build/lib scripts/build.sh
```

The build produces `build/forge-storage` and `build/library/libforge_storage.a`. The container build provides an isolated compiler and dependencies; no neighboring project checkout is required.

## Verify

Run against an **isolated** service with a separate storage volume. Tests publish immutable random test objects; they must not target production.

```sh
STORAGE_TOKEN="$TEST_STORAGE_TOKEN" python3 tests/test_service.py \
  --base http://127.0.0.1:18105
```

Pass `--root /path/to/test/data` when the test runner can inspect the service filesystem to verify temporary-file cleanup. Checks cover binary streaming, chunked uploads, authentication, checksum failures, immutable concurrent duplicates, metadata, HEAD, ranges, conditional reads, and oversized request headers. A service restart must retain the same object bytes when using its persistent volume.
