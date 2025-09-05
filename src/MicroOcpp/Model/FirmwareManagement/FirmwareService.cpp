// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019‑2024
// MIT License

#include <MicroOcpp/Model/FirmwareManagement/FirmwareService.h>
#include <MicroOcpp/Core/Context.h>
#include <MicroOcpp/Model/Model.h>
#include <MicroOcpp/Model/ConnectorBase/Connector.h>
#include <MicroOcpp/Model/Transactions/Transaction.h>
#include <MicroOcpp/Core/Configuration.h>
#include <MicroOcpp/Core/OperationRegistry.h>
#include <MicroOcpp/Core/Request.h>

#include <MicroOcpp/Operations/UpdateFirmware.h>
#include <MicroOcpp/Operations/FirmwareStatusNotification.h>

#include <MicroOcpp/Platform.h>
#include <MicroOcpp/Debug.h>

#include <Update.h>   // always required

#if MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP32)
  #include <HTTPUpdate.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#elif MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP8266)
  #include <ESP8266httpUpdate.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266WiFi.h>
#endif

#ifndef MO_IGNORE_FW_RETR_DATE
#define MO_IGNORE_FW_RETR_DATE 0
#endif

using MicroOcpp::FirmwareService;
using MicroOcpp::Ocpp16::FirmwareStatus;

/* ====================================================================== */
/*  helper: delay reboot so “Installed” is transmitted first              */
/* ====================================================================== */
void FirmwareService::scheduleDelayedRestart(unsigned long ms)
{
    restartScheduled   = true;
    restartScheduledAt = mocpp_tick_ms() + ms;
}

/* ====================================================================== */
/*  constructor                                                           */
/* ====================================================================== */
FirmwareService::FirmwareService(Context& ctx)
    : MemoryManaged("v16.Firmware.FirmwareService"),
      context(ctx),
      buildNumber(makeString(getMemoryTag())),
      location(makeString(getMemoryTag()))
{
    context.getOperationRegistry().registerOperation(
        "UpdateFirmware",
        [this]() { return new Ocpp16::UpdateFirmware(*this); });

    context.getOperationRegistry().registerOperation(
        "FirmwareStatusNotification",
        [this]() { return new Ocpp16::FirmwareStatusNotification(getFirmwareStatus()); });
}

/* ====================================================================== */
/*  public API                                                            */
/* ====================================================================== */
void FirmwareService::setBuildNumber(const char* bn)
{
    if (!bn) return;
    buildNumber = bn;
    previousBuildNumberString = declareConfiguration<const char*>("BUILD_NUMBER",
                                                                  buildNumber.c_str(),
                                                                  MO_KEYVALUE_FN,
                                                                  false, false, false);
    checkedSuccessfulFwUpdate = false;
}

