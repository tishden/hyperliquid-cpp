// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

// Shared credential loading for the example programs: environment variables or a key file.
// Keys are never taken from command-line arguments, which are visible to other users via `ps`.

#include <cstdlib>
#include <fstream>
#include <string>

namespace example {

struct Credentials {
    std::string privateKey;     ///< HL_PRIVATE_KEY — API (agent) wallet key
    std::string accountAddress; ///< HL_ACCOUNT_ADDRESS — master account the agent trades for
    std::string vaultAddress;   ///< HL_VAULT_ADDRESS — optional vault / sub-account
};

/// Read HL_PRIVATE_KEY / HL_ACCOUNT_ADDRESS / HL_VAULT_ADDRESS from the environment.
inline Credentials credentialsFromEnv() {
    const auto get = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr ? std::string{value} : std::string{};
    };
    return Credentials{get("HL_PRIVATE_KEY"), get("HL_ACCOUNT_ADDRESS"), get("HL_VAULT_ADDRESS")};
}

/**
 * @brief Overlay `NAME=value` lines from @p path onto @p credentials (`#` starts a comment).
 * @return false if the file cannot be read.
 */
inline bool loadCredentialsFile(const std::string& path, Credentials& credentials) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
            value.pop_back();
        }
        if (key == "HL_PRIVATE_KEY") {
            credentials.privateKey = value;
        } else if (key == "HL_ACCOUNT_ADDRESS") {
            credentials.accountAddress = value;
        } else if (key == "HL_VAULT_ADDRESS") {
            credentials.vaultAddress = value;
        }
    }
    return true;
}

}  // namespace example
