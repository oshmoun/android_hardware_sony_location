/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation, nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include <SystemStatus.h>
#include <LocationApiMsg.h>
#include <gps_extended_c.h>

#ifdef POWERMANAGER_ENABLED
#include <PowerEvtHandler.h>
#endif
#include <LocHalDaemonIPCReceiver.h>
#include <LocHalDaemonIPCSender.h>
#include <LocHalDaemonClientHandler.h>
#include <LocationApiService.h>
#include <location_interface.h>

typedef void* (getLocationInterface)();

/******************************************************************************
LocationApiService - static members
******************************************************************************/
LocationApiService* LocationApiService::mInstance = nullptr;
std::mutex LocationApiService::mMutex;

/******************************************************************************
LocationApiService - constructors
******************************************************************************/
LocationApiService::LocationApiService(uint32_t autostart, uint32_t sessiontbfms) :

    mLocationControlId(0),
    mAutoStartGnss(autostart)
#ifdef POWERMANAGER_ENABLED
    ,mPowerEventObserver(nullptr)
#endif
    {

    LOC_LOGd("AutoStartGnss=%u", mAutoStartGnss);
    LOC_LOGd("GnssSessionTbfMs=%u", sessiontbfms);

    // create Location control API
    mControlCallabcks.size = sizeof(mControlCallabcks);
    mControlCallabcks.responseCb = [this](LocationError err, uint32_t id) {
        onControlResponseCallback(err, id);
    };
    mControlCallabcks.collectiveResponseCb =
            [this](size_t count, LocationError *errs, uint32_t *ids) {
        onControlCollectiveResponseCallback(count, errs, ids);
    };
    mLocationControlApi = LocationControlAPI::createInstance(mControlCallabcks);
    if (nullptr == mLocationControlApi) {
        LOC_LOGd("Failed to create LocationControlAPI");
        return;
    }

    // enable
    mLocationControlId = mLocationControlApi->enable(LOCATION_TECHNOLOGY_TYPE_GNSS);
    LOC_LOGd("-->enable=%u", mLocationControlId);
    // this is a unique id assigned to this daemon - will be used when disable

    // create IPC receiver
    mIpcReceiver = new LocHalDaemonIPCReceiver(this);
    if (nullptr == mIpcReceiver) {
        LOC_LOGd("Failed to create LocHalDaemonIPCReceiver");
        return;
    }

#ifdef POWERMANAGER_ENABLED
    // register power event handler
    mPowerEventObserver = PowerEvtHandler::getPwrEvtHandler(this);
    if (nullptr == mPowerEventObserver) {
        LOC_LOGe("Failed to regiseter Powerevent handler");
        return;
    }
#endif

    // create a default client if enabled by config
    if (mAutoStartGnss) {
        LOC_LOGd("--> Starting a default client...");
        LocHalDaemonClientHandler* pClient = new LocHalDaemonClientHandler(this, "default");
        mClients.emplace("default", pClient);

        pClient->updateSubscription(
                E_LOC_CB_GNSS_LOCATION_INFO_BIT | E_LOC_CB_GNSS_SV_BIT);

        pClient->startTracking(0, sessiontbfms);
        pClient->mTracking = true;
        pClient->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);
    }

    // start receiver - never return
    LOC_LOGd("Ready");
    mIpcReceiver->start();
}

LocationApiService::~LocationApiService() {

    // stop ipc receiver thread
    if (nullptr != mIpcReceiver) {
        mIpcReceiver->stop();
        delete mIpcReceiver;
    }

    // free resource associated with the client
    for (auto each : mClients) {
        LOC_LOGd(">-- deleted client [%s]", each.first.c_str());
        each.second->cleanup();
    }

    // delete location contorol API handle
    mLocationControlApi->disable(mLocationControlId);
    mLocationControlApi->destroy();
}