/* ---------------------------------------------------------------------- */
void FirmwareService::loop()
{
    auto& now = context.getModel().getClock().now();
    
    /* ------------------------------------------------- FTP chunk handler */
    if (ftpDownload && ftpDownload->isActive()) ftpDownload->loop();
    if (ftpDownload && !ftpDownload->isActive()) {
        MO_DBG_DEBUG("Deinit FTP download");
        ftpDownload.reset();
    }

    /* ----------------------------- auto push FirmwareStatusNotification */
    if (auto n = getFirmwareStatusNotification()) context.initiateRequest(std::move(n));

    /* --------------------------------------------- honour transition delay */
    if (mocpp_tick_ms() - timestampTransition < delayTransition) goto rebootCheck;

    /* ------------------------------------------------ main state machine */
    if (retries == 0 || now < retreiveDate) goto rebootCheck;

    switch (stage) {

    case UpdateStage::Idle:
        MO_DBG_INFO("FW‑Update: start");
        if (context.getModel().getNumConnectors() > 0)
            context.getModel().getConnector(0)->setAvailabilityVolatile(false);

        if (!onDownload) {
            stage = UpdateStage::AfterDownload;
        } else {
            downloadIssued      = true;
            stage               = UpdateStage::AwaitDownload;
            timestampTransition = mocpp_tick_ms();
            delayTransition     = 2000;
        }
        break;

    case UpdateStage::AwaitDownload:
        MO_DBG_INFO("FW‑Update: invoke onDownload");
        stage = UpdateStage::Downloading;
        if (onDownload) onDownload(location.c_str());
        timestampTransition = mocpp_tick_ms();
        delayTransition     = downloadStatusInput ? 1000 : 30000;
        break;

    case UpdateStage::Downloading:
        if (!downloadStatusInput || downloadStatusInput() == DownloadStatus::Downloaded) {
            stage = UpdateStage::AfterDownload;
        } else if (downloadStatusInput() == DownloadStatus::DownloadFailed) {
            MO_DBG_INFO("FW‑Update: download failed – retry");
            retreiveDate = now + retryInterval;
            --retries;
            resetStage();
            timestampTransition = mocpp_tick_ms();
            delayTransition     = 10000;
            if (context.getModel().getNumConnectors() > 0)
                context.getModel().getConnector(0)->setAvailabilityVolatile(true);
        }
        break;

    case UpdateStage::AfterDownload: {
        bool txRunning = false;
        for (unsigned int c = 0; c < context.getModel().getNumConnectors(); ++c) {
            auto con = context.getModel().getConnector(c);
            if (con && con->getTransaction() && con->getTransaction()->isRunning()) {
                txRunning = true;
                break;
            }
        }
        if (txRunning) break;

        installationIssued  = true;
        stage               = UpdateStage::AwaitInstallation;
        timestampTransition = mocpp_tick_ms();
        delayTransition     = 2000;
        break;
    }

    case UpdateStage::AwaitInstallation:
        MO_DBG_INFO("FW‑Update: invoke onInstall");
        stage = UpdateStage::Installing;
        if (onInstall) onInstall(location.c_str());
        timestampTransition = mocpp_tick_ms();
        delayTransition     = installationStatusInput ? 1000 : 120000;
        break;

    case UpdateStage::Installing:
        if (!installationStatusInput ||
            installationStatusInput() == InstallationStatus::Installed) {

            MO_DBG_INFO("FW‑Update: finished");
            resetStage();
            stage   = UpdateStage::Installed;
            retries = 0;
            location.clear();

            if (context.getModel().getNumConnectors() > 0)
                context.getModel().getConnector(0)->setAvailabilityVolatile(true);

        } else if (installationStatusInput() == InstallationStatus::InstallationFailed) {
            MO_DBG_INFO("FW‑Update: install failed – retry");
            retreiveDate = now + retryInterval;
            --retries;
            resetStage();
            timestampTransition = mocpp_tick_ms();
            delayTransition     = 10000;
            
            if (context.getModel().getNumConnectors() > 0)
                context.getModel().getConnector(0)->setAvailabilityVolatile(true);
        }
        break;

    default: break;  // Installed / InternalError
    }

rebootCheck:
    /* ------------------------------------------------ delayed reboot (if any) */
#if MO_PLATFORM == MO_PLATFORM_ARDUINO
    if (restartScheduled && mocpp_tick_ms() > restartScheduledAt &&
        lastReportedStatus == FirmwareStatus::Installed) {
        MO_DBG_INFO("FW‑Update: rebooting now");
        ESP.restart();
    }
#endif
}

/* ---------------------------------------------------------------------- */
void FirmwareService::scheduleFirmwareUpdate(const char* loc, Timestamp rd,
                                             unsigned int r, unsigned int ri)
{
    if (!onDownload && !onInstall) {
        MO_DBG_ERR("FW service not configured");
        stage = UpdateStage::InternalError;
        return;
    }

    location      = loc;
    retreiveDate  = rd;
    retries       = r;
    retryInterval = ri;

#if MO_IGNORE_FW_RETR_DATE
    retreiveDate = context.getModel().getClock().now();
#endif

    char buf[JSONDATE_LENGTH + 1] = {};
    retreiveDate.toJsonString(buf, sizeof(buf));

    MO_DBG_INFO("Scheduled FW update:"
                "\n  url            = %s"
                "\n  retrieveDate   = %s"
                "\n  retries        = %u"
                "\n  retryInterval  = %u",
                location.c_str(), buf, retries, retryInterval);

    timestampTransition = mocpp_tick_ms();
    delayTransition     = 1000;
    resetStage();
}

/* ---------------------------------------------------------------------- */
FirmwareStatus FirmwareService::getFirmwareStatus()
{
    if (stage == UpdateStage::Installed)     return FirmwareStatus::Installed;
    if (stage == UpdateStage::InternalError) return FirmwareStatus::InstallationFailed;

    if (installationIssued) {
        if (installationStatusInput) {
            auto s = installationStatusInput();
            if (s == InstallationStatus::Installed)         return FirmwareStatus::Installed;
            if (s == InstallationStatus::InstallationFailed) return FirmwareStatus::InstallationFailed;
        }
        return FirmwareStatus::Installing;
    }

    if (downloadIssued) {
        if (downloadStatusInput) {
            auto s = downloadStatusInput();
            if (s == DownloadStatus::Downloaded)     return FirmwareStatus::Downloaded;
            if (s == DownloadStatus::DownloadFailed) return FirmwareStatus::DownloadFailed;
        }
        return FirmwareStatus::Downloading;
    }

    return FirmwareStatus::Idle;
}

