// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019‑2024
// MIT License

#ifndef FIRMWARESERVICE_H
#define FIRMWARESERVICE_H

#include <functional>
#include <memory>

#include <MicroOcpp/Core/ConfigurationKeyValue.h>
#include <MicroOcpp/Model/FirmwareManagement/FirmwareStatus.h>
#include <MicroOcpp/Core/Time.h>
#include <MicroOcpp/Core/Ftp.h>
#include <MicroOcpp/Core/Memory.h>

namespace MicroOcpp {

enum class DownloadStatus {
    NotDownloaded,
    Downloaded,
    DownloadFailed
};

enum class InstallationStatus {
    NotInstalled,
    Installed,
    InstallationFailed
};

class Context;
class Request;

class FirmwareService : public MemoryManaged {
private:
    Context& context;

    /* persistent build‑number check ------------------------------------ */
    std::shared_ptr<Configuration> previousBuildNumberString;
    String                          buildNumber;

    /* download path ----------------------------------------------------- */
    std::function<DownloadStatus()> downloadStatusInput;
    bool                            downloadIssued      = false;

    std::unique_ptr<FtpDownload>    ftpDownload;
    DownloadStatus                  ftpDownloadStatus   = DownloadStatus::NotDownloaded;
    const char*                     ftpServerCert       = nullptr;

    /* installation path -------------------------------------------------- */
    std::function<InstallationStatus()> installationStatusInput;
    bool                                installationIssued = false;

    /* status tracking ---------------------------------------------------- */
    Ocpp16::FirmwareStatus lastReportedStatus     = Ocpp16::FirmwareStatus::Idle;
    bool                   checkedSuccessfulFwUpdate = false;

    /* update parameters -------------------------------------------------- */
    String       location;
    Timestamp    retreiveDate;
    unsigned int retries        = 0;
    unsigned int retryInterval  = 0;

    std::function<bool(const char*)> onDownload;
    std::function<bool(const char*)> onInstall;

    /* state machine ------------------------------------------------------ */
    unsigned long delayTransition     = 0;
    unsigned long timestampTransition = 0;

    enum class UpdateStage {
        Idle,
        AwaitDownload,
        Downloading,
        AfterDownload,
        AwaitInstallation,
        Installing,
        Installed,
        InternalError
    } stage = UpdateStage::Idle;

    /* delayed‑reboot helper --------------------------------------------- */
    bool          restartScheduled   = false;
    unsigned long restartScheduledAt = 0;

    void resetStage();
    std::unique_ptr<Request> getFirmwareStatusNotification();

public:
    explicit FirmwareService(Context& context);

    /* regular service loop --------------------------------------------- */
    void loop();

    /* OCPP interface ---------------------------------------------------- */
    void scheduleFirmwareUpdate(const char* location,
                                Timestamp    retreiveDate,
                                unsigned int retries       = 1,
                                unsigned int retryInterval = 0);

    Ocpp16::FirmwareStatus getFirmwareStatus();
    void setBuildNumber(const char* buildNumber);

    /* download / install hooks ----------------------------------------- */
    void setDownloadFileWriter(std::function<size_t(const unsigned char*, size_t)> firmwareWriter,
                               std::function<void(MO_FtpCloseReason)>            onClose);

    void setFtpServerCert(const char* cert);   // zero‑copy (must live longer than MO)

    void setOnDownload(std::function<bool(const char*)> onDownload);
    void setDownloadStatusInput(std::function<DownloadStatus()> downloadStatusInput);

    void setOnInstall(std::function<bool(const char*)> onInstall);
    void setInstallationStatusInput(std::function<InstallationStatus()> installationStatusInput);
    
    void scheduleDelayedRestart(unsigned long ms = 5000);
};

} // namespace MicroOcpp

/* ---------------------------------------------------------------------- */
/* Optional default integration for ESP targets                           */
/* ---------------------------------------------------------------------- */
#if !defined(MO_CUSTOM_UPDATER)

#if MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP32) && MO_ENABLE_MBEDTLS
namespace MicroOcpp { std::unique_ptr<FirmwareService> makeDefaultFirmwareService(Context&); }
#elif MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP8266)
namespace MicroOcpp { std::unique_ptr<FirmwareService> makeDefaultFirmwareService(Context&); }
#endif

#endif // !MO_CUSTOM_UPDATER

#endif // FIRMWARESERVICE_H