void LocationApiService::onListenerReady() {

    // traverse client sockets directory - then broadcast READY message
    LOC_LOGd(">-- onListenerReady Finding client sockets...");

    DIR *dirp = opendir(SOCKET_DIR_TO_CLIENT);
    if (!dirp) {
        return;
    }

    struct dirent *dp = nullptr;
    struct stat sbuf = {0};
    const std::string fnamebase = SOCKET_TO_LOCATION_CLIENT_BASE;
    while (nullptr != (dp = readdir(dirp))) {
        std::string fname = SOCKET_DIR_TO_CLIENT;
        fname += dp->d_name;
        if (-1 == lstat(fname.c_str(), &sbuf)) {
            continue;
        }
        if ('.' == (dp->d_name[0])) {
            continue;
        }
        if (0 != fname.compare(0, fnamebase.size(), fnamebase)) {
            continue;
        }
        if (S_ISSOCK((mode_t)sbuf.st_mode)) {
            LOC_LOGd("<-- Sending ready to socket: %s", fname.c_str());
            LocHalDaemonIPCSender* pIpcSender = new LocHalDaemonIPCSender(fname.c_str());
            LocAPIHalReadyIndMsg msg(SERVICE_NAME);
            pIpcSender->send(reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
            delete pIpcSender;
        }
    }
    closedir(dirp);
}

/******************************************************************************
LocationApiService - implementation - registration
******************************************************************************/
void LocationApiService::newClient(LocAPIClientRegisterReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    std::string clientname(pMsg->mSocketName);

    // if this name is already used return error
    if (mClients.find(clientname) != mClients.end()) {
        LOC_LOGe("invalid client=%s already existing", clientname.c_str());
        return;
    }

    // store it in client property database
    LocHalDaemonClientHandler *pClient = new LocHalDaemonClientHandler(this, clientname);
    if (!pClient) {
        LOC_LOGe("failed to register client=%s", clientname.c_str());
        return;
    }

    mClients.emplace(clientname, pClient);
    LOC_LOGi(">-- registered new client=%s", clientname.c_str());
}

void LocationApiService::deleteClient(LocAPIClientDeregisterReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    std::string clientname(pMsg->mSocketName);
    deleteClientbyName(clientname);
}

void LocationApiService::deleteClientbyName(const std::string clientname) {

    // delete this client from property db
    LocHalDaemonClientHandler* pClient = getClient(clientname);

    if (!pClient) {
        LOC_LOGe(">-- deleteClient invlalid client=%s", clientname.c_str());
        return;
    }
    mClients.erase(clientname);
    pClient->cleanup();

    LOC_LOGi(">-- deleteClient client=%s", clientname.c_str());
}
/******************************************************************************
LocationApiService - implementation - tracking
******************************************************************************/
void LocationApiService::startTracking(LocAPIStartTrackingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invlalid client=%s", pMsg->mSocketName);
        return;
    }

    if (!pClient->startTracking(pMsg->distanceInMeters, pMsg->intervalInMs)) {
        LOC_LOGe("Failed to start session");
        return;
    }
    // success
    pClient->mTracking = true;
    pClient->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);

    LOC_LOGi(">-- start started session");
    return;
}

void LocationApiService::stopTracking(LocAPIStopTrackingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- stop invlalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->mTracking = false;
    pClient->unsubscribeLocationSessionCb();
    pClient->stopTracking();
    pClient->mPendingMessages.push(E_LOCAPI_STOP_TRACKING_MSG_ID);
    LOC_LOGi(">-- stopping session");
}

void LocationApiService::updateSubscription(LocAPIUpdateCallbacksReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- updateSubscription invlalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->updateSubscription(pMsg->locationCallbacks);

    LOC_LOGi(">-- update subscription client=%s mask=0x%x",
            pMsg->mSocketName, pMsg->locationCallbacks);
}

void LocationApiService::updateTrackingOptions(LocAPIUpdateTrackingOptionsReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);

    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (pClient) {
        pClient->updateTrackingOptions(pMsg->distanceInMeters, pMsg->intervalInMs);
        pClient->mPendingMessages.push(E_LOCAPI_UPDATE_TRACKING_OPTIONS_MSG_ID);
    }

    LOC_LOGi(">-- update tracking options");
}

void LocationApiService::updateNetworkAvailability(bool availability) {

    LOC_LOGi(">-- updateNetworkAvailability=%u", availability);
    GnssInterface* gnssInterface = getGnssInterface();
    if (gnssInterface) {
        gnssInterface->updateConnectionStatus(
                availability, loc_core::NetworkInfoDataItemBase::TYPE_UNKNOWN);
    }
}

void LocationApiService::getGnssEnergyConsumed(const char* clientSocketName) {

    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGi(">-- getGnssEnergyConsumed by=%s", clientSocketName);

    GnssInterface* gnssInterface = getGnssInterface();
    if (!gnssInterface) {
        LOC_LOGe(">-- getGnssEnergyConsumed null GnssInterface");
        return;
    }

    bool requestAlreadyPending = false;
    for (auto each : mClients) {
        if ((each.second != nullptr) &&
            (each.second->hasPendingEngineInfoRequest(E_ENGINE_INFO_CB_GNSS_ENERGY_CONSUMED_BIT))) {
            requestAlreadyPending = true;
            break;
        }
    }

    std::string clientname(clientSocketName);
    LocHalDaemonClientHandler* pClient = getClient(clientname);
    if (pClient) {
        pClient->addEngineInfoRequst(E_ENGINE_INFO_CB_GNSS_ENERGY_CONSUMED_BIT);

        // this is first client coming to request GNSS energy consumed
        if (requestAlreadyPending == false) {
            LOC_LOGd("--< issue request to GNSS HAL");

            // callback function for engine hub to report back sv event
            GnssEnergyConsumedCallback reportEnergyCb =
                [this](uint64_t total) {
                    onGnssEnergyConsumedCb(total);
                };

            gnssInterface->getGnssEnergyConsumed(reportEnergyCb);
        }
    }
}

