// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ============================================================================
// GRA BROWSER - PORTABLE MODE
// ============================================================================
// This file has been modified to use a hardcoded AES-256-GCM key instead of
// Windows DPAPI. This allows profile data (cookies, passwords, etc.) to be
// portable across different Windows machines.
//
// WARNING: This is for INTERNAL TESTING ONLY. The hardcoded key means anyone
// with access to this source code can decrypt the data.
// ============================================================================

#include "components/os_crypt/sync/os_crypt.h"

#include <windows.h>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/memory/singleton.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "crypto/aead.h"
#include "crypto/random.h"

namespace {

// ============================================================================
// GRA PORTABLE KEY - AES-256 (32 bytes)
// ============================================================================
// This hardcoded key enables portable encryption across machines.
// Generated randomly - DO NOT share this key publicly in production!
constexpr uint8_t kGraSecretKey[32] = {
    0x47, 0x72, 0x61, 0x42, 0x72, 0x6F, 0x77, 0x73,  // "GraBrows"
    0x65, 0x72, 0x50, 0x6F, 0x72, 0x74, 0x61, 0x62,  // "erPortab"
    0x6C, 0x65, 0x4B, 0x65, 0x79, 0x32, 0x30, 0x32,  // "leKey202"
    0x35, 0x21, 0x40, 0x23, 0x24, 0x25, 0x5E, 0x26   // "5!@#$%^&"
};

// AEAD key length in bytes (256 bits).
constexpr size_t kKeyLength = 32;

// AEAD nonce length in bytes (96 bits for GCM).
constexpr size_t kNonceLength = 12;

// Version prefix for GRA portable encryption.
// Using "v11" to distinguish from Chrome's "v10" DPAPI-based encryption.
constexpr char kGraEncryptionPrefix[] = "v11";

// Legacy prefix for fallback (Chrome's DPAPI-based encryption).
constexpr char kLegacyEncryptionPrefix[] = "v10";

}  // namespace

namespace OSCrypt {
bool EncryptString16(const std::u16string& plaintext, std::string* ciphertext) {
  return OSCryptImpl::GetInstance()->EncryptString16(plaintext, ciphertext);
}
bool DecryptString16(const std::string& ciphertext, std::u16string* plaintext) {
  return OSCryptImpl::GetInstance()->DecryptString16(ciphertext, plaintext);
}
bool EncryptString(const std::string& plaintext, std::string* ciphertext) {
  return OSCryptImpl::GetInstance()->EncryptString(plaintext, ciphertext);
}
bool DecryptString(const std::string& ciphertext, std::string* plaintext) {
  return OSCryptImpl::GetInstance()->DecryptString(ciphertext, plaintext);
}
void RegisterLocalPrefs(PrefRegistrySimple* registry) {
  OSCryptImpl::RegisterLocalPrefs(registry);
}
InitResult InitWithExistingKey(PrefService* local_state) {
  // GRA Portable: Always return success - no key needed from prefs
  return OSCrypt::kSuccess;
}
bool Init(PrefService* local_state) {
  // GRA Portable: Always return success - using hardcoded key
  return true;
}
std::string GetRawEncryptionKey() {
  return OSCryptImpl::GetInstance()->GetRawEncryptionKey();
}
void SetRawEncryptionKey(const std::string& key) {
  OSCryptImpl::GetInstance()->SetRawEncryptionKey(key);
}
bool IsEncryptionAvailable() {
  // GRA Portable: Always available - hardcoded key
  return true;
}
void UseMockKeyForTesting(bool use_mock) {
  OSCryptImpl::GetInstance()->UseMockKeyForTesting(use_mock);
}
void SetLegacyEncryptionForTesting(bool legacy) {
  OSCryptImpl::GetInstance()->SetLegacyEncryptionForTesting(legacy);
}
void ResetStateForTesting() {
  OSCryptImpl::GetInstance()->ResetStateForTesting();
}
}  // namespace OSCrypt

OSCryptImpl::OSCryptImpl() = default;
OSCryptImpl::~OSCryptImpl() = default;

OSCryptImpl* OSCryptImpl::GetInstance() {
  return base::Singleton<OSCryptImpl,
                         base::LeakySingletonTraits<OSCryptImpl>>::get();
}

bool OSCryptImpl::EncryptString16(const std::u16string& plaintext,
                              std::string* ciphertext) {
  return EncryptString(base::UTF16ToUTF8(plaintext), ciphertext);
}

bool OSCryptImpl::DecryptString16(const std::string& ciphertext,
                              std::u16string* plaintext) {
  std::string utf8;
  if (!DecryptString(ciphertext, &utf8))
    return false;

  *plaintext = base::UTF8ToUTF16(utf8);
  return true;
}

