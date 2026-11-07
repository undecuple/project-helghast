# Experimental networking: evidence and limits

The candidate for title-specific work is **BCUS98234, update 1.14, application ID 23044**. The build identifier is
the project's target; 23044 is the application ID named in [`rt_capture.c`](../port/net/rt_capture.c). This does
not establish a 1.14 connection or compatibility with any server.

## What was observed

The game-free host test runs the existing port configuration, NP ticket and RT code together with the patched
runtime's online switch and policy. It generates its own unsigned tickets and frames; host resolution, sockets and
sends are replaced by test doubles. It exercises no game, real ticket, certificate, capture or server. Run
`python3 tools/net/test_network.py` after preparing the patched runtime. The test is also in
[`tools/host-tests.txt`](../tools/host-tests.txt).

The previous [status](STATUS.md) reported reaching the MULTIPLAYER menu with a local test server. That report was
not reproduced here and did not identify the title version or establish genuine authentication, an independent
compatible server or a joined match. Earlier comments in [`rt_capture.c`](../port/net/rt_capture.c) describe
client messages and layouts from project investigations, including some explicitly labelled v1.00; those are
prior reports, not newly verified observations of the 1.14 candidate.

## Observations versus the diagnostic model

The loopback capture is a test model with canned or inferred responses, not a server compatibility claim. No
network exchange with any of the services below was observed in this work.

| Component | Prior project reports | Model and synthetic evidence here | Still unverified |
|---|---|---|---|
| **MUIS / RT** | Source comments describe the client hello, connect and universe-information messages. | Generated frames exercise header lengths, encrypted and nested frames, a supplied synthetic hello response and redacted diagnostics. | A real cryptographic handshake, certificate validation and interoperability. |
| **SVO** | Source comments describe region-list and login HTTP requests. | The loopback model can return example XML; this host test does not exercise HTTP. | Real SVO semantics, authentication and service acceptance. |
| **MAS** | Source comments describe session and ticket-login messages. | A generated session request exercises the modeled session-begin response; unsigned ticket shape and callback are tested separately. | An independent account service accepting a ticket or signing in a player. |
| **MLS** | Source comments describe lobby, channel and party messages. | The diagnostic has modeled replies; no MLS exchange is exercised here. | Independent lobby behavior, multiple clients and server compatibility. |
| **DME** | Source comments describe party and game-connection messages. | A generated matchmaking request exercises a modeled match-accepted reply; it is not a DME session. | Transport, synchronization, joining a match and multiplayer gameplay. |

The NP tests prove the shape, copy and callback behavior of the implementation's **synthetic unsigned** v2.1
ticket. They do not prove PSN authentication, entitlement or acceptance by any service.

## Opt-in and failure boundaries

- `KZ3_ONLINE` is read once by the existing runtime. Unset, empty or `0` is offline; any other nonempty value
  opts in. Offline setup does not open the configuration, resolve a host, start a socket or register NP handlers.
- Online setup reads `online.ini`. With no selector it uses the top-level host, or the first server section if
  there is no top-level host. An explicit top-level `server = NAME` must be a nonempty, unquoted, unique selector
  naming an existing `[server NAME]` section. A malformed, duplicated, misplaced or unknown selector fails closed
  before any host lookup or ticket load, including when `KZ3_NET_SERVER` names another host.
- Unknown keys and sections containing `server` anywhere, ignoring case, are conservatively treated as
  selector-shaped and fail closed. Malformed or duplicate server sections, an overlong or NUL-bearing line,
  an oversized or unreadable configuration, and an invalid or explicitly empty `KZ3_NET_SERVER` override also fail
  closed. Other unknown keys remain warnings. `KZ3_NET_SERVER` is a test override, absent by default; when valid it
  overrides a valid selection. On Windows, a native presence check catches an empty variable the CRT may treat as absent.
- On valid opt-in, resolving the configured host uses the operating system resolver and can itself cause a DNS
  request outside the guest socket policy. The existing policy then maps the MUIS name and allows the selected
  IPv4 address and configured ports; the test checks this policy directly with a fake resolver, not the host
  network. A missing host or failed lookup leaves no allowed destination and does not load a ticket.
- The **generic runtime resolver-policy gap remains a separate blocker**. These tests do not establish that every
  runtime resolver or HLE path respects the intended policy, and this change adds no resolver, errno or HLE patch.
  Do not infer a complete DNS, socket or PS3 error-semantics implementation from the configuration tests.
- `KZ3_NET_CAPTURE` is unset by default. When online and the configured address is loopback, setting it to a
  nonzero value can start diagnostic listeners on `127.0.0.1` ports 10070, 10071, 10075, 10078 and 10079.
  RT logs retain type, length and selected state metadata, but not payload prefixes, hello/certificate bytes,
  tokens, ticket bytes or their hashes. Other logs can still contain paths, configured identities or HTTP
  metadata; review and redact them before sharing.

Remaining gaps include the candidate 1.14 title run, the generic resolver-policy issue, genuine authentication and
handshake behavior, an independently operated compatible server, NAT behavior and a complete multiplayer session.
