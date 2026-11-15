// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Configuration keys for remote storage. They live in the General group and
// must all be registered in ConfigHandler::recognizedGeneralOptions(), which is
// where the defaults and value validation live.
//
// The legacy `recordingTransfer*` keys are still registered: migration copies
// them once into the shared keys below and leaves the originals in place so an
// older build, or a hand-edited configuration file, keeps working.

#define REMOTE_STORAGE_ENABLED "remoteStorageEnabled"
#define REMOTE_STORAGE_PROTOCOL "remoteStorageProtocol"
#define REMOTE_STORAGE_HOST "remoteStorageHost"
#define REMOTE_STORAGE_PORT "remoteStoragePort"
#define REMOTE_STORAGE_USER "remoteStorageUser"
#define REMOTE_STORAGE_PRIVATE_KEY "remoteStoragePrivateKey"
#define REMOTE_STORAGE_KNOWN_HOSTS "remoteStorageKnownHosts"
#define REMOTE_STORAGE_REMOTE_DIR "remoteStorageRemoteDir"
#define REMOTE_STORAGE_SECRET_SOURCE "remoteStorageSecretSource"
#define REMOTE_STORAGE_ACCEPT_INSECURE_FTP "remoteStorageAcceptInsecureFtp"
#define REMOTE_STORAGE_MIGRATION_VERSION "remoteStorageMigrationVersion"

#define REMOTE_STORAGE_S3_ENDPOINT "remoteStorageS3Endpoint"
#define REMOTE_STORAGE_S3_REGION "remoteStorageS3Region"
#define REMOTE_STORAGE_S3_BUCKET "remoteStorageS3Bucket"
#define REMOTE_STORAGE_S3_KEY_PREFIX "remoteStorageS3KeyPrefix"
#define REMOTE_STORAGE_S3_ACCESS_KEY "remoteStorageS3AccessKey"

#define REMOTE_STORAGE_IMAGE_MODE "remoteStorageImageMode"
#define REMOTE_STORAGE_IMAGE_DIR "remoteStorageImageDir"
#define REMOTE_STORAGE_IMAGE_FORMAT "remoteStorageImageFormat"
#define REMOTE_STORAGE_IMAGE_QUALITY "remoteStorageImageQuality"
#define REMOTE_STORAGE_IMAGE_COMPRESSION "remoteStorageImageCompression"

#define REMOTE_STORAGE_VIDEO_MODE "remoteStorageVideoMode"
#define REMOTE_STORAGE_VIDEO_DIR "remoteStorageVideoDir"
#define REMOTE_STORAGE_VIDEO_CONTAINER "remoteStorageVideoContainer"
#define REMOTE_STORAGE_VIDEO_CODEC "remoteStorageVideoCodec"
#define REMOTE_STORAGE_VIDEO_RATE_CONTROL "remoteStorageVideoRateControl"
#define REMOTE_STORAGE_VIDEO_QUALITY "remoteStorageVideoQuality"
#define REMOTE_STORAGE_VIDEO_BITRATE "remoteStorageVideoBitrateKbps"
#define REMOTE_STORAGE_VIDEO_AUDIO_MODE "remoteStorageVideoAudioMode"
#define REMOTE_STORAGE_VIDEO_AUDIO_CODEC "remoteStorageVideoAudioCodec"
#define REMOTE_STORAGE_VIDEO_AUDIO_BITRATE "remoteStorageVideoAudioBitrateKbps"

// Legacy keys, read once by RemoteStorage::migrateLegacy().
#define LEGACY_TRANSFER_ENABLED "recordingTransferEnabled"
#define LEGACY_TRANSFER_PROTOCOL "recordingTransferProtocol"
#define LEGACY_TRANSFER_HOST "recordingTransferHost"
#define LEGACY_TRANSFER_PORT "recordingTransferPort"
#define LEGACY_TRANSFER_USER "recordingTransferUser"
#define LEGACY_TRANSFER_REMOTE_DIR "recordingTransferRemoteDir"
#define LEGACY_TRANSFER_PRIVATE_KEY "recordingTransferPrivateKey"
#define LEGACY_TRANSFER_KNOWN_HOSTS "recordingTransferKnownHosts"

// Environment variables used when the secret source is "environment".
#define REMOTE_STORAGE_ENV_FTP_PASSWORD "FLAMESHOT_FTP_PASSWORD"
#define REMOTE_STORAGE_ENV_S3_SECRET_KEY "FLAMESHOT_S3_SECRET_KEY"
#define REMOTE_STORAGE_ENV_S3_SESSION_TOKEN "FLAMESHOT_S3_SESSION_TOKEN"
