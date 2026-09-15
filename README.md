# waapi

Minimal C89/gnu89 WhatsApp Cloud API service for `waapi.chaos.cc`, designed to run under Apache CGI during migration and directly under gmhttpd as the final target, with no PHP runtime.

## Architecture

Version 1.22 is a single executable, `waapi`.

The only public application route required is:

- `/webhook` - Meta WhatsApp webhook, GET verification and POST events.

The `/clock` and `/word` commands do not require public image URLs. Their PNG images are generated entirely in memory, uploaded directly to the WhatsApp Cloud API media endpoint, and sent using the returned media ID.

`/pic` intentionally keeps its existing external image-link behavior.

## Statistics

Usage statistics are stored locally in SQLite. Only aggregate daily counters are recorded: tool name, successful/failed uses, and last-use timestamp. Message contents, phone numbers, authentication codes, and command arguments are not stored.

`/stats` is family-only and reports today/total/error counts for WhatsApp commands and WhatsApp authentication events.

## Build

Dependencies: libcurl, libcjson, OpenSSL, libpng, FreeType, SQLite3.

```
make
```

Compilation uses `-O3 -march=native -Wall -Wextra -Werror -std=gnu89`.

## Runtime

By default `waapi` reads `/home/tools/mcp/work/waapi/config`. `WAAPI_CONFIG` can override the path for controlled tests.

Secrets are not compiled into the binary. The WhatsApp bearer token remains in the separate `token_file` configured at runtime.

If gmhttpd invokes the program as root, `waapi` drops privileges to `www-data` before processing requests.

Configuration supports repeated `family=` and `alias=name|command` entries. See `waapi.conf.example`.

## Preserved behavior

The C implementation preserves the active WhatsApp behavior: Meta webhook verification and events, text parsing, text/image replies, family numbers, aliases, numeric authentication codes, `/pic`, `/clock`, `/word`, `/rer`, `/radio`, `/we`, `/fo`, `/po`, `/so`, `/cc`, `/peso`, `/display`, and `/help`.

Five-digit WhatsApp authentication messages are forwarded only to the shared `auth` service using `action=incoming` and `method=whatsapp`. `waapi` does not create or read local OTP files; challenge ownership, validation and expiry are entirely managed by `auth`.

Configure this adapter with:

```text
auth_url=https://www.mazzini.org/auth
auth_key=<WhatsApp transport gateway key>
```

The WhatsApp transport key must be independent from the SMS gateway key and is never compiled into the binary.

`/clock` generates a 400x400 PNG in memory. `/word xx` generates a 600x300 PNG text-mask image in memory. Both are uploaded directly to Meta as `image/png` and sent by media ID.


## Security

- GET verification checks `hub.mode`, `hub.verify_token`, and `hub.challenge`.
- POST signature verification supports Meta `X-Hub-Signature-256` when `app_secret` is configured.
- Request bodies are limited to 256 KiB.
- JSON structure and message type are validated before use.
- Only text messages enter the command parser.
- External HTTP calls have connect and total timeouts and require a 2xx response.
- Dynamic PNG images are kept in memory and are not exposed through public URLs.
- `/display` uses the HTTPS status endpoint without shell execution.
- When launched as root, the process drops to `www-data` before handling the request.

## gmhttpd integration

The final gmhttpd virtual host only needs to execute `waapi` for `/webhook`. No `/clock` or `/word` HTTP routes are required.

The gmhttpd CGI environment must propagate the incoming `X-Hub-Signature-256` header as `HTTP_X_HUB_SIGNATURE_256` so Meta POST signature verification remains active.

No gmhttpd source or configuration is modified by this project automatically.

## Current deployment

Meta uses `https://waapi.chaos.cc/webhook` directly. Apache currently exposes the `webhook` executable; the service is ready for the later gmhttpd cutover without changing the WhatsApp application route.
