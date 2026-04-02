// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GRA_TOKEN_UTIL_H_
#define CHROME_BROWSER_GRA_TOKEN_UTIL_H_

#include <cstdint>
#include <string>

namespace gra {

// Ed25519 token verification for GraBrowser.
//
// Token format: Base64( signature[64 bytes] + json_payload )
//   - signature: Ed25519 signature of json_payload
//   - json_payload: {"profile_id":"...","timestamp":...,"expiry":...}
//
// Only the PUBLIC KEY is embedded in the binary.
// Private key stays on App Manager — cannot forge tokens from binary.
class GraTokenUtil {
 public:
  // Validate a --gra-token value. Returns true if:
  // 1. Decodes from Base64 successfully
  // 2. Ed25519 signature is valid (signed by App Manager's private key)
  // 3. Token has not expired
  static bool ValidateToken(const std::string& token_b64);

  // Validate without expiry check (for debugging)
  static bool ValidateTokenIgnoreExpiry(const std::string& token_b64);

  // Extract session_id from a validated token.
  // Must only be called AFTER ValidateToken returns true.
  // Returns empty string if session_id is missing.
  static std::string ExtractSessionId(const std::string& token_b64);

 private:
  // Ed25519 public key (32 bytes) — corresponds to App Manager's private key.
  // This key can VERIFY signatures but CANNOT CREATE them.
  static constexpr uint8_t kPublicKey[32] = {
      0x37, 0x5b, 0x0e, 0x9a, 0xb5, 0x41, 0x06, 0x44,
      0x85, 0x62, 0x77, 0xe0, 0x71, 0x1d, 0x91, 0x52,
      0xca, 0xac, 0x7e, 0x72, 0x90, 0xdf, 0x5f, 0x2f,
      0xd3, 0x83, 0x3c, 0xc6, 0x38, 0xfa, 0x28, 0xca};

  static constexpr size_t kSignatureSize = 64;

  static bool ValidateTokenInternal(const std::string& token_b64,
                                     bool check_expiry);
};

}  // namespace gra

#endif  // CHROME_BROWSER_GRA_TOKEN_UTIL_H_