// ============================================================================
// GRA PORTABLE ENCRYPTION - AES-256-GCM with Hardcoded Key
// ============================================================================
bool OSCryptImpl::EncryptString(const std::string& plaintext,
                            std::string* ciphertext) {
  // Initialize AEAD with hardcoded key
  crypto::Aead aead(crypto::Aead::AES_256_GCM);
  std::string key(reinterpret_cast<const char*>(kGraSecretKey), kKeyLength);
  aead.Init(&key);

  // Generate random nonce (12 bytes for GCM)
  std::string nonce(kNonceLength, '\0');
  crypto::RandBytes(base::as_writable_byte_span(nonce));

  // Encrypt with AEAD
  if (!aead.Seal(plaintext, nonce, std::string(), ciphertext)) {
    LOG(ERROR) << "[GRA] Encryption failed";
    return false;
  }

  // Prepend nonce and version prefix: "v11" + nonce + ciphertext
  ciphertext->insert(0, nonce);
  ciphertext->insert(0, kGraEncryptionPrefix);
  
  return true;
}

bool OSCryptImpl::DecryptString(const std::string& ciphertext,
                            std::string* plaintext) {
  // Check minimum length: prefix(3) + nonce(12) + tag(16) = 31 bytes minimum
  if (ciphertext.length() < 31) {
    LOG(ERROR) << "[GRA] Ciphertext too short";
    return false;
  }

  // Check for GRA portable prefix "v11"
  if (base::StartsWith(ciphertext, kGraEncryptionPrefix,
                       base::CompareCase::SENSITIVE)) {
    // GRA Portable decryption
    crypto::Aead aead(crypto::Aead::AES_256_GCM);
    std::string key(reinterpret_cast<const char*>(kGraSecretKey), kKeyLength);
    aead.Init(&key);

    // Extract nonce (after "v11" prefix)
    const std::string nonce =
        ciphertext.substr(sizeof(kGraEncryptionPrefix) - 1, kNonceLength);
    
    // Extract actual ciphertext (after prefix and nonce)
    const std::string raw_ciphertext =
        ciphertext.substr(kNonceLength + (sizeof(kGraEncryptionPrefix) - 1));

    if (!aead.Open(raw_ciphertext, nonce, std::string(), plaintext)) {
      LOG(ERROR) << "[GRA] Decryption failed - invalid key or corrupted data";
      return false;
    }
    
    return true;
  }

  // Check for legacy Chrome "v10" prefix (DPAPI-based, not supported in portable mode)
  if (base::StartsWith(ciphertext, kLegacyEncryptionPrefix,
                       base::CompareCase::SENSITIVE)) {
    LOG(WARNING) << "[GRA] Legacy v10 (DPAPI) encryption detected - not portable!";
    // Cannot decrypt DPAPI data on different machine
    return false;
  }

  // Unknown format or raw DPAPI data
  LOG(WARNING) << "[GRA] Unknown encryption format - cannot decrypt";
  return false;
}

// static
void OSCryptImpl::RegisterLocalPrefs(PrefRegistrySimple* registry) {
  // GRA Portable: No prefs needed, but register empty for compatibility
  registry->RegisterStringPref("os_crypt.encrypted_key", "");
  registry->RegisterBooleanPref("os_crypt.audit_enabled", true);
}

bool OSCryptImpl::Init(PrefService* local_state) {
  // GRA Portable: No initialization needed - using hardcoded key
  return true;
}

OSCrypt::InitResult OSCryptImpl::InitWithExistingKey(PrefService* local_state) {
  // GRA Portable: Always success - hardcoded key always available
  return OSCrypt::kSuccess;
}

void OSCryptImpl::SetRawEncryptionKey(const std::string& raw_key) {
  // GRA Portable: Ignored - using hardcoded key
  DLOG(INFO) << "[GRA] SetRawEncryptionKey ignored - using hardcoded key";
}

std::string OSCryptImpl::GetRawEncryptionKey() {
  // GRA Portable: Return hardcoded key
  return std::string(reinterpret_cast<const char*>(kGraSecretKey), kKeyLength);
}

bool OSCryptImpl::IsEncryptionAvailable() {
  // GRA Portable: Always available
  return true;
}

void OSCryptImpl::UseMockKeyForTesting(bool use_mock) {
  use_mock_key_ = use_mock;
}

void OSCryptImpl::SetLegacyEncryptionForTesting(bool legacy) {
  use_legacy_ = legacy;
}

void OSCryptImpl::ResetStateForTesting() {
  use_legacy_ = false;
  use_mock_key_ = false;
}
