---
sidebar_label: Network Adapters
sidebar_position: 10
---

# Network Adapters

Many games naively pick the first network adapter Windows reports. That breaks LAN play as soon as a VPN, Hamachi, or any other virtual adapter shadows the real LAN NIC — the game binds to, advertises, or browses on the wrong interface, and nobody on the LAN can see it.

The `NetworkAdapters` section presents the game a filtered view of the machine's adapters so that it lands on the one you want, without disabling or reordering anything on the host.

```yaml
NetworkAdapters:
  Enabled: true
  Subnets: ['192.168.1.0/24', '10.0.0.0/8']   # IPv4 CIDR
  Names:   ['^Ethernet', 'Realtek']           # regex vs FriendlyName/Description
  MACs:    ['00:11:22:33:44:55']              # colon or dash separated
```

## How It Works

This is an **allow-list**, the same model as [DirectInput device filtering](/Interposer/DirectInput#filtering-devices): an adapter is **kept if it matches any** configured filter, and every non-matching adapter is hidden from the enumeration APIs.

If all three lists are empty the feature is a no-op and every adapter is shown unchanged, regardless of `Enabled`. Nothing is hooked at all unless something asks for it, so leaving the section at its defaults costs the game nothing.

| Option | Meaning |
|---|---|
| `Enabled` | Apply the filters below. `false` by default. |
| `Subnets` | IPv4 CIDR ranges. An adapter is kept if any of its IPv4 addresses falls inside one of them. |
| `Names` | Case-insensitive ECMAScript regex matched against each adapter's `FriendlyName` **and** `Description` |
| `MACs` | Physical addresses, colon- or dash-separated, case-insensitive |

Partial matches count for `Names`, so anchor with `^` and `$` for exact matching. Use single-quoted YAML strings for patterns so backslashes stay literal.

An entry that cannot be parsed — a malformed CIDR, an invalid regex, a MAC that is not six bytes — is skipped, and the remaining entries in that list still apply. If every entry in every list is skipped the section behaves as though it were empty.

## Coverage

The three filters do not all have the same reach, because not every enumeration API exposes every attribute. Only `Subnets` can be applied to an API that reports addresses and nothing else:

| Filter | Matched against | Covered APIs |
|---|---|---|
| `Subnets` | Each of the adapter's IPv4 addresses | `GetAdaptersInfo`, `GetAdaptersAddresses`, `gethostbyname` (local host), `WSAIoctl SIO_GET_INTERFACE_LIST` |
| `Names` | `FriendlyName` and `Description` | `GetAdaptersInfo`, `GetAdaptersAddresses` |
| `MACs` | Physical address | `GetAdaptersInfo`, `GetAdaptersAddresses` |

By API:

- **iphlpapi** — `GetAdaptersInfo`, `GetAdaptersAddresses` (`Subnets` + `Names` + `MACs`)
- **ws2_32 / wsock32** — `gethostbyname` for the local host (`Subnets` only)
- **ws2_32** — `WSAIoctl SIO_GET_INTERFACE_LIST` (`Subnets` only)

Filtering is IPv4-only. An adapter with no IPv4 address can still be kept by a `Names` or `MACs` rule on the iphlpapi APIs.

:::tip Prefer a subnet rule
`Subnets` is the only filter that reaches every covered API, so a game that enumerates through Winsock rather than iphlpapi will only respond to a subnet rule. Start there and add `Names` or `MACs` when you need to disambiguate two adapters on the same subnet.
:::

## Logging

Set `Logging.Network: true` to record enumeration and hidden-adapter activity in the session log:

```
2026-08-14 20:11:47  [ADAPTER ENUM]    GetAdaptersAddresses
2026-08-14 20:11:47  [ADAPTER HIDE]    Hamachi
2026-08-14 20:11:47  [ADAPTER HIDE]    VirtualBox Host-Only Network
```

Every adapter the game asked for that does not appear in an `[ADAPTER HIDE]` line was shown to it. If a game still picks the wrong interface after filtering, the `[ADAPTER ENUM]` verb tells you which API it used — a game enumerating through `WSAIoctl SIO_GET_INTERFACE_LIST` will not respond to a `Names` or `MACs` rule.

## Reading the Adapter List

Plugins and host applications can enumerate the canonical adapter list, with each adapter's allowed/hidden status, through the exported `InterposerEnumNetworkAdapters`. In .NET this is surfaced as `InterposerService.EnumerateNetworkAdapters()` returning `NetworkAdapterInfo` records:

| Property | Meaning |
|---|---|
| `FriendlyName` | The adapter's friendly name, e.g. `Ethernet` |
| `Description` | The adapter's description, usually the NIC model |
| `MacAddress` | Physical address as `00:11:22:33:44:55`, empty if none |
| `IPv4Address` | First IPv4 address, empty if none |
| `IPv6Address` | First IPv6 address, empty if none |
| `Allowed` | `true` if the adapter passes the configured filter, `false` if it is hidden from the game |

This reports the real adapter set regardless of the filter, which makes it suitable for building a UI that lets a user pick which adapter a game should use.
