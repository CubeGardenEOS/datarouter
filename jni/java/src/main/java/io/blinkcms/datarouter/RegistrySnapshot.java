package io.blinkcms.datarouter;

import java.util.List;

/** A point-in-time registry view. */
public record RegistrySnapshot(List<RegistryEntry> entries) {
    public RegistrySnapshot {
        entries = List.copyOf(entries);
    }
}
