#pragma once
#include "shin/server/model.h"
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <openssl/core_names.h>
#include <openssl/pem.h>

namespace oidc_fixture {
inline std::string encode(const std::string& value) {
    return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(value));
}
struct Key {
    std::string private_pem;
    shin::server::Json jwk;
    explicit Key(const std::string& kid, int bits = 2048) {
        const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) <= 0)
            throw std::runtime_error("Fixture key setup failed");
        EVP_PKEY* raw = nullptr;
        if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) throw std::runtime_error("Fixture keygen failed");
        const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
        const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        if (!bio || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
            throw std::runtime_error("Fixture PEM failed");
        char* bytes = nullptr;
        const auto size = BIO_get_mem_data(bio.get(), &bytes);
        private_pem.assign(bytes, static_cast<std::size_t>(size));
        const auto component = [&](const char* name) {
            BIGNUM* number = nullptr;
            if (EVP_PKEY_get_bn_param(key.get(), name, &number) != 1) throw std::runtime_error("Fixture JWK failed");
            const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned(number, BN_free);
            std::string value(static_cast<std::size_t>(BN_num_bytes(number)), '\0');
            BN_bn2bin(number, reinterpret_cast<unsigned char*>(value.data()));
            return encode(value);
        };
        jwk = {{"kid", kid}, {"kty", "RSA"}, {"use", "sig"}, {"alg", "RS256"},
            {"n", component(OSSL_PKEY_PARAM_RSA_N)}, {"e", component(OSSL_PKEY_PARAM_RSA_E)}};
    }
    std::string sign(const std::string& header, const std::string& claims) const {
        const auto content = encode(header) + "." + encode(claims);
        std::error_code error;
        const auto signature = jwt::algorithm::rs256("", private_pem).sign(content, error);
        if (error) throw std::runtime_error("Fixture signing failed");
        return content + "." + encode(signature);
    }
};
} // namespace oidc_fixture
