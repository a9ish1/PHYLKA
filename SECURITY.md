# Security policy

PHYLKA is experimental, unaudited software. There is no bug bounty and no
guarantee of any reward, but responsible reports are welcome and will be
credited if you wish.

## Reporting a vulnerability

Please **do not** open a public issue or pull request for security problems.

Use GitHub's private vulnerability reporting: go to the repository's
**Security** tab and choose **Report a vulnerability**. (Maintainers: enable
"Private vulnerability reporting" in the repository settings before
publishing.)

Include, where possible:

- affected version or commit and platform,
- a description of the impact (consensus failure, fund loss, remote crash,
  key/seed disclosure, denial of service, ...),
- steps or a proof of concept to reproduce,
- whether the issue is already public.

## What to expect

- Acknowledgement within about 7 days; this is a volunteer project, so
  timelines are best effort.
- We will confirm the issue, work on a fix, and coordinate disclosure with you.
  Please allow a reasonable period (we suggest 90 days) before publishing
  details.
- Consensus-affecting fixes may require a coordinated network upgrade.

## Scope

In scope: code in `src/`, build files, and the protocol described in
`docs/PROTOCOL.md`. Issues in vendored libraries (`deps/`) should also be
reported upstream (libsecp256k1, PQClean).

Known and out of scope: the project has no external audit, the RPC port is
unauthenticated by design (loopback only), and the dev network accepts test
secrets from environment variables.

## Handling your own funds

Never share your seed phrase or wallet password with anyone, including
maintainers. We will never ask for them.
