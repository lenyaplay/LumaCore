import Foundation
import Security

/// Generates a random UUID on first run and persists it to the Keychain
/// (survives app reinstall, tied to device+signing team) — `identifierForVendor`
/// isn't usable for this because it resets on reinstall. See
/// ARCHITECTURE.md §6: fingerprint = SHA256(keychainUUID + bundle_id).
enum KeychainFingerprint {
  private static let service = "com.lumacore.device-fingerprint"
  private static let account = "device-uuid"

  static func loadOrCreateUUID() -> String {
    if let existing = load() { return existing }
    let newUUID = UUID().uuidString
    save(newUUID)
    return newUUID
  }

  private static func load() -> String? {
    let query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: account,
      kSecReturnData as String: true,
      kSecMatchLimit as String: kSecMatchLimitOne,
    ]
    var result: AnyObject?
    guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess,
          let data = result as? Data
    else { return nil }
    return String(data: data, encoding: .utf8)
  }

  private static func save(_ value: String) {
    let query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: account,
    ]
    SecItemDelete(query as CFDictionary)
    var attributes = query
    attributes[kSecValueData as String] = Data(value.utf8)
    attributes[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
    SecItemAdd(attributes as CFDictionary, nil)
  }
}