/* ---------------------------------------------------------------------- */
std::unique_ptr<MicroOcpp::Request> FirmwareService::getFirmwareStatusNotification()
{
    if (!checkedSuccessfulFwUpdate && !buildNumber.empty() && previousBuildNumberString) {
        checkedSuccessfulFwUpdate = true;

        if (buildNumber.compare(previousBuildNumberString->getString())) {
            previousBuildNumberString->setString(buildNumber.c_str());
            configuration_save();
            buildNumber.clear();

            lastReportedStatus = FirmwareStatus::Installed;
            return makeRequest(new Ocpp16::FirmwareStatusNotification(lastReportedStatus));
        }
    }

    auto current = getFirmwareStatus();
    if (current != lastReportedStatus && current != FirmwareStatus::Idle) {
        lastReportedStatus = current;
        return makeRequest(new Ocpp16::FirmwareStatusNotification(current));
    }
    return nullptr;
}

/* ---------------------------------------------------------------------- */
void FirmwareService::setOnDownload(std::function<bool(const char*)> cb)          { onDownload             = std::move(cb); }
void FirmwareService::setDownloadStatusInput(std::function<DownloadStatus()> cb)  { downloadStatusInput    = std::move(cb); }
void FirmwareService::setOnInstall(std::function<bool(const char*)> cb)           { onInstall              = std::move(cb); }
void FirmwareService::setInstallationStatusInput(std::function<InstallationStatus()> cb) { installationStatusInput = std::move(cb); }

/* ---------------------------------------------------------------------- */
void FirmwareService::resetStage()
{
    stage              = UpdateStage::Idle;
    downloadIssued     = false;
    installationIssued = false;

    /* FIX #1: clear last result so the next attempt starts with “Downloading” */
    ftpDownloadStatus  = DownloadStatus::NotDownloaded;
}

/* ====================================================================== */
/*  generic download handler (FTP + HTTP/S with link validation)          */
/* ====================================================================== */
void FirmwareService::setDownloadFileWriter(
        std::function<size_t(const unsigned char*, size_t)> firmwareWriter,
        std::function<void(MO_FtpCloseReason)>              onClose)
{
    onDownload = [this, firmwareWriter, onClose](const char* url) -> bool {

        /* ----------------------------------------------------------------
         *  HTTP / HTTPS path – quick 5 s HEAD to validate reachability
         * ---------------------------------------------------------------- */
        if (strncmp(url, "ftp://", 6) != 0) {

#if MO_PLATFORM == MO_PLATFORM_ARDUINO
            ftpDownloadStatus = DownloadStatus::NotDownloaded;   /* FIX #2 */
            
            /* create client first – it must out‑live HTTPClient -------- */
        #if defined(ESP32)
            std::unique_ptr<WiFiClient>  net;
            if (strncmp(url, "https://", 8) == 0) {
                auto* ssl = new WiFiClientSecure;
                ssl->setInsecure();
                net.reset(ssl);
            } else {
                net.reset(new WiFiClient());
            }
            HTTPClient http;
            bool linkOk = false;
            if (http.begin(*net, url)) {
                http.setTimeout(5000);
                http.setReuse(false);               // ensure .end() really closes
                int code = http.sendRequest("HEAD");
                linkOk   = (code > 0 && code < 400);
                http.end();                         // closes + clears _tcp ptr
            }
        #elif defined(ESP8266)
            WiFiClient         plain;
            WiFiClientSecure   secure;
            HTTPClient         http;
            bool linkOk = false;

            bool beginOk = (strncmp(url, "https://", 8) == 0)
                               ? http.begin(secure, url)   // secure gets setInsecure() inside begin()
                               : http.begin(plain , url);

            if (beginOk) {
                http.setTimeout(5000);
                http.setReuse(false);
                int code = http.sendRequest("HEAD");
                linkOk   = (code > 0 && code < 400);
                http.end();
            }
        #endif  // ESP platform switch

            if (linkOk) {
                ftpDownloadStatus = DownloadStatus::Downloaded;
                return true;
            } else {
                MO_DBG_WARN("FW‑Update: URL unreachable – abort");
                ftpDownloadStatus = DownloadStatus::DownloadFailed;
                return false;
            }
#else   // non‑Arduino build
            ftpDownloadStatus = DownloadStatus::DownloadFailed;
            return false;
#endif
        }

        /* ----------------------------------------------------------------
         *  FTP path (unchanged)
         * ---------------------------------------------------------------- */
        auto ftpClient = context.getFtpClient();
        if (!ftpClient) {
            MO_DBG_ERR("FTP client not set");
            ftpDownloadStatus = DownloadStatus::DownloadFailed;
            return false;
        }

        ftpDownload = ftpClient->getFile(
            url,
            firmwareWriter,
            [this, onClose](MO_FtpCloseReason reason) {
                ftpDownloadStatus = (reason == MO_FtpCloseReason_Success)
                                  ? DownloadStatus::Downloaded
                                  : DownloadStatus::DownloadFailed;
                onClose(reason);
            });

        if (ftpDownload) {
            ftpDownloadStatus = DownloadStatus::NotDownloaded;
            return true;
        }

        ftpDownloadStatus = DownloadStatus::DownloadFailed;
        return false;
    };

    downloadStatusInput = [this]() { return ftpDownloadStatus; };
}


