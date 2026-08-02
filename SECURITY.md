# Security

## Reporting

Email the maintainer (see GitHub profile for HaoZeke) or open a private
security advisory on the GitHub repository. Do not file public issues for
unfixed wire-parser crashes or traversal-limit bypasses.

## Scope

This library parses untrusted Cap'n Proto messages. Treat every network or
IPC buffer as hostile. Traversal and depth limits are enabled by default;
do not raise them without a concrete need.

Embedders that expose Janet packs must keep the Janet environment sealed
(no open/spawn/net) independently of this library.
