# Changelog

## 0.13.0 (2026-10-22)

- Strict privacy: the message fingerprint now also replaces email addresses with `<email>` and quoted
  values (`"..."`, and `'...'` when the quotes aren't part of a word) with `<str>`, before the existing
  steps. `KeyError: 'Jane Doe'` and `KeyError: 'Bob Smith'` now group together, and no guessable value sits
  behind the hash. Errors whose messages contain quotes or emails get new fingerprints once, so they open
  new issues after upgrading.

## 0.12.1 (2026-10-22)

- The README says what strict privacy does in its opening lines.

## 0.12.0 (2026-10-20)

- **Behavior change under strict privacy: a raw request path keeps only its first segment.** A span name or performance transaction name holding a raw path is now sent as its first segment plus `/:path` (`/patients/jane-doe/12` is `/patients/:path`; a first segment with a digit, or over 40 characters, is `:id`), where before only segments with a digit were replaced, so a name slug like `jane-doe` went out. A route template (any segment like `:id`, `{id}`, `<int:id>`, `[slug]` or `*path`) is kept exactly as it is. New `strict_privacy::strict_path`; `strict_privacy::path_without_ids` is kept as another name for it. With strict privacy off, nothing changes.

## 0.11.0 (2026-10-20)

- Strict privacy, for an app that can't let personal or health data leave its own servers: new `Configuration::strict_privacy` (default `false`) and `Configuration::privacy_allowed_fields` (default empty), which start as `FORGE_OPS_STRICT_PRIVACY=true` and `FORGE_OPS_PRIVACY_ALLOWED_FIELDS` (comma separated) when those are set; a value set in code wins. With it on, an error is sent with an empty `message` and a new `message_fingerprint` (a one-way hash of the message with its ids taken out, computed in this process), `context` and `tags` cut to the allowed keys with plain values, the user cut to its `id` as a string, and no breadcrumbs (none are recorded either), SQL statement or source context. Span names are read by hand, never with a URL parser: an `http` span is named by its method and host only, a span named by a path or by a method and a path has its ids replaced by `:id`, and every span's data keeps only `db.statement` and `db.system`. A performance sample's transaction name is reduced the same way (a route is kept, a raw path loses its ids). ForgeOps applies the same rules on arrival when the project's own Strict privacy setting is on. New `strict_privacy.hpp` holds the rules (`normalize_message`, `message_fingerprint`, `path_without_ids`, `span_name`, `transaction_name`, `event`, `span_data`). With it off, nothing changes.

## 0.10.0 (2026-10-13)

- A batch the server will never accept is no longer sent again and again. A response of 4xx (other than 408 and 429) now means a batch of performance samples, metrics or infrastructure readings is dropped. A network error, a timeout, a 5xx, 408 or 429 still keeps the batch for the next flush, but not for good: it is dropped after five failed flushes in a row. Each drop is passed once to `Configuration::logger`, if you set one, saying what was dropped and why. Before, any failure kept the batch, so a rejected one was sent at every flush for as long as the process lived. Errors, traces and changes are unchanged: each was already sent once and never retried.
- New `Client::performance_samples_status`, `metrics_status` and `infrastructure_metrics_status` return the HTTP status (0 for no response), `delivery_outcome_for_status` sorts one into `DeliveryOutcome::delivered`, `rejected` or `failed`, and a second `MetricBuffer` constructor takes a function that returns the status. Every existing function is unchanged: `Client::deliver` and the rest still return `true` only for a 2xx, and a `MetricBuffer` built with a `bool` function treats `false` as a failure that may pass.

## 0.9.1 (2026-10-07)

- The library now ships its MIT licence (`LICENSE.txt`). Earlier versions were published without one. No code changes.

## 0.9.0 (2026-10-02)

- Fixed a crash at the end of an ordinary program: capturing a handled exception and then returning 0 from `main` could segfault (exit 139) and lose the event. At static destruction the error queue's global joined its delivery thread, which was still sending and parsing the DSN with a function-local `std::regex` that had already been destroyed. `init()` (and the first capture, for a program that never calls it) now registers an `atexit` hook that delivers what is queued and joins every background thread (errors, traces, changes, performance, metrics) before static destruction starts, and the regexes and lists those threads read are now never destroyed. New `shutdown()` on `DeliveryQueue`, `SpanQueue`, `MetricBuffer` and `PerformanceFlusher` is that hook's per-worker step.
- **Behavior change:** `Configuration::environment` now starts as `FORGE_OPS_ENVIRONMENT` when that is set and not blank. Unset, it is still `"production"`. If that variable is set to something other than `production` or `staging` where this SDK runs, reporting from there stops until you change it or add it to `enabled_environments`. New `Configuration::default_environment()` returns that starting value.
- With a DSN set and an environment that isn't enabled, `init()` now logs one line, once per process, through `config.logger` (stderr when no logger is set): `[ForgeOps] Not sending: this environment is "development", and only production, staging are enabled. Set FORGE_OPS_ENVIRONMENT=production (or add "development" to the enabled environments) to send from here.` Nothing is logged without a DSN.

## 0.8.0 (2026-10-01)

- An uncaught exception is now delivered before the process ends. The terminate handler queued the report and then called `std::abort()`, which runs no destructors, so the background thread died with the report still queued and the crash was never sent. The handler now delivers it, along with anything else still queued, on the terminating thread before chaining to the previous handler, waiting at most 2 seconds so an unreachable ForgeOps can't hold up the crash. This works when the exception escapes on any thread, the delivery thread included.
- New `flush_errors(timeout)` (default 2 seconds) delivers every captured error still queued, on the calling thread, and returns `true` once everything went out in time or `false` when the timeout passed first. Call it before a way out that skips destructors (`std::quick_exit`, `std::_Exit`); a normal exit already delivers what's queued. It never throws. Built on the new `DeliveryQueue::drain(timeout)`, and `Client::deliver(payload, max_time)` caps one delivery at `max_time` when that's sooner than `Configuration::timeout_seconds`.
- A logger that throws when the queue is full no longer leaves the queue's lock held.

## 0.7.0 (2026-09-29)

- SQL masking now catches values it used to let through, matching ForgeOps's own masker again: a string with a backslash-escaped quote (`'o\'brien'`, `E'o\'brien'`) is masked whole instead of leaving the rest of it visible, a string's type prefix goes with it (`E''`, `X''`, `N''`, `B''` and `U&''` each become one `?`), and hex (`0x1F`), binary (`0b101`), exponent (`3e10`, `1.5E-3`) and leading-dot (`.5`) numbers are masked. On a `database` span whose `db.system` is `mysql` or `mariadb`, "double quoted" text is a string and is masked too; on any other database it's a name and is still left alone. `sql_statement::mask(statement, system)` takes that system as a new optional second argument (default `std::nullopt`).

## 0.6.0 (2026-09-25)

- A `database` span can now carry the SQL it ran, such as an embedded SQLite query: new `ScopedSpan::set_statement(statement, db_system)`, `database_span(name, statement, db_system, f)` and `record_database_span(name, started_at, duration_ms, statement, db_system, data)`. The statement is masked before it leaves the process (every string and number literal becomes `?`), cut at 4000 characters, and sent in the span's data as `db.statement`, with `db.system` lowercased. A `db.statement` put in the `data` of a `database` span directly is masked the same way. `set_statement` is ignored on spans of any other kind.

## 0.5.0 (2026-09-25)

- Change tracking: `record_change(kind, title, details, options)` records one change you made (a feature flag flipped, a config value changed, a firmware setting pushed) so ForgeOps can show it next to the errors and slowdowns that followed. `details` is an optional JSON object; `ChangeOptions` holds `environment` (default `Configuration::environment`), `service`, `actor`, `url`, `id` (an idempotency key) and `occurred_at` (default now). `kind` is one of `feature_flag`, `config`, `migration`, `dependency`, `infrastructure` or `other` (`change_kinds`); anything else is sent as `other`. The title is cut to 200 characters, never through a multibyte UTF-8 character, and a blank one records nothing. Delivered on a background thread through the same bounded queue traces use (`SpanQueue` now takes the endpoint to deliver to), drained when the process exits normally, so the call never blocks on the network and never throws; a full queue or a failed delivery, including a 403 on a plan without change tracking, drops the change quietly. A no-op when reporting isn't enabled. This client sends no startup snapshot of its own.

## 0.4.0

- Distributed tracing across services, using the W3C Trace Context standard (`traceparent`). `ScopedHttpSpan(method, url, data)` (or `http_span(method, url, f)`, which passes `f` the value) records an outgoing HTTP call as an `http` span named after the method and host, and hands back the `traceparent` header value to send with that request from `traceparent()` (a `std::optional<std::string>`); its parent id is the span's own id, so the called service's root span nests under it. `ScopedTrace(root_name, traceparent)` (or `trace(root_name, traceparent, f)`) continues an incoming request's trace (same trace id, root span parented under the caller's span); `std::nullopt` or a malformed value starts a fresh trace. New `Configuration::propagate_traces` (default true) and `Configuration::trace_propagation_targets` (default `std::nullopt`, meaning every host; or a list of `TracePropagationTarget`, each a host string matching that host and its subdomains or a `std::regex` searched for in the host) control where the header goes. `trace_parent::header` is the header's name.
- Errors captured inside a trace now carry a top-level `trace_id`, so ForgeOps can link them to errors from other services that handled the same request: `capture_exception` and `capture_exception_with_sql`, and the terminate handler. `current_trace_id()` returns it. Errors captured outside a trace are unchanged. `EventBuilder::build` and `Reporter::report` take it as a new optional last argument.
- With `track_tracing = false`, a `ScopedTrace` still starts a trace with an id (attached to errors and propagated in the header, since that is also what links errors across services); only span reporting stops. Previously nothing started with tracing off.
- Trace and span ids were already W3C shaped (32 and 16 lowercase hex characters); they are now also guaranteed never to be all zeros, the one value the standard reserves as invalid.
- `ScopedSpan::span_id()` returns the span's id (empty outside a trace).

This SDK has earlier tagged releases on its mirror (v0.3.0 is the latest), but no `CHANGELOG.md` existed for it before this entry; it starts here rather than backfilling every earlier version. There is no version constant in the source or `CMakeLists.txt`: a release is its git tag, so this one is `v0.4.0`.
