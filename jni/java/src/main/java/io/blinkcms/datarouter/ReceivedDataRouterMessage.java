package io.blinkcms.datarouter;

import java.util.Objects;

/** A message together with the switch-authenticated source service. */
public record ReceivedDataRouterMessage(String source, DataRouterMessage message) {
    public ReceivedDataRouterMessage {
        source = Objects.requireNonNull(source, "source");
        message = Objects.requireNonNull(message, "message");
    }
}
