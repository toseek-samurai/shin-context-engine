# Capability Architecture

The fixture runtime resolves widget capabilities through a bounded host adapter.

## Decision

Capability requests must pass through the host policy before execution.

## Result Contract

The capability result envelope contains `value`, `commands`, and `diagnostics`.
The executor never performs connector I/O directly.

## Detailed Flow

The scene submits a capability request.
The host validates the capability identifier.
The assistant uses the default capability route for ordinary requests.
The adapter resolves the configured connector.
The connector returns a typed value.
The host applies policy to emitted commands.
The executor resumes the suspended invocation.
The scene receives the bounded result envelope.
