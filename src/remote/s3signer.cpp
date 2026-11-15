// SPDX-License-Identifier: GPL-3.0-or-later
#include "s3signer.h"

#include <QCryptographicHash>
#include <QMap>

#include <algorithm>

namespace S3 {
namespace {

constexpr char kAlgorithm[] = "AWS4-HMAC-SHA256";

QByteArray toHex(const QByteArray& bytes)
{
    return bytes.toHex();
}

QByteArray trimHeaderValue(const QByteArray& value)
{
    QByteArray result = value;
    while (!result.isEmpty() &&
           (result.back() == ' ' || result.back() == '\t')) {
        result.chop(1);
    }
    while (!result.isEmpty() &&
           (result.at(0) == ' ' || result.at(0) == '\t')) {
        result.remove(0, 1);
    }
    return result;
}

QByteArray hmacSha256Bytes(const QByteArray& key, const QByteArray& data)
{
    constexpr int blockSize = 64;
    QByteArray block = key;
    if (block.size() > blockSize) {
        block = QCryptographicHash::hash(block, QCryptographicHash::Sha256);
    }
    // Pad to the block size explicitly: QByteArray::resize() leaves grown bytes
    // uninitialised, which would feed garbage into the key schedule.
    block += QByteArray(blockSize - block.size(), '\0');
    Q_ASSERT(block.size() == blockSize);
    QByteArray innerPad(blockSize, '\0');
    QByteArray outerPad(blockSize, '\0');
    for (int index = 0; index < blockSize; ++index) {
        const auto byte = static_cast<unsigned char>(block.at(index));
        innerPad[index] = static_cast<char>(byte ^ 0x36);
        outerPad[index] = static_cast<char>(byte ^ 0x5c);
    }
    QCryptographicHash inner(QCryptographicHash::Sha256);
    inner.addData(innerPad);
    inner.addData(data);
    QCryptographicHash outer(QCryptographicHash::Sha256);
    outer.addData(outerPad);
    outer.addData(inner.result());
    return outer.result();
}

} // namespace

QString uriEncode(const QString& value, bool keepSlash)
{
    static const char* digits = "0123456789ABCDEF";
    // Encode the UTF-8 bytes of the whole string. Encoding per QChar would emit
    // a %00 pad byte for every ASCII character and split surrogate pairs.
    const QByteArray utf8 = value.toUtf8();
    QString encoded;
    encoded.reserve(utf8.size());
    for (const char raw : utf8) {
        const auto byte = static_cast<unsigned char>(raw);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') ||
                                (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' ||
                                byte == '_' || byte == '.' || byte == '~';
        if (unreserved) {
            encoded.append(QLatin1Char(raw));
            continue;
        }
        if (keepSlash && byte == '/') {
            encoded.append(QLatin1Char('/'));
            continue;
        }
        encoded.append(QLatin1Char('%'));
        encoded.append(QLatin1Char(digits[byte >> 4]));
        encoded.append(QLatin1Char(digits[byte & 0x0F]));
    }
    return encoded;
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
    return hmacSha256Bytes(key, data);
}

QByteArray sha256Hex(const QByteArray& data)
{
    return toHex(QCryptographicHash::hash(data, QCryptographicHash::Sha256));
}

QString canonicalUri(const QList<QString>& segments)
{
    if (segments.isEmpty()) {
        return QStringLiteral("/");
    }
    QStringList encoded;
    encoded.reserve(segments.size());
    for (const QString& segment : segments) {
        encoded.append(uriEncode(segment));
    }
    return QLatin1Char('/') + encoded.join(QLatin1Char('/'));
}

Signed sign(const Request& request,
            const Credentials& credentials,
            const QString& region,
            const QDateTime& utcNow,
            const QString& service)
{
    const QString date = utcNow.toUTC().toString(QStringLiteral("yyyyMMdd"));
    const QString amzDate = utcNow.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const QString scope = QStringLiteral("%1/%2/%3/aws4_request")
                            .arg(date, region, service);

    // Canonical headers: lowercase names, trimmed values, sorted by name.
    QMap<QByteArray, QByteArray> headers;
    headers.insert(QByteArrayLiteral("host"), request.host.toUtf8());
    headers.insert(QByteArrayLiteral("x-amz-content-sha256"), request.payloadHash);
    headers.insert(QByteArrayLiteral("x-amz-date"), amzDate.toUtf8());
    if (!credentials.sessionToken.isEmpty()) {
        headers.insert(QByteArrayLiteral("x-amz-security-token"),
                       credentials.sessionToken.toUtf8());
    }
    for (const auto& header : request.headers) {
        headers.insert(header.first.toLower(), trimHeaderValue(header.second));
    }

    QByteArray canonicalHeaders;
    QByteArray signedHeaders;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (!signedHeaders.isEmpty()) {
            signedHeaders.append(';');
        }
        signedHeaders.append(it.key());
        canonicalHeaders.append(it.key());
        canonicalHeaders.append(':');
        canonicalHeaders.append(it.value());
        canonicalHeaders.append('\n');
    }

    QList<QPair<QString, QString>> query;
    query.reserve(request.query.size());
    for (const auto& pair : request.query) {
        query.append({ uriEncode(pair.first), uriEncode(pair.second) });
    }
    std::sort(query.begin(), query.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    });
    QStringList canonicalQuery;
    canonicalQuery.reserve(query.size());
    for (const auto& pair : query) {
        canonicalQuery.append(pair.first + QLatin1Char('=') + pair.second);
    }

