// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AWS Signature Version 4 request signing for S3-compatible object storage.
//
// The signing code is deliberately free of process, network and configuration
// dependencies so the canonical request, string to sign and signature can be
// checked against fixed vectors in the test suite without any live service.

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>

namespace S3 {

// S3 access credentials. The secret access key and session token are never
// written to the configuration file, the process argument list or the log.
struct Credentials
{
    QString accessKeyId;
    QString secretAccessKey;
    QString sessionToken;
};

// A request ready to be signed. `pathSegments` holds decoded path components so
// encoding of hostile names happens in exactly one place.
struct Request
{
    QByteArray method = QByteArrayLiteral("PUT");
    QString host;                                   // host[:port] as sent
    QList<QString> pathSegments;                    // decoded, e.g. {bucket,prefix,file}
    QList<QPair<QString, QString>> query;           // decoded name/value pairs
    QList<QPair<QByteArray, QByteArray>> headers;   // decoded extra headers
    QByteArray payloadHash;                         // lowercase hex sha256 of body
};

struct Signed
{
    QByteArray authorization;
    QByteArray canonicalRequest;
    QByteArray stringToSign;
    QByteArray signature;
    QByteArray amzDate;
    QByteArray scope;
    // Headers to send in addition to Authorization, in wire order:
    // x-amz-date, x-amz-content-sha256 and x-amz-security-token when present.
    QList<QPair<QByteArray, QByteArray>> headers;
};

// RFC 3986 percent encoding used by SigV4: only A-Z a-z 0-9 - _ . ~ stay
// literal. Slashes are kept when `keepSlash` is set (canonical URI only).
QString uriEncode(const QString& value, bool keepSlash = false);

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data);
QByteArray sha256Hex(const QByteArray& data);

// Encoded canonical URI. An empty segment list produces "/", and S3 forbids
// path normalisation, so segments are joined verbatim (encoded, not collapsed).
QString canonicalUri(const QList<QString>& segments);

Signed sign(const Request& request,
            const Credentials& credentials,
            const QString& region,
            const QDateTime& utcNow,
            const QString& service = QStringLiteral("s3"));

}