/******************************************************************************
LocationApiService - implementation - batching
******************************************************************************/
void LocationApiService::startBatching(LocAPIStartBatchingReqMsg *pMsg) {

    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invalid client=%s", pMsg->mSocketName);
        return;
    }

    if (!pClient->startBatching(pMsg->intervalInMs, pMsg->distanceInMeters,
                pMsg->batchingMode)) {
        LOC_LOGe("Failed to start session");
        return;
    }
    // success
    pClient->mBatching = true;
    pClient->mBatchingMode = pMsg->batchingMode;
    pClient->mPendingMessages.push(E_LOCAPI_START_BATCHING_MSG_ID);

    LOC_LOGi(">-- start batching session");
    return;
}

void LocationApiService::stopBatching(LocAPIStopBatchingReqMsg *pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- stop invalid client=%s", pMsg->mSocketName);
        return;
    }

    pClient->mBatching = false;
    pClient->mBatchingMode = BATCHING_MODE_NO_AUTO_REPORT;
    pClient->updateSubscription(0);
    pClient->stopBatching();
    pClient->mPendingMessages.push(E_LOCAPI_STOP_BATCHING_MSG_ID);
    LOC_LOGi(">-- stopping batching session");
}

void LocationApiService::updateBatchingOptions(LocAPIUpdateBatchingOptionsReqMsg *pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (pClient) {
        pClient->updateBatchingOptions(pMsg->intervalInMs, pMsg->distanceInMeters,
                pMsg->batchingMode);
        pClient->mPendingMessages.push(E_LOCAPI_UPDATE_BATCHING_OPTIONS_MSG_ID);
    }

    LOC_LOGi(">-- update batching options");
}

/******************************************************************************
LocationApiService - implementation - geofence
******************************************************************************/
void LocationApiService::addGeofences(LocAPIAddGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- start invlalid client=%s", pMsg->mSocketName);
        return;
    }
    GeofenceOption* gfOptions =
            (GeofenceOption*)malloc(pMsg->geofences.count * sizeof(GeofenceOption));
    GeofenceInfo* gfInfos = (GeofenceInfo*)malloc(pMsg->geofences.count * sizeof(GeofenceInfo));
    uint32_t* clientIds = (uint32_t*)malloc(pMsg->geofences.count * sizeof(uint32_t));

    for(int i=0; i < pMsg->geofences.count; ++i) {
        gfOptions[i] = (*(pMsg->geofences.gfPayload + i)).gfOption;
        gfInfos[i] = (*(pMsg->geofences.gfPayload + i)).gfInfo;
        clientIds[i] = (*(pMsg->geofences.gfPayload + i)).gfClientId;
    }

    uint32_t* sessions = pClient->addGeofences(pMsg->geofences.count, gfOptions, gfInfos);
    if (!sessions) {
        LOC_LOGe("Failed to add geofences");
        free(clientIds);
        free(gfInfos);
        free(gfOptions);
        return;
    }
    pClient->setGeofenceIds(pMsg->geofences.count, clientIds, sessions);
    // success
    pClient->mGfPendingMessages.push(E_LOCAPI_ADD_GEOFENCES_MSG_ID);

    LOC_LOGi(">-- add geofences");
    free(clientIds);
    free(gfInfos);
    free(gfOptions);
}