    QByteArray canonicalRequest;
    canonicalRequest.append(request.method.toUpper());
    canonicalRequest.append('\n');
    canonicalRequest.append(canonicalUri(request.pathSegments).toUtf8());
    canonicalRequest.append('\n');
    canonicalRequest.append(canonicalQuery.join(QLatin1Char('&')).toUtf8());
    canonicalRequest.append('\n');
    canonicalRequest.append(canonicalHeaders);
    canonicalRequest.append('\n');
    canonicalRequest.append(signedHeaders);
    canonicalRequest.append('\n');
    canonicalRequest.append(request.payloadHash);

    QByteArray stringToSign;
    stringToSign.append(kAlgorithm);
    stringToSign.append('\n');
    stringToSign.append(amzDate.toUtf8());
    stringToSign.append('\n');
    stringToSign.append(scope.toUtf8());
    stringToSign.append('\n');
    stringToSign.append(sha256Hex(canonicalRequest));

    QByteArray signingKey = hmacSha256Bytes(
      QByteArrayLiteral("AWS4") + credentials.secretAccessKey.toUtf8(), date.toUtf8());
    signingKey = hmacSha256Bytes(signingKey, region.toUtf8());
    signingKey = hmacSha256Bytes(signingKey, service.toUtf8());
    signingKey = hmacSha256Bytes(signingKey, QByteArrayLiteral("aws4_request"));
    const QByteArray signature = toHex(hmacSha256Bytes(signingKey, stringToSign));

    Signed result;
    result.amzDate = amzDate.toUtf8();
    result.scope = scope.toUtf8();
    result.canonicalRequest = canonicalRequest;
    result.stringToSign = stringToSign;
    result.signature = signature;
    result.authorization = QByteArray(kAlgorithm) + QByteArrayLiteral(" Credential=") +
                           credentials.accessKeyId.toUtf8() + QByteArrayLiteral("/") +
                           scope.toUtf8() + QByteArrayLiteral(", SignedHeaders=") +
                           signedHeaders + QByteArrayLiteral(", Signature=") + signature;
    result.headers.append({ QByteArrayLiteral("x-amz-date"), amzDate.toUtf8() });
    result.headers.append(
      { QByteArrayLiteral("x-amz-content-sha256"), request.payloadHash });
    if (!credentials.sessionToken.isEmpty()) {
        result.headers.append({ QByteArrayLiteral("x-amz-security-token"),
                                credentials.sessionToken.toUtf8() });
    }
    return result;
}

}
