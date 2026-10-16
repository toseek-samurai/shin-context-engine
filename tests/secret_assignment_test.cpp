#include "context_internal.h"
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::string opaque = "kV8mQ2xL9sN4" "pT6wY3dR7jC5";
    const std::string role = "support-operator-admin";
    const std::string install = "installation-release-integrity";
    const std::vector<std::string> safe = {
        "SAMPLE_SERVICE_AUTH_JWT_SECRET: local-development-auth-jwt-secret",
        "var token = IssueForRole(services, key, \"" + role + "\", \"" + install + "\");",
        "const token = await issuer.issue(\"" + install + "\");",
        "password = provider.current(\"" + role + "\");",
        "token = AcquireVersion2AccessTokenForAccount (\"" + install + "\");",
        "token = <supplied by issuer>",
        "token = (IssueForRole(services, \"" + install + "\"));",
        "var token = \"cursor1:\" + Convert.ToBase64String(JsonSerializer.SerializeToUtf8Bytes(cursor));",
        "token = EncodeCursorV2(cursor) + \"suffix\";",
        "token = \"pre\" + (codec.EncodeCursorV2(cursor)) + \"post\";",
        "export const credentialSecretProviderReadSecret = async (request: CredentialRequest, options?: RequestInit): Promise<CredentialResponse> => {",
        "const getSecret = (options?: RequestInit): Promise<Response42> => {",
    };
    const std::vector<std::string> blocked = {
        "token = \"local-development-" + opaque + "\";",
        "const getSecret = (token = \"" + opaque + "\"): Promise<Response42> => {",
        "const getSecret = (options?: RequestInit): Promise<Response42> => { const key = \"" + opaque + "\"; }",
        "token = \"" + opaque + "\";",
        "password: '" + opaque + "'",
        "api_key=" + opaque,
        "{\"access_token\":\"" + opaque + "\"}",
        "var token = @\"" + opaque + "\";",
        "var token = $\"" + opaque + "\";",
        "const token = `" + opaque + "`;",
        "token = \"" + install + "\";",
        "token = \"prefix " + opaque + "\";",
        "token = \"\"\"" + opaque + "\"\"\"",
        "token = @\"prefix \"\"" + opaque + "\"\"\";",
        "password = \"prefix\\\" " + opaque + "\";",
        "token = \"" + opaque,
        "token = (\"" + opaque + "\");",
        "token = ((string) \"" + opaque + "\");",
        "token = (System.String)\"" + opaque + "\";",
        "token = (string)@\"" + opaque + "\";",
        "token = (\"prefix\") + \"" + opaque + "\";",
        "token = (condition) ? \"" + opaque + "\" : fallback;",
        "token = GetPrefix() + \"" + opaque + "\";",
        "token = GetPrefix() + WithSuffix(\"" + opaque + "\");",
        "token = \"prefix\" + Convert.ToBase64String(\"" + opaque + "\");",
        "token = \"Convert.ToBase64String\";",
        "token = \"prefix\" + Convert.ToBase64String(cursor",
        "token = GetPrefix(); password = \"" + opaque + "\";",
        "token = static_cast<const char*>(\"" + opaque + "\");",
        "token = \"prefix\" + \"" + opaque + "\";",
        "token = condition ? \"" + opaque + "\" : fallback;",
        "var value = IssueForRole(\"sk-" + opaque + "\");",
        "var token = IssueForRole(\"sk-" + opaque + "\");",
        "authorization = Bearer " + opaque,
        "authorization = bEaReR " + opaque,
        "token = IssueForRole(\"AKIA" + std::string(16, 'Z') + "\");",
        "token = IssueForRole(\"eyJ" + std::string(20, 'a') + "." + std::string(20, 'b') + "." + std::string(20, 'c') + "\");",
    };
    for (const auto& line : safe) {
        if (shin::context::detect_secret_like_text(line, false).found) {
            std::cerr << "Unexpected assignment finding for case " << &line - safe.data() << '\n';
            return 1;
        }
    }
    for (const auto& line : blocked) {
        if (!shin::context::detect_secret_like_text(line, false).found) {
            std::cerr << "Missing secret finding for case " << &line - blocked.data() << '\n';
            return 1;
        }
    }
    const auto suppressed = "token = \"" + opaque + "\"; // GiTlEaKs:AlLoW";
    if (shin::context::detect_secret_like_text(suppressed, true).found ||
        !shin::context::detect_secret_like_text(suppressed, false).found) return 1;
    std::cout << safe.size() + blocked.size() << " secret assignment cases passed\n";
}
