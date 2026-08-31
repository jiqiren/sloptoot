#pragma once

#include <stddef.h>
#include <stdint.h>

[[nodiscard]] char *base64url_encode(const uint8_t *data, size_t len);

/* Standard base64 (RFC 4648, `+/` alphabet with `=` padding), as required by
 * the kitty graphics protocol payloads. Fresh allocation; caller frees. */
[[nodiscard]] char *base64_encode(const uint8_t *data, size_t len);

[[nodiscard]] char *random_verifier(size_t bytes);
