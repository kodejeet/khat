#pragma once

#include <QString>

// Supplies a short-lived Gmail access token for an account.
//
// Step i deliberately ships only the empty provider: OAuth credentials and
// secure token storage are not implemented yet. Keeping the lookup behind this
// interface means the scheduler never receives a placeholder token or polls an
// account that has not been authorized.
class AccessTokenProvider
{
public:
    virtual ~AccessTokenProvider() = default;

    // Returns false (and clears *token) when the account has no usable token.
    // Implementations must never write tokens to logs or configuration files.
    virtual bool accessToken(const QString &accountId, QString *token) const = 0;
};

class EmptyAccessTokenProvider final : public AccessTokenProvider
{
public:
    bool accessToken(const QString &accountId, QString *token) const override
    {
        Q_UNUSED(accountId)
        if (token) {
            token->clear();
        }
        return false;
    }
};
