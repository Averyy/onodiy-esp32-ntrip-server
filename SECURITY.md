# Security

## Reporting a vulnerability

Report vulnerabilities privately through GitHub's private vulnerability reporting
on this repository (the **Security** tab, **Report a vulnerability**). Please don't
open a public issue for a security problem.

Say what is affected, how to reproduce it, and the firmware version if you know it.

## Scope

- The station firmware (`main/`, `components/`).
- The web UI and its API (`components/web/`).
- The update trust model: a station fetches releases only from this repository's
  GitHub Releases, over HTTPS with the server certificate checked, and uses a file
  only if its size and SHA-256 match the release manifest (see "Releases and
  updates" in [docs/releases.md](docs/releases.md)).

The receiver firmware packages in `firmware/um982/` are Unicore's; report problems
in them to Unicore.

There is no bug bounty.
