package io.blinkcms.datarouter;

/** Immutable diagnostic registry entry; it is not an authority grant. */
public record RegistryEntry(String serviceName, int pid, long routeId, long channelId, long slot, long lastSeenMonotonicNanos) {}
