# Pinned client protocol reference

These unmodified Apache-2.0 protocol sources come from LiveKit protocol v1.53.0.
`manifest.json` records the resolved commit and SHA-256 of every vendored file.
They are used for descriptor contract verification, not linked into the SDK.

The runtime currently builds the compatible subset in `../livekit.proto` using
Protobuf 21.12 lite. `tests/protocol_contract.py` compiles both descriptors and
compares every exposed field's number, label, type, presence and oneof, as well
as enum values. Legacy int32/enum substitutions are explicitly listed in that
test; new fields should preserve the upstream types. An upstream definition
being present here does not mean its behavior is implemented by the SDK.

Protocol upgrades must update the pinned sources and manifest together, pass
the descriptor checks, and separately verify the runtime behavior before
raising the SDK's advertised protocol version.
