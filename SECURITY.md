# Security

## Reporting a vulnerability

Email <contact@mowogames.com>, or use GitHub's
[private advisory form](https://github.com/MowoGames/Wolang/security/advisories/new).
Please do not open a public issue.

Include what you found, what it lets someone do, and the smallest program
that shows it. You will get an acknowledgement within a few days and an
answer about what happens next.

## What counts

Wolang is a compiler, a runtime and a set of libraries. What is in scope:

- **The compiler**: a crash, a hang or memory corruption while compiling
  a program. A program is untrusted input to a compiler.
- **The runtime**: anything that lets a program written in Wolang break
  the guarantees the language makes — reading outside a buffer without
  `unsafe`, a use-after-free in a destructor, a data race the type system
  said could not happen.
- **The libraries**: `http` parsing a request, `std.json`, `std.yaml`,
  `std.toml` and `std.compress` reading a document someone else wrote,
  `lib/db` reading a store, `lib/tls` verifying a certificate. All of
  these read bytes from somewhere else, which is where the bugs that
  matter live.
- **The embedding API**: a script escaping what the host allowed it.

What is not:

- A program that uses `unsafe` and gets what it asked for. `unsafe` is
  how a program takes responsibility; the compiler stops checking and
  says so.
- A missing feature, unless the gap is itself exploitable.
- Denial of service from a program that was *asked* to use the machine:
  `ParallelFor` on every core is not a vulnerability.

## What the language promises

Outside `unsafe`, a Wolang program should not be able to read or write
memory it does not own, call a function with the wrong signature, or
observe a value that was never written. Anything that breaks one of those
is a security bug even if the program was trying to do it.

Inside `unsafe`, the program is responsible, and the manual says so.

## Versions

Until 1.0, the newest commit on `alpha` is the supported version. Fixes
go there.
