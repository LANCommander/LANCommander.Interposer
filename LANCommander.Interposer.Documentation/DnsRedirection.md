---
sidebar_label: DNS Redirection
sidebar_position: 5
---

# DNS Redirection

DNS redirection intercepts hostname lookups before they reach the operating system's resolver, substituting one hostname (or IP literal) for another. The game opens a socket to a different server than the one it was hard-coded to use, with no patching or `hosts` file editing required.

## Why Use It

Common use cases:

- **Reviving dead online services**: point a game's hard-coded master server hostname at a community-run replacement
- **LAN play**: force every lookup of a remote game service at a local replacement on your LAN
- **Pointing at private master servers**: redirect a public service hostname to a server you host yourself
- **Testing**: temporarily redirect a production endpoint to a staging host without touching DNS or `hosts`

Because the substitution happens inside the game process, it does not require administrator privileges, does not require editing system files, and does not affect any other application running on the machine.

## Configuring Redirects

DNS redirects are defined as a list in `.interposer/Config.yml` under the `DnsRedirects` key. Each entry has a `Pattern` (regex) and a `Replacement`:

```yaml
DnsRedirects:
  - Pattern: '(.+)\.gamespy\.com'
    Replacement: '$1.openspy.net'
```

This single rule transparently redirects every `*.gamespy.com` lookup the game performs to the corresponding `*.openspy.net` host. This is enough to make many GameSpy-era titles work against [OpenSpy](https://openspy.net/) without editing the game's binaries.

:::tip Use single-quoted strings for patterns
YAML single-quoted strings pass backslashes through literally — no extra escaping needed when writing regex patterns. Double-quoted strings interpret YAML escape sequences and should be avoided here.
:::

Rules are evaluated in order. **The first matching rule wins** — subsequent rules are not checked once a match is found.

## Pattern Syntax

Patterns are [ECMAScript regular expressions](https://en.cppreference.com/w/cpp/regex/ecmascript), matched case-insensitively against the hostname the game passed to the resolver.

Partial matches are allowed, so `'\.gamespy\.com'` will match `master.gamespy.com`, `gpcm.gamespy.com`, `peerchat.gamespy.com`, and so on. If you need exact-host matching, anchor the pattern with `^` and `$`:

```yaml
DnsRedirects:
  - Pattern: '^master\.gamespy\.com$'
    Replacement: 'master.openspy.net'
```

IP literals (e.g. `192.0.2.10`) are passed through the same matching pipeline as hostnames. If you want to skip them, anchor your pattern so it cannot match a numeric address.

Note that this happens at the API level with no binary patching required, which also opens up the opportunity to redirect to hostnames of any length.

### Capture Groups

Use parentheses to capture parts of the matched hostname for use in the replacement:

```yaml
DnsRedirects:
  - Pattern: '(.+)\.gamespy\.com'
    Replacement: '$1.openspy.net'
```

For the host `peerchat.gamespy.com`:
- `$1` captures `peerchat`
- The replacement expands to `peerchat.openspy.net`

Up to nine capture groups (`$1` through `$9`) are supported.

## Replacement Syntax

| Token | Meaning |
|---|---|
| `$1` – `$9` | Replaced with the corresponding capture group from the pattern match. |

The replacement may be a hostname (which is then resolved normally by the OS) or an IPv4 / IPv6 literal (which is returned directly without going through DNS).

## Examples

### Use OpenSpy in place of GameSpy without patching the game

```yaml
DnsRedirects:
  - Pattern: '(.+)\.gamespy\.com'
    Replacement: '$1.openspy.net'
```

This is the headline use case. Many late-1990s and early-2000s multiplayer titles hard-code one or more `*.gamespy.com` hostnames into the executable. The OpenSpy revival project hosts drop-in replacements at the matching `*.openspy.net` names, so a single regex rule is enough to bring the game's online features back to life with no executable patching and no `hosts` file editing.

### Pin an exact host to a LAN IP address

```yaml
DnsRedirects:
  - Pattern: '^master\.example\.com$'
    Replacement: '10.0.0.5'
```

The replacement is an IPv4 literal, so the resolver returns it directly without performing DNS at all.

### Redirect a whole subdomain to a community-revival domain

```yaml
DnsRedirects:
  - Pattern: '^(.+)\.deadgame\.net$'
    Replacement: '$1.community-revival.org'
```

### Multiple rules — first match wins

```yaml
DnsRedirects:
  - Pattern: '^master\.gamespy\.com$'
    Replacement: '10.0.0.5'
  - Pattern: '(.+)\.gamespy\.com'
    Replacement: '$1.openspy.net'
```

The first rule pins the master server to a LAN address; the second catches every other `*.gamespy.com` lookup and routes it through OpenSpy.

## Verifying Redirects

Enable network logging in `.interposer/Config.yml`:

```yaml
Logging:
  Network: true
```

When a DNS redirect rule matches, the session log will contain a `[DNS REDIRECT]` line showing both the original and substituted hostnames:

```
2025-03-14 12:00:01  [DNS REDIRECT]   master.gamespy.com  ->  master.openspy.net
```

If a redirect is not firing, look for the corresponding `[CONNECT]` entries to see what hostnames the game is actually resolving and compare them against your pattern.

## Hooked Functions

DNS redirection applies to the following Winsock resolver functions in both `ws2_32.dll` and (where applicable) `wsock32.dll`:

| Function | Notes |
|---|---|
| `getaddrinfo` | Modern ANSI resolver. Used by most contemporary games. |
| `GetAddrInfoW` | Wide-character variant. |
| `GetAddrInfoExW` / `GetAddrInfoExA` | Async-capable resolver used by newer Winsock clients. |
| `gethostbyname` (`ws2_32`) | Legacy Winsock 2 entry point. |
| `gethostbyname` (`wsock32`) | Legacy Winsock 1 entry point used by older titles that link `wsock32.dll` directly. |

The substitution happens before the call reaches the operating system, so the OS resolver, the `hosts` file, and any DNS server on the network see only the replacement hostname.