void LocationApiService::removeGeofences(LocAPIRemoveGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->removeGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_REMOVE_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- remove geofences");
    free(sessions);
}
void LocationApiService::modifyGeofences(LocAPIModifyGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    GeofenceOption* gfOptions = (GeofenceOption*)
            malloc(sizeof(GeofenceOption) * pMsg->geofences.count);
    uint32_t* clientIds = (uint32_t*)malloc(sizeof(uint32_t) * pMsg->geofences.count);
    for (int i=0; i<pMsg->geofences.count; ++i) {
        gfOptions[i] = (*(pMsg->geofences.gfPayload + i)).gfOption;
        clientIds[i] = (*(pMsg->geofences.gfPayload + i)).gfClientId;
    }
    uint32_t* sessions = pClient->getSessionIds(pMsg->geofences.count, clientIds);

    if (pClient && sessions) {
        pClient->modifyGeofences(pMsg->geofences.count, sessions, gfOptions);
        pClient->mGfPendingMessages.push(E_LOCAPI_MODIFY_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- modify geofences");
    free(sessions);
    free(clientIds);
    free(gfOptions);
}
void LocationApiService::pauseGeofences(LocAPIPauseGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->pauseGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_PAUSE_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- pause geofences");
    free(sessions);
}
void LocationApiService::resumeGeofences(LocAPIResumeGeofencesReqMsg* pMsg) {
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    uint32_t* sessions = pClient->getSessionIds(pMsg->gfClientIds.count, pMsg->gfClientIds.gfIds);
    if (pClient && sessions) {
        pClient->resumeGeofences(pMsg->gfClientIds.count, sessions);
        pClient->mGfPendingMessages.push(E_LOCAPI_RESUME_GEOFENCES_MSG_ID);
    }

    LOC_LOGi(">-- resume geofences");
    free(sessions);
}

void LocationApiService::pingTest(LocAPIPingTestReqMsg* pMsg) {

    // test only - ignore this request when config is not enabled
    std::lock_guard<std::mutex> lock(mMutex);
    LocHalDaemonClientHandler* pClient = getClient(pMsg->mSocketName);
    if (!pClient) {
        LOC_LOGe(">-- pingTest invlalid client=%s", pMsg->mSocketName);
        return;
    }
    pClient->pingTest();
    LOC_LOGd(">-- pingTest");
}

/******************************************************************************
LocationApiService - Location Control API callback functions
******************************************************************************/
void LocationApiService::onControlResponseCallback(LocationError err, uint32_t id) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onControlResponseCallback err=%u id=%u", err, id);
}

void LocationApiService::onControlCollectiveResponseCallback(
    size_t count, LocationError *errs, uint32_t *ids) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onControlCollectiveResponseCallback");
}

#ifdef POWERMANAGER_ENABLED
/******************************************************************************
LocationApiService - power event handlers
******************************************************************************/
void LocationApiService::onSuspend() {

    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onSuspend");

    for (auto client : mClients) {
        // stop session if running
        if (client.second) {
            client.second->stopTracking();
            client.second->mPendingMessages.push(E_LOCAPI_STOP_TRACKING_MSG_ID);
            LOC_LOGi("--> suspended");
        }
    }
}

void LocationApiService::onResume() {

    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onResume");

    for (auto client : mClients) {
        // start session if not running
        if (client.second && client.second->mTracking) {

            // resume session with preserved options
            if (!client.second->startTracking()) {
                LOC_LOGe("Failed to start session");
                return;
            }
            // success
            client.second->mPendingMessages.push(E_LOCAPI_START_TRACKING_MSG_ID);
            LOC_LOGi("--> resumed");
        }
    }
}

void LocationApiService::onShutdown() {

    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onShutdown");
    // stop session if running
    onSuspend();
}
#endif

/******************************************************************************
LocationApiService - on query callback from location engines
******************************************************************************/
void LocationApiService::onGnssEnergyConsumedCb(uint64_t totalGnssEnergyConsumedSinceFirstBoot) {
    std::lock_guard<std::mutex> lock(mMutex);
    LOC_LOGd("--< onGnssEnergyConsumedCb");

    LocAPIGnssEnergyConsumedIndMsg msg(SERVICE_NAME, totalGnssEnergyConsumedSinceFirstBoot);
    for (auto each : mClients) {
        // deliver the engergy info to registered client
        each.second->onGnssEnergyConsumedInfoAvailable(msg);
    }
}

/******************************************************************************
LocationApiService - other utilities
******************************************************************************/
GnssInterface* LocationApiService::getGnssInterface() {

    static bool getGnssInterfaceFailed = false;
    static GnssInterface* gnssInterface = nullptr;

    if (nullptr == gnssInterface && !getGnssInterfaceFailed) {
        LOC_LOGd("Loading libgnss.so::getGnssInterface ...");
        getLocationInterface* getter = NULL;
        const char *error = NULL;
        dlerror();
        void *handle = dlopen("libgnss.so", RTLD_NOW);
        if (nullptr == handle) {
            LOC_LOGe("dlopen for libgnss.so failed");
        } else if (nullptr != (error = dlerror()))  {
            LOC_LOGe("dlopen for libgnss.so failed, error = %s", error);
        } else {
            getter = (getLocationInterface*)dlsym(handle, "getGnssInterface");
            if ((error = dlerror()) != NULL)  {
                LOC_LOGe("dlsym for getGnssInterface failed, error = %s", error);
                getter = NULL;
            }
        }

        if (nullptr == getter) {
            getGnssInterfaceFailed = true;
        } else {
            gnssInterface = (GnssInterface*)(*getter)();
        }
    }
    return gnssInterface;
}

