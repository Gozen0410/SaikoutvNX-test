#pragma once
#include <string>
namespace crypto {
std::string md5(const std::string& data);
std::string sha1(const std::string& data);
std::string sha256(const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);
std::string aesCbcDecrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7 = true);
std::string aesCbcEncrypt(const std::string& data, const std::string& key, const std::string& iv, bool pkcs7 = true);
std::string aesEcbDecrypt(const std::string& data, const std::string& key, bool pkcs7 = true);
std::string aesCtr(const std::string& data, const std::string& key, const std::string& iv);
std::string rc4(const std::string& key, const std::string& data);
std::string cryptoJsDecrypt(const std::string& base64Cipher, const std::string& passphrase);
std::string evpBytesToKey(const std::string& password, const std::string& salt, int keyLen = 32, int ivLen = 16);
std::string base64Encode(const std::string& data, bool urlSafe = false, bool padding = true);
std::string base64Decode(const std::string& data, bool urlSafe = false);
std::string toHex(const std::string& data);
std::string fromHex(const std::string& hex);
}
