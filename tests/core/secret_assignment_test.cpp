#include "context_internal.h"
#include <iostream>
#include <string>
#include <vector>

int main() {
    const std::string opaque = "kV8mQ2xL9sN4" "pT6wY3dR7jC5";
    const std::string role = "support-operator-admin";
    const std::string install = "installation-release-integrity";
    const std::string manifest =
        "\napiVersion: v1\nkind: Secret\nmetadata: {name: workflow-runtime-settings-secrets}\n"
        "stringData: {SERVICE_CONFIG: placeholder}\n";
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
        "Authorization: Bearer ${{ secrets.DEPLOYMENT_ACCESS_TOKEN }}",
        "Authorization: Bearer ${DEPLOYMENT_ACCESS_TOKEN}",
        "Authorization: Bearer <supplied-by-credential-provider>",
        "export const getToken = (token: string): Promise<AuthorizationHeadersVersion2> => {\n"
        "  return buildHeaders(token);\n}\n",
        "const token = makeToken();\nconst version = AuthorizationHeadersVersion2;\n",
        "const password = process.env.DEPLOYMENT_PASSWORD;",
        "const secret = readSecret(); // AuthorizationHeadersVersion2",
        "const description = 'This API returns a secret or token';",
        "fetch(`/api/continue?token=${token}`);\nconst mode = AuthorizationHeadersVersion2;\n",
        "expect(url).toBe('https://service.test/continue?api_token=hello');\n",
        "fetch('https://service.test/continue?token=undefined');\nconst mode = AuthorizationHeadersVersion2;\n",
        "\"SENTRY_AUTH_TOKEN\": \"${SENTRY_AUTH_TOKEN}\"",
        "append(\"&next_token=\")\nappend(URLEncoder.encode(nextToken, StandardCharsets.UTF_8))\n"
        "val response = get(url, headers)\nreturn workflowRuntimeSettings\n",
        "append('?next_token=');\nconst mode = AuthorizationHeadersVersion2;\n",
        "append(`&next_token=`)\nconst mode = AuthorizationHeadersVersion2;\n",
        "private const string Secret = \"\"\"" + manifest + "\"\"\";",
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
        "append(\"&next_token=" + opaque + "\");",
        "append(\"&next_token=\" + \"" + opaque + "\");",
        "const url = (\"?next_token=\") + \"" + opaque + "\";",
        "append(\"&next_token=\"\n + \"" + opaque + "\");",
        "append(\"&next_token=\");\nconst password = \"" + opaque + "\";",
        "private const string Secret = \"\"\"" + manifest + "password: " + opaque + "\n\"\"\";",
        "private const string Secret = \"\"\"\napiVersion: v1\nkind: Secret\n"
        "metadata: {name: workflow-runtime-settings-secrets}\nstringData: {SERVICE_CONFIG: " + opaque + "}\n\"\"\";",
        "private const string Secret = \"\"\"\napiVersion: v1\nkind: Secret\n"
        "metadata: {name: workflow-runtime-settings-secrets}\nstringData: {SERVICE_CONFIG: " + install + "}\n\"\"\";",
        "token = \"workflow-runtime-settings-secrets\";",
        "private const string Secret = \"\"\"\napiVersion: v1\nkind: Secret\n"
        "metadata: {name: sk-" + opaque + "}\nstringData: {SERVICE_CONFIG: placeholder}\n\"\"\";",
        "private const string Secret = \"\"\"\nmetadata: {name: workflow-runtime-settings-secrets}\n\"\"\";",
        "token = condition ? \"" + opaque + "\" : fallback;",
        "Authorization: Bearer ${{ secrets.DEPLOYMENT_ACCESS_TOKEN }}\nAuthorization: Bearer " + opaque,
        "const harmless = 1;\nconst api_key = \"" + opaque + "\";",
        "const getSecret = (options?: RequestInit): Promise<Response42> => {\n"
        "  const password = \"" + opaque + "\";\n}\n",
        "password =\n  \"" + opaque + "\";",
        "token = GetPrefix() +\n WithSuffix(\"" + opaque + "\");",
        "authorization = Bearer\t" + opaque,
        "const password = process.env.DEPLOYMENT_PASSWORD; const api_key = \"" + opaque + "\";",
        "const url = 'https://service.test/continue?token=" + opaque + "';",
        "export ACCESS_KEY=ASIA" + std::string(16, 'Z'),
        std::string("-----") + "BEGIN RSA PRIVATE KEY-----\nopaque\n-----END RSA PRIVATE KEY-----",
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
    // Neither long safe prefixes nor non-UTF-8 source bytes waive later findings.
    const auto large = std::string(1024 * 1024, 'x') + "\n";
    if (shin::context::detect_secret_like_text(large, false).found ||
        !shin::context::detect_secret_like_text(large + "password = \"" + opaque + "\";", false).found ||
        !shin::context::detect_secret_like_text(std::string(1, static_cast<char>(0xff)) +
            "\nauthorization = Bearer " + opaque, false).found) return 1;
    std::cout << safe.size() + blocked.size() << " secret assignment cases passed\n";
}
