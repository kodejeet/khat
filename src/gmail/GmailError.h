#pragma once

#include <QString>

enum class GmailErrorKind
{
    None,
    NoToken,
    Network,
    Unauthorized,
    Forbidden,
    RateLimited,
    QuotaExceeded,
    Server,
    InvalidRequest,
    NotFound,
    InvalidResponse,
    Internal,
};

struct GmailError
{
    GmailErrorKind kind = GmailErrorKind::None;
    int httpStatus = 0;
    int retryAfterSeconds = 0;
    QString message;

    bool isRetryable() const
    {
        switch (kind) {
        case GmailErrorKind::Network:
        case GmailErrorKind::RateLimited:
        case GmailErrorKind::QuotaExceeded:
        case GmailErrorKind::Server:
            return true;
        case GmailErrorKind::None:
        case GmailErrorKind::NoToken:
        case GmailErrorKind::Unauthorized:
        case GmailErrorKind::Forbidden:
        case GmailErrorKind::InvalidRequest:
        case GmailErrorKind::NotFound:
        case GmailErrorKind::InvalidResponse:
        case GmailErrorKind::Internal:
            return false;
        }
        return false;
    }
};