void FirmwareService::setFtpServerCert(const char* cert) { ftpServerCert = cert; }

/* ====================================================================== */
/*  Default ESP integration:  FTP or HTTPS                                */
/* ====================================================================== */
#if !defined(MO_CUSTOM_UPDATER)

#if MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP32) && MO_ENABLE_MBEDTLS
/* ---------------------------------------------------------------------- */
std::unique_ptr<FirmwareService> MicroOcpp::makeDefaultFirmwareService(Context& ctx)
{
    auto fw = std::unique_ptr<FirmwareService>(new FirmwareService(ctx));
    auto fs = fw.get();

    /* ----------------------------- FTP (chunked) writer --------------- */
    fw->setDownloadFileWriter(
        [fs](const unsigned char* data, size_t len) -> size_t {
            if (!Update.isRunning()) {
                fs->setInstallationStatusInput([] { return InstallationStatus::NotInstalled; });
                if (!Update.begin()) {
                    MO_DBG_ERR("Update.begin() failed");
                    return 0;
                }
            }
            return Update.write((uint8_t*)data, len);
        },
        [](MO_FtpCloseReason r) { if (r != MO_FtpCloseReason_Success) Update.abort(); });

    /* ----------------------------- installer (FTP finish OR HTTP/S) --- */
    fw->setOnInstall([fs](const char* url) {

        /* ---------- FTP branch (already written) ---------------------- */
        if (Update.isRunning()) {
            if (Update.end(true)) {                       // finalize, **no reboot**
                fs->setInstallationStatusInput([] { return InstallationStatus::Installed; });
                fs->scheduleDelayedRestart();             // reboot later
            } else {
                fs->setInstallationStatusInput([] { return InstallationStatus::InstallationFailed; });
            }
            return true;
        }

        /* ---------- HTTP / HTTPS one‑shot ----------------------------- */
        httpUpdate.rebootOnUpdate(false);                 // suppress auto‑reboot
        fs->setInstallationStatusInput([] { return InstallationStatus::NotInstalled; });

        WiFiClientSecure client;
        client.setTimeout(60);
        client.setInsecure();

        t_httpUpdate_return ret = httpUpdate.update(client, url);

        if (ret == HTTP_UPDATE_OK) {
            fs->setInstallationStatusInput([] { return InstallationStatus::Installed; });
            fs->scheduleDelayedRestart();
        } else {
            fs->setInstallationStatusInput([] { return InstallationStatus::InstallationFailed; });
            MO_DBG_WARN("HTTP update failed (%d)", ret);
        }
        return true;
    });

    fw->setInstallationStatusInput([] { return InstallationStatus::NotInstalled; });
    return fw;
}

/* ---------------------------------------------------------------------- */
#elif MO_PLATFORM == MO_PLATFORM_ARDUINO && defined(ESP8266)

std::unique_ptr<FirmwareService> MicroOcpp::makeDefaultFirmwareService(Context& ctx)
{
    auto fw = std::unique_ptr<FirmwareService>(new FirmwareService(ctx));
    auto fs = fw.get();

    fw->setOnInstall([fs](const char* url) {

        httpUpdate.rebootOnUpdate(false);                 // suppress auto‑reboot
        fs->setInstallationStatusInput([] { return InstallationStatus::NotInstalled; });

        WiFiClient client;                // insecure for demo; add CA for production
        client.setTimeout(60);

        HTTPUpdateResult ret = ESPhttpUpdate.update(client, url);

        if (ret == HTTP_UPDATE_OK) {
            fs->setInstallationStatusInput([] { return InstallationStatus::Installed; });
            fs->scheduleDelayedRestart();
        } else {
            fs->setInstallationStatusInput([] { return InstallationStatus::InstallationFailed; });
            MO_DBG_WARN("HTTP update failed (%d)", ret);
        }
        return true;
    });

    fw->setInstallationStatusInput([] { return InstallationStatus::NotInstalled; });
    return fw;
}
#endif
#endif // !MO_CUSTOM_UPDATER
