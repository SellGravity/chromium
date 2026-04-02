// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gra_token_util.h"

#include <cstdint>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "crypto/keypair.h"
#include "crypto/sign.h"

namespace gra {

// static
bool GraTokenUtil::ValidateToken(const std::string& token_b64) {
  return ValidateTokenInternal(token_b64, /*check_expiry=*/true);
}

// static
bool GraTokenUtil::ValidateTokenIgnoreExpiry(const std::string& token_b64) {
  return ValidateTokenInternal(token_b64, /*check_expiry=*/false);
}

// static
bool GraTokenUtil::ValidateTokenInternal(const std::string& token_b64,
                                          bool check_expiry) {
  // Step 1: Base64 decode
  auto decoded = base::Base64Decode(token_b64);
  if (!decoded.has_value()) {
    LOG(ERROR) << "[GraToken] Base64 decode failed";
    return false;
  }

  // Step 2: Extract signature (first 64 bytes) and payload (rest)
  if (decoded->size() <= kSignatureSize) {
    LOG(ERROR) << "[GraToken] Token too short: " << decoded->size();
    return false;
  }

  auto full_span = base::span(*decoded);
  auto signature = full_span.first(kSignatureSize);
  auto payload_bytes = full_span.subspan(kSignatureSize);

  // Step 3: Import public key
  auto public_key = crypto::keypair::PublicKey::FromEd25519PublicKey(
      base::span<const uint8_t, 32>(kPublicKey));

  // Step 4: Verify Ed25519 signature
  if (!crypto::sign::Verify(crypto::sign::ED25519, public_key, payload_bytes,
                             signature)) {
    LOG(ERROR) << "[GraToken] Signature verification failed";
    return false;
  }

  // Step 5: Parse JSON payload
  std::string payload_str(reinterpret_cast<const char*>(payload_bytes.data()),
                           payload_bytes.size());
  auto parsed = base::JSONReader::Read(payload_str, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[GraToken] JSON parse failed";
    return false;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Step 6: Check required fields
  const std::string* profile_id = dict.FindString("profile_id");
  if (!profile_id || profile_id->empty()) {
    LOG(ERROR) << "[GraToken] Missing profile_id";
    return false;
  }

  // Step 7: Check expiry (if enabled)
  if (check_expiry) {
    auto expiry = dict.FindDouble("expiry");
    if (!expiry.has_value()) {
      LOG(ERROR) << "[GraToken] Missing expiry field";
      return false;
    }

    double now = base::Time::Now().InSecondsFSinceUnixEpoch();
    if (now > expiry.value()) {
      LOG(ERROR) << "[GraToken] Token expired";
      return false;
    }
  }

  LOG(INFO) << "[GraToken] Valid token for profile: " << *profile_id;
  return true;
}

// static
std::string GraTokenUtil::ExtractSessionId(const std::string& token_b64) {
  auto decoded = base::Base64Decode(token_b64);
  if (!decoded.has_value() || decoded->size() <= kSignatureSize) {
    return "";
  }

  auto full_span = base::span(*decoded);
  auto payload_bytes = full_span.subspan(kSignatureSize);

  std::string payload_str(reinterpret_cast<const char*>(payload_bytes.data()),
                           payload_bytes.size());
  auto parsed = base::JSONReader::Read(payload_str, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return "";
  }

  const std::string* session_id = parsed->GetDict().FindString("session_id");
  return session_id ? *session_id : "";
}

}  // namespace gra
