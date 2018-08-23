/* Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
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
 *     * Neither the name of The Linux Foundatoin, nor the names of its
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
#define LOG_NDEBUG 0
#define LOG_TAG "LocSvc_ApiV02"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include <math.h>
#include <dlfcn.h>

#include <LocApiV02.h>
#include <loc_api_v02_log.h>
#include <loc_api_sync_req.h>
#include <loc_api_v02_client.h>
#include <loc_util_log.h>
#include <gps_extended.h>
#include "loc_pla.h"
#include <loc_cfg.h>
#include <LocDualContext.h>

using namespace loc_core;

/* Doppler Conversion from M/S to NS/S */
#define MPS_TO_NSPS         (1.0/0.299792458)

/* Default session id ; TBD needs incrementing for each */
#define LOC_API_V02_DEF_SESSION_ID (1)

/* UMTS CP Address key*/
#define LOC_NI_NOTIF_KEY_ADDRESS           "Address"

/* GPS SV Id offset */
#define GPS_SV_ID_OFFSET        (1)

/* GLONASS SV Id offset */
#define GLONASS_SV_ID_OFFSET    (65)

/* SV ID range */
#define SV_ID_RANGE             (32)

#define BDS_SV_ID_OFFSET         (201)

/* BeiDou SV ID RANGE*/
#define BDS_SV_ID_RANGE          QMI_LOC_DELETE_MAX_BDS_SV_INFO_LENGTH_V02

/* GPS week unknown*/
#define C_GPS_WEEK_UNKNOWN      (65535)

/* seconds per week*/
#define WEEK_MSECS              (60*60*24*7*1000)

/* number of QMI_LOC messages that need to be checked*/
#define NUMBER_OF_MSG_TO_BE_CHECKED        (3)

/* the time, in seconds, to wait for user response for NI  */
#define LOC_NI_NO_RESPONSE_TIME 20

#define GPS_L1CA_CARRIER_FREQUENCY      1575420000.0
#define GPS_L1C_CARRIER_FREQUENCY       1575420000.0
#define GPS_L2C_L_CARRIER_FREQUENCY     1227600000.0
#define GPS_L5_Q_CARRIER_FREQUENCY      1176450000.0
#define GLONASS_G1_CARRIER_FREQUENCY    1602000000.0
#define GLONASS_G2_CARRIER_FREQUENCY    1246000000.0
#define GALILEO_E1_C_CARRIER_FREQUENCY  1575420000.0
#define GALILEO_E5A_Q_CARRIER_FREQUENCY 1176450000.0
#define GALILEO_E5B_Q_CARRIER_FREQUENCY 1207140000.0
#define BEIDOU_B1_I_CARRIER_FREQUENCY   1561098000.0
#define BEIDOU_B1C_CARRIER_FREQUENCY    1575420000.0
#define BEIDOU_B2_I_CARRIER_FREQUENCY   1207140000.0
#define BEIDOU_B2A_I_CARRIER_FREQUENCY  1176450000.0
#define QZSS_L1CA_CARRIER_FREQUENCY     1575420000.0
#define QZSS_L1S_CARRIER_FREQUENCY      1575420000.0
#define QZSS_L2C_L_CARRIER_FREQUENCY    1227600000.0
#define QZSS_L5_Q_CARRIER_FREQUENCY     1176450000.0
#define SBAS_L1_CA_CARRIER_FREQUENCY    1575420000.0

/* Gaussian 2D scaling table - scale from x% to 68% confidence */
struct conf_scaler_to_68_pair {
    uint8_t confidence;
    float scaler_to_68;
};
/* length of confScalers array */
#define CONF_SCALER_ARRAY_MAX   (3)
const struct conf_scaler_to_68_pair confScalers[CONF_SCALER_ARRAY_MAX] = {
    {39, 1.517}, // 0 - 39 . Index 0
    {50, 1.287}, // 40 - 50. Index 1
    {63, 1.072}, // 51 - 63. Index 2
};

/*fixed timestamp uncertainty 10 milli second */
static int ap_timestamp_uncertainty = 0;
static loc_param_s_type gps_conf_param_table[] =
{
        {"AP_TIMESTAMP_UNCERTAINTY",&ap_timestamp_uncertainty,NULL,'n'}
};

/* static event callbacks that call the LocApiV02 callbacks*/

/* global event callback, call the eventCb function in loc api adapter v02
   instance */
static void globalEventCb(locClientHandleType clientHandle,
                          uint32_t eventId,
                          const locClientEventIndUnionType eventPayload,
                          void*  pClientCookie)
{
  MODEM_LOG_CALLFLOW(%s, loc_get_v02_event_name(eventId));
  LocApiV02 *locApiV02Instance =
      (LocApiV02 *)pClientCookie;

  LOC_LOGv ("client = %p, event id = 0x%X, client cookie ptr = %p",
             clientHandle, eventId, pClientCookie);

  // return if null is passed
  if( NULL == locApiV02Instance)
  {
    LOC_LOGe ("NULL object passed : client = %p, event id = 0x%X",
              clientHandle, eventId);
    return;
  }
  locApiV02Instance->eventCb(clientHandle, eventId, eventPayload);
}

/* global response callback, it calls the sync request process
   indication function to unblock the request that is waiting on this
   response indication*/
static void globalRespCb(locClientHandleType clientHandle,
                         uint32_t respId,
                         const locClientRespIndUnionType respPayload,
                         uint32_t respPayloadSize,
                         void*  pClientCookie)
{
  MODEM_LOG_CALLFLOW(%s, loc_get_v02_event_name(respId));
  LocApiV02 *locApiV02Instance =
        (LocApiV02 *)pClientCookie;

  LOC_LOGV ("%s:%d] client = %p, resp id = %d, client cookie ptr = %p\n",
                  __func__,  __LINE__,  clientHandle, respId, pClientCookie);

  if( NULL == locApiV02Instance)
  {
    LOC_LOGE ("%s:%d] NULL object passed : client = %p, resp id = %d\n",
                  __func__,  __LINE__,  clientHandle, respId);
    return;
  }

  switch(respId)
  {
    case QMI_LOC_GET_AVAILABLE_WWAN_POSITION_IND_V02:
      if (respPayload.pGetAvailWwanPositionInd != NULL) {
          locApiV02Instance->handleWwanZppFixIndication(*respPayload.pGetAvailWwanPositionInd);
      }
      break;
    case QMI_LOC_GET_BEST_AVAILABLE_POSITION_IND_V02:
        if (respPayload.pGetBestAvailablePositionInd != NULL) {
            locApiV02Instance->handleZppBestAvailableFixIndication(
                    *respPayload.pGetBestAvailablePositionInd);
        }
       // Call loc_sync_process_ind below also
    default:
      // process the sync call
      // use pDeleteAssistDataInd as a dummy pointer
      loc_sync_process_ind(clientHandle, respId,
          (void *)respPayload.pDeleteAssistDataInd, respPayloadSize);
      break;
  }
}

/* global error callback, it will call the handle service down
   function in the loc api adapter instance. */
static void globalErrorCb (locClientHandleType clientHandle,
                           locClientErrorEnumType errorId,
                           void *pClientCookie)
{
  LocApiV02 *locApiV02Instance =
          (LocApiV02 *)pClientCookie;

  LOC_LOGV ("%s:%d] client = %p, error id = %d\n, client cookie ptr = %p\n",
                  __func__,  __LINE__,  clientHandle, errorId, pClientCookie);
  if( NULL == locApiV02Instance)
  {
    LOC_LOGE ("%s:%d] NULL object passed : client = %p, error id = %d\n",
                  __func__,  __LINE__,  clientHandle, errorId);
    return;
  }
  locApiV02Instance->errorCb(clientHandle, errorId);
}

/* global structure containing the callbacks */
locClientCallbacksType globalCallbacks =
{
    sizeof(locClientCallbacksType),
    globalEventCb,
    globalRespCb,
    globalErrorCb
};

static void getInterSystemTimeBias(const char* interSystem,
                                   Gnss_InterSystemBiasStructType &interSystemBias,
                                   const qmiLocInterSystemBiasStructT_v02* pInterSysBias)
{
    LOC_LOGV("%s] Mask:%d, TimeBias:%f, TimeBiasUnc:%f,\n",
             interSystem, pInterSysBias->validMask, pInterSysBias->timeBias,
             pInterSysBias->timeBiasUnc);

    interSystemBias.validMask    = pInterSysBias->validMask;
    interSystemBias.timeBias     = pInterSysBias->timeBias;
    interSystemBias.timeBiasUnc  = pInterSysBias->timeBiasUnc;
}

/* Constructor for LocApiV02 */
LocApiV02 :: LocApiV02(LOC_API_ADAPTER_EVENT_MASK_T exMask,
                       ContextBase* context):
    LocApiBase(exMask, context),
    clientHandle(LOC_CLIENT_INVALID_HANDLE_VALUE),
    mQmiMask(0), mInSession(false),
    mEngineOn(false), mMeasurementsStarted(false),
    mIsMasterRegistered(false)
{
  // initialize loc_sync_req interface
  loc_sync_req_init();

  UTIL_READ_CONF(LOC_PATH_GPS_CONF,gps_conf_param_table);
}

/* Destructor for LocApiV02 */
LocApiV02 :: ~LocApiV02()
{
    close();
}

LocApiBase* getLocApi(LOC_API_ADAPTER_EVENT_MASK_T exMask,
                      ContextBase* context)
{
    return (LocApiBase*)LocApiV02::createLocApiV02(exMask, context);
}

LocApiBase* LocApiV02::createLocApiV02(LOC_API_ADAPTER_EVENT_MASK_T exMask,
                                       ContextBase* context)
{
    LOC_LOGD("%s:%d]: Creating new LocApiV02", __func__, __LINE__);
    return new LocApiV02(exMask, context);
}

/* Initialize a loc api v02 client AND
   check which loc message are supported by modem */
enum loc_api_adapter_err
LocApiV02 :: open(LOC_API_ADAPTER_EVENT_MASK_T mask)
{
  enum loc_api_adapter_err rtv = LOC_API_ADAPTER_ERR_SUCCESS;
  locClientStatusEnumType status = eLOC_CLIENT_SUCCESS;

  LOC_API_ADAPTER_EVENT_MASK_T newMask = mask & ~mExcludedMask;
  locClientEventMaskType qmiMask = 0;

  LOC_LOGd("%p Enter mMask: 0x%" PRIx64 "  mQmiMask: 0x%" PRIx64 " mExcludedMask: 0x%" PRIx64 "",
           clientHandle, mMask, mQmiMask, mExcludedMask);

  /* If the client is already open close it first */
  if(LOC_CLIENT_INVALID_HANDLE_VALUE == clientHandle)
  {
    LOC_LOGV ("%s:%d]: reference to this = %p passed in \n",
              __func__, __LINE__, this);
    /* initialize the loc api v02 interface, note that
       the locClientOpen() function will block if the
       service is unavailable for a fixed time out */

    // it is important to cap the mask here, because not all LocApi's
    // can enable the same bits, e.g. foreground and bckground.
    status = locClientOpen(0, &globalCallbacks, &clientHandle, (void *)this);
    if (eLOC_CLIENT_SUCCESS != status ||
        clientHandle == LOC_CLIENT_INVALID_HANDLE_VALUE )
    {
      mMask = 0;
      mQmiMask = 0;
      LOC_LOGE ("%s:%d]: locClientOpen failed, status = %s\n", __func__,
                __LINE__, loc_get_v02_client_status_name(status));
      rtv = LOC_API_ADAPTER_ERR_FAILURE;
    } else {
        uint64_t supportedMsgList = 0;
        const uint32_t msgArray[NUMBER_OF_MSG_TO_BE_CHECKED] =
        {
            // For - LOC_API_ADAPTER_MESSAGE_LOCATION_BATCHING
            QMI_LOC_GET_BATCH_SIZE_REQ_V02,

            // For - LOC_API_ADAPTER_MESSAGE_BATCHED_GENFENCE_BREACH
            QMI_LOC_EVENT_GEOFENCE_BATCHED_BREACH_NOTIFICATION_IND_V02,

            // For - LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_TRACKING
            QMI_LOC_START_DBT_REQ_V02
        };

        bool gnssMeasurementSupported = false;
        if (isMaster()) {
            checkRegisterMaster();
            gnssMeasurementSupported = cacheGnssMeasurementSupport();
            if (gnssMeasurementSupported) {
                /* Indicate that QMI LOC message for GNSS measurement was sent */
                mQmiMask |= QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02;
            }
        }

        // check the modem
        status = locClientSupportMsgCheck(clientHandle,
                                          msgArray,
                                          NUMBER_OF_MSG_TO_BE_CHECKED,
                                          &supportedMsgList);
        if (eLOC_CLIENT_SUCCESS != status) {
            LOC_LOGE("%s:%d]: Failed to checking QMI_LOC message supported. \n",
                     __func__, __LINE__);
        }

        /** if batching is supported , check if the adaptive batching or
            distance-based batching is supported. */
        uint32_t messageChecker = 1 << LOC_API_ADAPTER_MESSAGE_LOCATION_BATCHING;
        if ((messageChecker & supportedMsgList) == messageChecker) {
            locClientReqUnionType req_union;
            locClientStatusEnumType status = eLOC_CLIENT_SUCCESS;
            qmiLocQueryAonConfigReqMsgT_v02 queryAonConfigReq;
            qmiLocQueryAonConfigIndMsgT_v02 queryAonConfigInd;

            memset(&queryAonConfigReq, 0, sizeof(queryAonConfigReq));
            memset(&queryAonConfigInd, 0, sizeof(queryAonConfigInd));
            queryAonConfigReq.transactionId = LOC_API_V02_DEF_SESSION_ID;

            req_union.pQueryAonConfigReq = &queryAonConfigReq;
            status = locSyncSendReq(QMI_LOC_QUERY_AON_CONFIG_REQ_V02,
                                    req_union,
                                    LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                                    QMI_LOC_QUERY_AON_CONFIG_IND_V02,
                                    &queryAonConfigInd);

            if (status == eLOC_CLIENT_FAILURE_UNSUPPORTED) {
                LOC_LOGE("%s:%d]: Query AON config is not supported.\n", __func__, __LINE__);
            } else {
                if (status != eLOC_CLIENT_SUCCESS ||
                    queryAonConfigInd.status != eQMI_LOC_SUCCESS_V02) {
                    LOC_LOGE("%s:%d]: Query AON config failed."
                             " status: %s, ind status:%s\n",
                             __func__, __LINE__,
                             loc_get_v02_client_status_name(status),
                             loc_get_v02_qmi_status_name(queryAonConfigInd.status));
                } else {
                    LOC_LOGD("%s:%d]: Query AON config succeeded. aonCapability is %d.\n",
                             __func__, __LINE__, queryAonConfigInd.aonCapability);
                    if (queryAonConfigInd.aonCapability_valid) {
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_TIME_BASED_BATCHING_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: LB 1.0 is supported.\n", __func__, __LINE__);
                        }
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_AUTO_BATCHING_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: LB 1.5 is supported.\n", __func__, __LINE__);
                            supportedMsgList |=
                                (1 << LOC_API_ADAPTER_MESSAGE_ADAPTIVE_LOCATION_BATCHING);
                        }
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_DISTANCE_BASED_BATCHING_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: LB 2.0 is supported.\n", __func__, __LINE__);
                            supportedMsgList |=
                                (1 << LOC_API_ADAPTER_MESSAGE_DISTANCE_BASE_LOCATION_BATCHING);
                        }
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_DISTANCE_BASED_TRACKING_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: DBT 2.0 is supported.\n", __func__, __LINE__);
                        }
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_UPDATE_TBF_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: Updating tracking TBF on the fly is supported.\n",
                                     __func__, __LINE__);
                            supportedMsgList |=
                                (1 << LOC_API_ADAPTER_MESSAGE_UPDATE_TBF_ON_THE_FLY);
                        }
                        if (queryAonConfigInd.aonCapability |
                            QMI_LOC_MASK_AON_OUTDOOR_TRIP_BATCHING_SUPPORTED_V02) {
                            LOC_LOGD("%s:%d]: OTB is supported.\n",
                                     __func__, __LINE__);
                            supportedMsgList |=
                                (1 << LOC_API_ADAPTER_MESSAGE_OUTDOOR_TRIP_BATCHING);
                        }
                    } else {
                        LOC_LOGE("%s:%d]: AON capability is invalid.\n", __func__, __LINE__);
                    }
                }
            }
        }
        LOC_LOGV("%s:%d]: supportedMsgList is %" PRIu64 ". \n",
                 __func__, __LINE__, supportedMsgList);

        // Query for supported feature list
        locClientReqUnionType req_union;
        locClientStatusEnumType status = eLOC_CLIENT_SUCCESS;
        qmiLocGetSupportedFeatureReqMsgT_v02 getSupportedFeatureList_req;
        qmiLocGetSupportedFeatureIndMsgT_v02 getSupportedFeatureList_ind;

        memset(&getSupportedFeatureList_req, 0, sizeof(getSupportedFeatureList_req));
        memset(&getSupportedFeatureList_ind, 0, sizeof(getSupportedFeatureList_ind));

        req_union.pGetSupportedFeatureReq = &getSupportedFeatureList_req;
        status = locSyncSendReq(QMI_LOC_GET_SUPPORTED_FEATURE_REQ_V02,
                                req_union,
                                LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                                QMI_LOC_GET_SUPPORTED_FEATURE_IND_V02,
                                &getSupportedFeatureList_ind);
        if (eLOC_CLIENT_SUCCESS != status) {
            LOC_LOGE("%s:%d:%d]: Failed to get features supported from "
                     "QMI_LOC_GET_SUPPORTED_FEATURE_REQ_V02. \n", __func__, __LINE__, status);
        } else {
            LOC_LOGD("%s:%d:%d]: Got list of features supported of length:%d ",
                     __func__, __LINE__, status, getSupportedFeatureList_ind.feature_len);
            for (uint32_t i = 0; i < getSupportedFeatureList_ind.feature_len; i++) {
                LOC_LOGD("Bit-mask of supported features at index:%d is %d",i,
                         getSupportedFeatureList_ind.feature[i]);
            }
        }

        // cache the mpss engine capabilities
       mContext->setEngineCapabilities(supportedMsgList,
            (getSupportedFeatureList_ind.feature_len != 0 ? getSupportedFeatureList_ind.feature:
            NULL), gnssMeasurementSupported);

       // broadcast handle engine up event
        handleEngineUpEvent();
    }
  }

  if ((eLOC_CLIENT_SUCCESS == status) && (LOC_CLIENT_INVALID_HANDLE_VALUE != clientHandle)) {
    qmiMask = convertMask(newMask);

    LOC_LOGd("clientHandle = %p mMask: 0x%" PRIx64 " Adapter mask: 0x%" PRIx64 " "
             "newMask: 0x%" PRIx64 " mQmiMask: 0x%" PRIx64 " qmiMask: 0x%" PRIx64 "",
             clientHandle, mMask, mask, newMask, mQmiMask, qmiMask);
    checkRegisterMaster();

    if ((mQmiMask ^ qmiMask) & qmiMask & QMI_LOC_EVENT_MASK_WIFI_REQ_V02) {
        wifiStatusInformSync();
    }

    if (newMask != mMask) {
      locClientEventMaskType maskDiff = qmiMask ^ mQmiMask;
      // it is important to cap the mask here, because not all LocApi's
      // can enable the same bits, e.g. foreground and background.
      if (!registerEventMask(qmiMask)) {
        // we do not update mMask here, because it did not change
        // as the mask update has failed.
        rtv = LOC_API_ADAPTER_ERR_FAILURE;
      }
      else {
        mMask = newMask;
        mQmiMask = qmiMask;
      }
      if (isMaster()) {
        /* Set the SV Measurement Constellation when Measurement Report or Polynomial report is set */
        /* Check if either measurement report or sv polynomial report bit is different in the new
           mask compared to the old mask. If yes then turn that report on or off as requested */
        locClientEventMaskType measOrSvPoly = QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02 |
                                              QMI_LOC_EVENT_MASK_GNSS_SV_POLYNOMIAL_REPORT_V02;
        LOC_LOGd("clientHandle = %p isMaster(): %d measOrSvPoly: 0x%" PRIx64 \
                 " maskDiff: 0x%" PRIx64 "",
                 clientHandle, isMaster(), measOrSvPoly, maskDiff);
        if (((maskDiff & measOrSvPoly) != 0)) {
          setSvMeasurementConstellation(qmiMask);
        }
      }
    }
  }

  LOC_LOGd("clientHandle = %p Exit mMask: 0x%" PRIx64 " mQmiMask: 0x%" PRIx64 "",
           clientHandle, mMask, mQmiMask);

  return rtv;
}

bool LocApiV02 :: registerEventMask(locClientEventMaskType qmiMask)
{
    // if NOT in session and NOT the Background Loc Client, adjust the mask
    if (!mInSession && (LocDualContext::mBgExclMask & mMask)) {
        qmiMask = adjustMaskForNoSession(qmiMask);
    }
    LOC_LOGd("mQmiMask=0x%" PRIx64 " qmiMask=0x%" PRIx64 "", mQmiMask, qmiMask);
    return locClientRegisterEventMask(clientHandle, qmiMask, isMaster());
}

locClientEventMaskType LocApiV02 :: adjustMaskForNoSession(locClientEventMaskType qmiMask)
{
    LOC_LOGd("before qmiMask=0x%" PRIx64 "",qmiMask);
    locClientEventMaskType clearMask = QMI_LOC_EVENT_MASK_POSITION_REPORT_V02 |
                                       QMI_LOC_EVENT_MASK_UNPROPAGATED_POSITION_REPORT_V02 |
                                       QMI_LOC_EVENT_MASK_GNSS_SV_INFO_V02 |
                                       QMI_LOC_EVENT_MASK_NMEA_V02 |
                                       QMI_LOC_EVENT_MASK_ENGINE_STATE_V02 |
                                       QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02 |
                                       QMI_LOC_EVENT_MASK_GNSS_SV_POLYNOMIAL_REPORT_V02;

    qmiMask = qmiMask & ~clearMask;
    LOC_LOGd("after qmiMask=0x%" PRIx64 "", qmiMask);
    return qmiMask;
}

enum loc_api_adapter_err LocApiV02 :: close()
{
  enum loc_api_adapter_err rtv =
      // success if either client is already invalid, or
      // we successfully close the handle
      (LOC_CLIENT_INVALID_HANDLE_VALUE == clientHandle ||
       eLOC_CLIENT_SUCCESS == locClientClose(&clientHandle)) ?
      LOC_API_ADAPTER_ERR_SUCCESS : LOC_API_ADAPTER_ERR_FAILURE;

  mMask = 0;
  clientHandle = LOC_CLIENT_INVALID_HANDLE_VALUE;
  mIsMasterRegistered = false;

  return rtv;
}

/* start positioning session */
void LocApiV02 :: startFix(const LocPosMode& fixCriteria, LocApiResponse *adapterResponse)
{
  sendMsg(new LocApiMsg([this, fixCriteria, adapterResponse] () {

  locClientStatusEnumType status;
  locClientReqUnionType req_union;

  qmiLocStartReqMsgT_v02 start_msg;

  qmiLocSetOperationModeReqMsgT_v02 set_mode_msg;
  qmiLocSetOperationModeIndMsgT_v02 set_mode_ind;

    // clear all fields, validity masks
  memset (&start_msg, 0, sizeof(start_msg));
  memset (&set_mode_msg, 0, sizeof(set_mode_msg));
  memset (&set_mode_ind, 0, sizeof(set_mode_ind));

  LOC_LOGV("%s:%d]: start \n", __func__, __LINE__);
  fixCriteria.logv();

  mInSession = true;
  mMeasurementsStarted = true;
  registerEventMask(mQmiMask);

  // fill in the start request
  switch(fixCriteria.mode)
  {
    case LOC_POSITION_MODE_MS_BASED:
      set_mode_msg.operationMode = eQMI_LOC_OPER_MODE_MSB_V02;
      break;

    case LOC_POSITION_MODE_MS_ASSISTED:
      set_mode_msg.operationMode = eQMI_LOC_OPER_MODE_MSA_V02;
      break;

    case LOC_POSITION_MODE_RESERVED_4:
      set_mode_msg.operationMode = eQMI_LOC_OPER_MODE_CELL_ID_V02;
        break;

    case LOC_POSITION_MODE_RESERVED_5:
      set_mode_msg.operationMode = eQMI_LOC_OPER_MODE_WWAN_V02;
        break;

    default:
      set_mode_msg.operationMode = eQMI_LOC_OPER_MODE_STANDALONE_V02;
      break;
  }

  req_union.pSetOperationModeReq = &set_mode_msg;

  // send the mode first, before the start message.
  status = locSyncSendReq(QMI_LOC_SET_OPERATION_MODE_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_OPERATION_MODE_IND_V02,
                          &set_mode_ind); // NULL?
   //When locSyncSendReq status is time out, more likely the response was lost.
   //startFix will continue as though it is succeeded.
  if ((status != eLOC_CLIENT_SUCCESS && status != eLOC_CLIENT_FAILURE_TIMEOUT) ||
       eQMI_LOC_SUCCESS_V02 != set_mode_ind.status)
  {
    LOC_LOGE ("%s:%d]: set opertion mode failed status = %s, "
                   "ind..status = %s\n", __func__, __LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(set_mode_ind.status));
  } else {
      if (status == eLOC_CLIENT_FAILURE_TIMEOUT)
      {
          LOC_LOGE ("%s:%d]: set operation mode timed out\n", __func__, __LINE__);
      }
      start_msg.minInterval_valid = 1;
      start_msg.minInterval = fixCriteria.min_interval;

      start_msg.horizontalAccuracyLevel_valid = 1;

      if (fixCriteria.preferred_accuracy <= 100)
      {
          // fix needs high accuracy
          start_msg.horizontalAccuracyLevel =  eQMI_LOC_ACCURACY_HIGH_V02;
      }
      else if (fixCriteria.preferred_accuracy <= 1000)
      {
          //fix needs med accuracy
          start_msg.horizontalAccuracyLevel =  eQMI_LOC_ACCURACY_MED_V02;
      }
      else
      {
          //fix needs low accuracy
          start_msg.horizontalAccuracyLevel =  eQMI_LOC_ACCURACY_LOW_V02;
          // limit the scanning max time to 1 min and TBF to 10 min
          // this is to control the power cost for gps for LOW accuracy
          start_msg.positionReportTimeout_valid = 1;
          start_msg.positionReportTimeout = 60000;
          if (start_msg.minInterval < 600000) {
              start_msg.minInterval = 600000;
          }
      }

      start_msg.fixRecurrence_valid = 1;
      if(LOC_GPS_POSITION_RECURRENCE_SINGLE == fixCriteria.recurrence)
      {
          start_msg.fixRecurrence = eQMI_LOC_RECURRENCE_SINGLE_V02;
      }
      else
      {
          start_msg.fixRecurrence = eQMI_LOC_RECURRENCE_PERIODIC_V02;
      }

      //dummy session id
      // TBD: store session ID, check for session id in pos reports.
      start_msg.sessionId = LOC_API_V02_DEF_SESSION_ID;

      //Set whether position report can be shared with other LOC clients
      start_msg.sharePosition_valid = 1;
      start_msg.sharePosition = fixCriteria.share_position;

      if (fixCriteria.credentials[0] != 0) {
          int size1 = sizeof(start_msg.applicationId.applicationName);
          int size2 = sizeof(fixCriteria.credentials);
          int len = ((size1 < size2) ? size1 : size2) - 1;
          memcpy(start_msg.applicationId.applicationName,
                 fixCriteria.credentials,
                 len);

          size1 = sizeof(start_msg.applicationId.applicationProvider);
          size2 = sizeof(fixCriteria.provider);
          len = ((size1 < size2) ? size1 : size2) - 1;
          memcpy(start_msg.applicationId.applicationProvider,
                 fixCriteria.provider,
                 len);

          start_msg.applicationId_valid = 1;
      }

      // config Altitude Assumed
      start_msg.configAltitudeAssumed_valid = 1;
      start_msg.configAltitudeAssumed = eQMI_LOC_ALTITUDE_ASSUMED_IN_GNSS_SV_INFO_DISABLED_V02;

      // set power mode details
      if (GNSS_POWER_MODE_INVALID != fixCriteria.powerMode) {
          start_msg.powerMode_valid = 1;
          start_msg.powerMode.powerMode = convertPowerMode(fixCriteria.powerMode);
          start_msg.powerMode.timeBetweenMeasurement =
                  fixCriteria.timeBetweenMeasurements;
          // Force low accuracy for background power modes
          if (GNSS_POWER_MODE_M3 == fixCriteria.powerMode ||
                  GNSS_POWER_MODE_M4 == fixCriteria.powerMode ||
                  GNSS_POWER_MODE_M5 == fixCriteria.powerMode) {
              start_msg.horizontalAccuracyLevel =  eQMI_LOC_ACCURACY_LOW_V02;
          }
      }

      req_union.pStartReq = &start_msg;

      status = locClientSendReq(QMI_LOC_START_REQ_V02, req_union);
  }

  LocationError err = LOCATION_ERROR_GENERAL_FAILURE;
  if (eLOC_CLIENT_SUCCESS == status) {
      err = LOCATION_ERROR_SUCCESS;
  }

  if (adapterResponse != NULL) {
      adapterResponse->returnToSender(err);
  }
  }));

}

/* stop a positioning session */
void LocApiV02 :: stopFix(LocApiResponse *adapterResponse)
{
  sendMsg(new LocApiMsg([this, adapterResponse] () {

  locClientStatusEnumType status;
  locClientReqUnionType req_union;

  qmiLocStopReqMsgT_v02 stop_msg;

  LOC_LOGD(" %s:%d]: stop called \n", __func__, __LINE__);

  memset(&stop_msg, 0, sizeof(stop_msg));

  // dummy session id
  stop_msg.sessionId = LOC_API_V02_DEF_SESSION_ID;

  req_union.pStopReq = &stop_msg;

  status = locClientSendReq(QMI_LOC_STOP_REQ_V02, req_union);

  mInSession = false;
  // if engine on never happend, deregister events
  // without waiting for Engine Off
  if (!mEngineOn) {
      registerEventMask(mQmiMask);
  }

  if( eLOC_CLIENT_SUCCESS != status)
  {
      LOC_LOGE("%s:%d]: error = %s\n",__func__, __LINE__,
               loc_get_v02_client_status_name(status));
  }

  LocationError err = LOCATION_ERROR_GENERAL_FAILURE;
  if (eLOC_CLIENT_SUCCESS == status) {
      err = LOCATION_ERROR_SUCCESS;
  }

  adapterResponse->returnToSender(err);
  }));
}

/* set the positioning fix criteria */
void LocApiV02 :: setPositionMode(
  const LocPosMode& posMode)
{
    if(isInSession())
    {
        //fix is in progress, send a restart
        LOC_LOGD ("%s:%d]: fix is in progress restarting the fix with new "
                  "criteria\n", __func__, __LINE__);

        startFix(posMode, NULL);
    }
}

/* inject time into the position engine */
void LocApiV02 ::
    setTime(LocGpsUtcTime time, int64_t timeReference, int uncertainty)
{
  sendMsg(new LocApiMsg([this, time, timeReference, uncertainty] () {

  locClientReqUnionType req_union;
  locClientStatusEnumType status;
  qmiLocInjectUtcTimeReqMsgT_v02  inject_time_msg;
  qmiLocInjectUtcTimeIndMsgT_v02 inject_time_ind;

  memset(&inject_time_msg, 0, sizeof(inject_time_msg));

  inject_time_ind.status = eQMI_LOC_GENERAL_FAILURE_V02;

  inject_time_msg.timeUtc = time;

  inject_time_msg.timeUtc += (int64_t)(uptimeMillis() - timeReference);

  inject_time_msg.timeUnc = uncertainty;

  req_union.pInjectUtcTimeReq = &inject_time_msg;

  LOC_LOGV ("%s:%d]: uncertainty = %d\n", __func__, __LINE__,
                 uncertainty);

  status = locSyncSendReq(QMI_LOC_INJECT_UTC_TIME_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_INJECT_UTC_TIME_IND_V02,
                          &inject_time_ind);

  if (status != eLOC_CLIENT_SUCCESS ||
      eQMI_LOC_SUCCESS_V02 != inject_time_ind.status)
  {
    LOC_LOGE ("%s:%d] status = %s, ind..status = %s\n", __func__,  __LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(inject_time_ind.status));
  }

  }));
}

/* inject position into the position engine */
void LocApiV02 ::
    injectPosition(double latitude, double longitude, float accuracy)
{
    Location location = {};

    location.flags |= LOCATION_HAS_LAT_LONG_BIT;
    location.latitude = latitude;
    location.longitude = longitude;

    location.flags |= LOCATION_HAS_ACCURACY_BIT;
    location.accuracy = accuracy;

    struct timespec time_info_current;
    if(clock_gettime(CLOCK_REALTIME,&time_info_current) == 0) //success
    {
        location.timestamp = (time_info_current.tv_sec)*1e3 +
            (time_info_current.tv_nsec)/1e6;
    }

    injectPosition(location, false);
}

void LocApiV02::injectPosition(const Location& location, bool onDemandCpi)
{
    sendMsg(new LocApiMsg([this, location, onDemandCpi] () {

    qmiLocInjectPositionReqMsgT_v02 injectPositionReq;
    memset(&injectPositionReq, 0, sizeof(injectPositionReq));

    if (location.timestamp > 0) {
        injectPositionReq.timestampUtc_valid = 1;
        injectPositionReq.timestampUtc = location.timestamp;
    }

    if (LOCATION_HAS_LAT_LONG_BIT & location.flags) {
        injectPositionReq.latitude_valid = 1;
        injectPositionReq.longitude_valid = 1;
        injectPositionReq.latitude = location.latitude;
        injectPositionReq.longitude = location.longitude;
    }

    if (LOCATION_HAS_ACCURACY_BIT & location.flags) {
        injectPositionReq.horUncCircular_valid = 1;
        injectPositionReq.horUncCircular = location.accuracy;
        injectPositionReq.horConfidence_valid = 1;
        injectPositionReq.horConfidence = 68;
        injectPositionReq.rawHorUncCircular_valid = 1;
        injectPositionReq.rawHorUncCircular = location.accuracy;
        injectPositionReq.rawHorConfidence_valid = 1;
        injectPositionReq.rawHorConfidence = 68;

        // We don't wish to advertise accuracy better than 1000 meters to Modem
        if (injectPositionReq.horUncCircular < 1000) {
            injectPositionReq.horUncCircular = 1000;
        }
    }

    if (LOCATION_HAS_ALTITUDE_BIT & location.flags) {
        injectPositionReq.altitudeWrtEllipsoid_valid = 1;
        injectPositionReq.altitudeWrtEllipsoid = location.altitude;
    }

    if (LOCATION_HAS_VERTICAL_ACCURACY_BIT & location.flags) {
        injectPositionReq.vertUnc_valid = 1;
        injectPositionReq.vertUnc = location.verticalAccuracy;
        injectPositionReq.vertConfidence_valid = 1;
        injectPositionReq.vertConfidence = 68;
    }

    if (onDemandCpi) {
        injectPositionReq.onDemandCpi_valid = 1;
        injectPositionReq.onDemandCpi = 1;
    }

    LOC_LOGv("Lat=%lf, Lon=%lf, Acc=%.2lf rawAcc=%.2lf horConfidence=%d"
             "rawHorConfidence=%d onDemandCpi=%d",
             injectPositionReq.latitude, injectPositionReq.longitude,
             injectPositionReq.horUncCircular, injectPositionReq.rawHorUncCircular,
             injectPositionReq.horConfidence, injectPositionReq.rawHorConfidence,
             injectPositionReq.onDemandCpi);

    LOC_SEND_SYNC_REQ(InjectPosition, INJECT_POSITION, injectPositionReq);

    }));
}

/* delete assistance date */
void
LocApiV02::deleteAidingData(const GnssAidingData& data, LocApiResponse *adapterResponse)
{
  sendMsg(new LocApiMsg([this, data, adapterResponse] () {

  static bool isNewApiSupported = true;
  locClientReqUnionType req_union;
  locClientStatusEnumType status = eLOC_CLIENT_FAILURE_UNSUPPORTED;
  LocationError err = LOCATION_ERROR_SUCCESS;

  // Use the new API first
  qmiLocDeleteGNSSServiceDataReqMsgT_v02 delete_gnss_req;
  qmiLocDeleteGNSSServiceDataIndMsgT_v02 delete_gnss_resp;

  memset(&delete_gnss_req, 0, sizeof(delete_gnss_req));
  memset(&delete_gnss_resp, 0, sizeof(delete_gnss_resp));

  if (isNewApiSupported) {
      if (data.deleteAll) {
          delete_gnss_req.deleteAllFlag = true;
      } else {
          if (GNSS_AIDING_DATA_SV_EPHEMERIS_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_EPHEMERIS_V02;
          }
          if (GNSS_AIDING_DATA_SV_ALMANAC_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_ALMANAC_V02;
          }
          if (GNSS_AIDING_DATA_SV_HEALTH_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_SVHEALTH_V02;
          }
          if (GNSS_AIDING_DATA_SV_DIRECTION_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_SVDIR_V02;
          }
          if (GNSS_AIDING_DATA_SV_STEER_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_SVSTEER_V02;
          }
          if (GNSS_AIDING_DATA_SV_ALMANAC_CORR_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_ALM_CORR_V02;
          }
          if (GNSS_AIDING_DATA_SV_BLACKLIST_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_BLACKLIST_V02;
          }
          if (GNSS_AIDING_DATA_SV_SA_DATA_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_SA_DATA_V02;
          }
          if (GNSS_AIDING_DATA_SV_NO_EXIST_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_SV_NO_EXIST_V02;
          }
          if (GNSS_AIDING_DATA_SV_IONOSPHERE_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_IONO_V02;
          }
          if (GNSS_AIDING_DATA_SV_TIME_BIT & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_TIME_V02;
          }
          if (GNSS_AIDING_DATA_SV_MB_DATA & data.sv.svMask) {
              delete_gnss_req.deleteSatelliteData_valid = 1;
              delete_gnss_req.deleteSatelliteData.deleteSatelliteDataMask |=
                  QMI_LOC_DELETE_DATA_MASK_MB_DATA_V02;
          }
          if (delete_gnss_req.deleteSatelliteData_valid) {
              if (GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask) {
                  delete_gnss_req.deleteSatelliteData.system |= QMI_LOC_SYSTEM_GPS_V02;
              }
              if (GNSS_AIDING_DATA_SV_TYPE_GLONASS_BIT & data.sv.svTypeMask) {
                  delete_gnss_req.deleteSatelliteData.system |= QMI_LOC_SYSTEM_GLO_V02;
              }
              if (GNSS_AIDING_DATA_SV_TYPE_QZSS_BIT & data.sv.svTypeMask) {
                  delete_gnss_req.deleteSatelliteData.system |= QMI_LOC_SYSTEM_BDS_V02;
              }
              if (GNSS_AIDING_DATA_SV_TYPE_BEIDOU_BIT & data.sv.svTypeMask) {
                  delete_gnss_req.deleteSatelliteData.system |= QMI_LOC_SYSTEM_GAL_V02;
              }
              if (GNSS_AIDING_DATA_SV_TYPE_GALILEO_BIT & data.sv.svTypeMask) {
                  delete_gnss_req.deleteSatelliteData.system |= QMI_LOC_SYSTEM_QZSS_V02;
              }
          }

          if (GNSS_AIDING_DATA_COMMON_POSITION_BIT & data.common.mask) {
              delete_gnss_req.deleteCommonDataMask_valid = 1;
              delete_gnss_req.deleteCommonDataMask |= QMI_LOC_DELETE_COMMON_MASK_POS_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_TIME_BIT & data.common.mask) {
              delete_gnss_req.deleteCommonDataMask_valid = 1;
              delete_gnss_req.deleteCommonDataMask |= QMI_LOC_DELETE_COMMON_MASK_TIME_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_UTC_BIT & data.common.mask) {
              delete_gnss_req.deleteCommonDataMask_valid = 1;
              delete_gnss_req.deleteCommonDataMask |= QMI_LOC_DELETE_COMMON_MASK_UTC_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_RTI_BIT & data.common.mask) {
              delete_gnss_req.deleteCommonDataMask_valid = 1;
              delete_gnss_req.deleteCommonDataMask |= QMI_LOC_DELETE_COMMON_MASK_RTI_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_FREQ_BIAS_EST_BIT & data.common.mask) {
              delete_gnss_req.deleteCommonDataMask_valid = 1;
              delete_gnss_req.deleteCommonDataMask |= QMI_LOC_DELETE_COMMON_MASK_FREQ_BIAS_EST_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_CELLDB_BIT & data.common.mask) {
              delete_gnss_req.deleteCellDbDataMask_valid = 1;
              delete_gnss_req.deleteCellDbDataMask =
                  (QMI_LOC_MASK_DELETE_CELLDB_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_LATEST_GPS_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_OTA_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_EXT_REF_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_TIMETAG_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CELLID_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CACHED_CELLID_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_LAST_SRV_CELL_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CUR_SRV_CELL_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_NEIGHBOR_INFO_V02);
          }
      }

      req_union.pDeleteGNSSServiceDataReq = &delete_gnss_req;

      status = locSyncSendReq(QMI_LOC_DELETE_GNSS_SERVICE_DATA_REQ_V02,
                              req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                              QMI_LOC_DELETE_GNSS_SERVICE_DATA_IND_V02,
                              &delete_gnss_resp);

      if (status != eLOC_CLIENT_SUCCESS ||
          eQMI_LOC_SUCCESS_V02 != delete_gnss_resp.status)
      {
          LOC_LOGE("%s:%d]: error! status = %s, delete_resp.status = %s\n",
              __func__, __LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(delete_gnss_resp.status));
      }
  }

  if (eLOC_CLIENT_FAILURE_UNSUPPORTED == status ||
      eLOC_CLIENT_FAILURE_INTERNAL == status) {
      // If the new API is not supported we fall back on the old one
      // The error could be eLOC_CLIENT_FAILURE_INTERNAL if
      // QMI_LOC_DELETE_GNSS_SERVICE_DATA_REQ_V02 is not in the .idl file
      LOC_LOGD("%s:%d]: QMI_LOC_DELETE_GNSS_SERVICE_DATA_REQ_V02 not supported"
          "We use QMI_LOC_DELETE_ASSIST_DATA_REQ_V02\n",
          __func__, __LINE__);
      isNewApiSupported = false;

      qmiLocDeleteAssistDataReqMsgT_v02 delete_req;
      qmiLocDeleteAssistDataIndMsgT_v02 delete_resp;

      memset(&delete_req, 0, sizeof(delete_req));
      memset(&delete_resp, 0, sizeof(delete_resp));

      if (data.deleteAll) {
          delete_req.deleteAllFlag = true;
      } else {
          /* to keep track of svInfoList for GPS and GLO*/
          uint32_t curr_sv_len = 0;
          uint32_t curr_sv_idx = 0;
          uint32_t sv_id = 0;

          if ((GNSS_AIDING_DATA_SV_EPHEMERIS_BIT & data.sv.svMask ||
              GNSS_AIDING_DATA_SV_ALMANAC_BIT & data.sv.svMask) &&
              GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask) {

              /* do delete for all GPS SV's */
              curr_sv_len += SV_ID_RANGE;

              sv_id = GPS_SV_ID_OFFSET;

              delete_req.deleteSvInfoList_valid = 1;

              delete_req.deleteSvInfoList_len = curr_sv_len;

              LOC_LOGV("%s:%d]: Delete GPS SV info for index %d to %d"
                  "and sv id %d to %d \n",
                  __func__, __LINE__, curr_sv_idx, curr_sv_len - 1,
                  sv_id, sv_id + SV_ID_RANGE - 1);

              for (uint32_t i = curr_sv_idx; i < curr_sv_len; i++, sv_id++) {
                  delete_req.deleteSvInfoList[i].gnssSvId = sv_id;

                  delete_req.deleteSvInfoList[i].system = eQMI_LOC_SV_SYSTEM_GPS_V02;

                  if (GNSS_AIDING_DATA_SV_EPHEMERIS_BIT & data.sv.svMask) {
                      // set ephemeris mask for all GPS SV's
                      delete_req.deleteSvInfoList[i].deleteSvInfoMask |=
                          QMI_LOC_MASK_DELETE_EPHEMERIS_V02;
                  }

                  if (GNSS_AIDING_DATA_SV_ALMANAC_BIT & data.sv.svMask) {
                      delete_req.deleteSvInfoList[i].deleteSvInfoMask |=
                          QMI_LOC_MASK_DELETE_ALMANAC_V02;
                  }
              }
              // increment the current index
              curr_sv_idx += SV_ID_RANGE;

          }

          if (GNSS_AIDING_DATA_COMMON_POSITION_BIT & data.common.mask) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_POSITION_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_TIME_BIT & data.common.mask) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_TIME_V02;
          }
          if ((GNSS_AIDING_DATA_SV_IONOSPHERE_BIT & data.sv.svMask) &&
              (GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask)) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_IONO_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_UTC_BIT & data.common.mask)
          {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_UTC_V02;
          }
          if ((GNSS_AIDING_DATA_SV_HEALTH_BIT & data.sv.svMask) &&
              (GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask)) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_HEALTH_V02;
          }
          if ((GNSS_AIDING_DATA_SV_DIRECTION_BIT & data.sv.svMask) &&
              (GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask)) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_GPS_SVDIR_V02;
          }
          if ((GNSS_AIDING_DATA_SV_SA_DATA_BIT & data.sv.svMask) &&
              (GNSS_AIDING_DATA_SV_TYPE_GPS_BIT & data.sv.svTypeMask)) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_SADATA_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_RTI_BIT & data.common.mask) {
              delete_req.deleteGnssDataMask_valid = 1;
              delete_req.deleteGnssDataMask |= QMI_LOC_MASK_DELETE_RTI_V02;
          }
          if (GNSS_AIDING_DATA_COMMON_CELLDB_BIT & data.common.mask) {
              delete_req.deleteCellDbDataMask_valid = 1;
              delete_req.deleteCellDbDataMask =
                  (QMI_LOC_MASK_DELETE_CELLDB_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_LATEST_GPS_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_OTA_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_EXT_REF_POS_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_TIMETAG_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CELLID_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CACHED_CELLID_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_LAST_SRV_CELL_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_CUR_SRV_CELL_V02 |
                      QMI_LOC_MASK_DELETE_CELLDB_NEIGHBOR_INFO_V02);

          }
      }

      req_union.pDeleteAssistDataReq = &delete_req;

      status = locSyncSendReq(QMI_LOC_DELETE_ASSIST_DATA_REQ_V02,
                              req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                              QMI_LOC_DELETE_ASSIST_DATA_IND_V02,
                              &delete_resp);

      if (status != eLOC_CLIENT_SUCCESS ||
          eQMI_LOC_SUCCESS_V02 != delete_resp.status)
      {
          LOC_LOGE("%s:%d]: error! status = %s, delete_resp.status = %s\n",
              __func__, __LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(delete_resp.status));
          err = LOCATION_ERROR_GENERAL_FAILURE;
      }
  }

  adapterResponse->returnToSender(err);
  }));
}

/* send NI user repsonse to the engine */
void
LocApiV02::informNiResponse(GnssNiResponse userResponse, const void* passThroughData)
{
  sendMsg(new LocApiMsg([this, userResponse, passThroughData] () {

  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientReqUnionType req_union;
  locClientStatusEnumType status;
  qmiLocNiUserRespReqMsgT_v02 ni_resp;
  qmiLocNiUserRespIndMsgT_v02 ni_resp_ind;

  qmiLocEventNiNotifyVerifyReqIndMsgT_v02 *request_pass_back =
    (qmiLocEventNiNotifyVerifyReqIndMsgT_v02 *)passThroughData;

  memset(&ni_resp,0, sizeof(ni_resp));

  memset(&ni_resp_ind,0, sizeof(ni_resp_ind));

  switch (userResponse)
  {
    case GNSS_NI_RESPONSE_ACCEPT:
      ni_resp.userResp = eQMI_LOC_NI_LCS_NOTIFY_VERIFY_ACCEPT_V02;
      break;
   case GNSS_NI_RESPONSE_DENY:
      ni_resp.userResp = eQMI_LOC_NI_LCS_NOTIFY_VERIFY_DENY_V02;
      break;
   case GNSS_NI_RESPONSE_NO_RESPONSE:
      ni_resp.userResp = eQMI_LOC_NI_LCS_NOTIFY_VERIFY_NORESP_V02;
      break;
   default:
      err = LOCATION_ERROR_INVALID_PARAMETER;
      free((void *)passThroughData);
      return;
  }

  LOC_LOGV(" %s:%d]: NI response: %d\n", __func__, __LINE__,
                ni_resp.userResp);

  ni_resp.notificationType = request_pass_back->notificationType;

  // copy SUPL payload from request
  if(request_pass_back->NiSuplInd_valid == 1)
  {
     ni_resp.NiSuplPayload_valid = 1;
     memcpy(&(ni_resp.NiSuplPayload), &(request_pass_back->NiSuplInd),
            sizeof(qmiLocNiSuplNotifyVerifyStructT_v02));

  }
  // should this be an "else if"?? we don't need to decide

  // copy UMTS-CP payload from request
  if( request_pass_back->NiUmtsCpInd_valid == 1 )
  {
     ni_resp.NiUmtsCpPayload_valid = 1;
     memcpy(&(ni_resp.NiUmtsCpPayload), &(request_pass_back->NiUmtsCpInd),
            sizeof(qmiLocNiUmtsCpNotifyVerifyStructT_v02));
  }

  //copy Vx payload from the request
  if( request_pass_back->NiVxInd_valid == 1)
  {
     ni_resp.NiVxPayload_valid = 1;
     memcpy(&(ni_resp.NiVxPayload), &(request_pass_back->NiVxInd),
            sizeof(qmiLocNiVxNotifyVerifyStructT_v02));
  }

  // copy Vx service interaction payload from the request
  if(request_pass_back->NiVxServiceInteractionInd_valid == 1)
  {
     ni_resp.NiVxServiceInteractionPayload_valid = 1;
     memcpy(&(ni_resp.NiVxServiceInteractionPayload),
            &(request_pass_back->NiVxServiceInteractionInd),
            sizeof(qmiLocNiVxServiceInteractionStructT_v02));
  }

  // copy Network Initiated SUPL Version 2 Extension
  if (request_pass_back->NiSuplVer2ExtInd_valid == 1)
  {
     ni_resp.NiSuplVer2ExtPayload_valid = 1;
     memcpy(&(ni_resp.NiSuplVer2ExtPayload),
            &(request_pass_back->NiSuplVer2ExtInd),
            sizeof(qmiLocNiSuplVer2ExtStructT_v02));
  }

  // copy SUPL Emergency Notification
  if(request_pass_back->suplEmergencyNotification_valid)
  {
     ni_resp.suplEmergencyNotification_valid = 1;
     memcpy(&(ni_resp.suplEmergencyNotification),
            &(request_pass_back->suplEmergencyNotification),
            sizeof(qmiLocEmergencyNotificationStructT_v02));
  }

  req_union.pNiUserRespReq = &ni_resp;

  status = locSyncSendReq(QMI_LOC_NI_USER_RESPONSE_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_NI_USER_RESPONSE_IND_V02,
                          &ni_resp_ind);

  if (status != eLOC_CLIENT_SUCCESS ||
      eQMI_LOC_SUCCESS_V02 != ni_resp_ind.status)
  {
    LOC_LOGE ("%s:%d]: error! status = %s, ni_resp_ind.status = %s\n",
              __func__, __LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(ni_resp_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  free((void *)passThroughData);
  }));
}

void
LocApiV02::registerMasterClient()
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientReqUnionType req_union;
  locClientStatusEnumType status;
  qmiLocRegisterMasterClientReqMsgT_v02 reg_master_client_req;
  qmiLocRegisterMasterClientIndMsgT_v02 reg_master_client_ind;

  memset(&reg_master_client_req, 0, sizeof(reg_master_client_req));
  memset(&reg_master_client_ind, 0, sizeof(reg_master_client_ind));

  reg_master_client_req.key = 0xBAABCDEF;

  req_union.pRegisterMasterClientReq = &reg_master_client_req;

  status = locSyncSendReq(QMI_LOC_REGISTER_MASTER_CLIENT_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_REGISTER_MASTER_CLIENT_IND_V02,
                          &reg_master_client_ind);

  if (eLOC_CLIENT_SUCCESS != status ||
         eQMI_LOC_SUCCESS_V02 != reg_master_client_ind.status) {
    LOC_LOGe ("error status = %s, reg_master_client_ind.status = %s",
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_reg_mk_status_name(reg_master_client_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }
}

/* Set UMTs SLP server URL */
LocationError
LocApiV02::setServerSync(const char* url, int len, LocServerType type)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientReqUnionType req_union;
  locClientStatusEnumType status;
  qmiLocSetServerReqMsgT_v02 set_server_req;
  qmiLocSetServerIndMsgT_v02 set_server_ind;

  if(len < 0 || (size_t)len > sizeof(set_server_req.urlAddr))
  {
    LOC_LOGe("len = %d greater than max allowed url length", len);

    return LOCATION_ERROR_INVALID_PARAMETER;
  }

  memset(&set_server_req, 0, sizeof(set_server_req));
  memset(&set_server_ind, 0, sizeof(set_server_ind));

  LOC_LOGd("url = %s, len = %d type=%d", url, len, type);

  if (LOC_AGPS_MO_SUPL_SERVER == type) {
      set_server_req.serverType = eQMI_LOC_SERVER_TYPE_CUSTOM_SLP_V02;
  } else {
      set_server_req.serverType = eQMI_LOC_SERVER_TYPE_UMTS_SLP_V02;
  }

  set_server_req.urlAddr_valid = 1;

  strlcpy(set_server_req.urlAddr, url, sizeof(set_server_req.urlAddr));

  req_union.pSetServerReq = &set_server_req;

  status = locSyncSendReq(QMI_LOC_SET_SERVER_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_SERVER_IND_V02,
                          &set_server_ind);

  if (status != eLOC_CLIENT_SUCCESS ||
         eQMI_LOC_SUCCESS_V02 != set_server_ind.status)
  {
    LOC_LOGe ("error status = %s, set_server_ind.status = %s",
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(set_server_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

LocationError
LocApiV02::setServerSync(unsigned int ip, int port, LocServerType type)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientReqUnionType req_union;
  locClientStatusEnumType status;
  qmiLocSetServerReqMsgT_v02 set_server_req;
  qmiLocSetServerIndMsgT_v02 set_server_ind;
  qmiLocServerTypeEnumT_v02 set_server_cmd;

  switch (type) {
  case LOC_AGPS_MPC_SERVER:
      set_server_cmd = eQMI_LOC_SERVER_TYPE_CDMA_MPC_V02;
      break;
  case LOC_AGPS_CUSTOM_PDE_SERVER:
      set_server_cmd = eQMI_LOC_SERVER_TYPE_CUSTOM_PDE_V02;
      break;
  default:
      set_server_cmd = eQMI_LOC_SERVER_TYPE_CDMA_PDE_V02;
      break;
  }

  memset(&set_server_req, 0, sizeof(set_server_req));
  memset(&set_server_ind, 0, sizeof(set_server_ind));

  LOC_LOGD("%s:%d]:, ip = %u, port = %d\n", __func__, __LINE__, ip, port);

  set_server_req.serverType = set_server_cmd;
  set_server_req.ipv4Addr_valid = 1;
  set_server_req.ipv4Addr.addr = ip;
  set_server_req.ipv4Addr.port = port;

  req_union.pSetServerReq = &set_server_req;

  status = locSyncSendReq(QMI_LOC_SET_SERVER_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_SERVER_IND_V02,
                          &set_server_ind);

  if (status != eLOC_CLIENT_SUCCESS ||
      eQMI_LOC_SUCCESS_V02 != set_server_ind.status)
  {
    LOC_LOGE ("%s:%d]: error status = %s, set_server_ind.status = %s\n",
              __func__,__LINE__,
              loc_get_v02_client_status_name(status),
              loc_get_v02_qmi_status_name(set_server_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

/* Inject XTRA data, this module breaks down the XTRA
   file into "chunks" and injects them one at a time */
enum loc_api_adapter_err LocApiV02 :: setXtraData(
  char* data, int length)
{
  locClientStatusEnumType status = eLOC_CLIENT_SUCCESS;
  uint16_t  total_parts;
  uint16_t  part;
  uint32_t  len_injected;

  locClientReqUnionType req_union;
  qmiLocInjectPredictedOrbitsDataReqMsgT_v02 inject_xtra;
  qmiLocInjectPredictedOrbitsDataIndMsgT_v02 inject_xtra_ind;

  req_union.pInjectPredictedOrbitsDataReq = &inject_xtra;

  LOC_LOGD("%s:%d]: xtra size = %d\n", __func__, __LINE__, length);

  inject_xtra.formatType_valid = 1;
  inject_xtra.formatType = eQMI_LOC_PREDICTED_ORBITS_XTRA_V02;
  inject_xtra.totalSize = length;

  total_parts = ((length - 1) / QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02) + 1;

  inject_xtra.totalParts = total_parts;

  len_injected = 0; // O bytes injected

  // XTRA injection starts with part 1
  for (part = 1; part <= total_parts; part++)
  {
    inject_xtra.partNum = part;

    if (QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02 > (length - len_injected))
    {
      inject_xtra.partData_len = length - len_injected;
    }
    else
    {
      inject_xtra.partData_len = QMI_LOC_MAX_PREDICTED_ORBITS_PART_LEN_V02;
    }

    // copy data into the message
    memcpy(inject_xtra.partData, data+len_injected, inject_xtra.partData_len);

    LOC_LOGD("[%s:%d] part %d/%d, len = %d, total injected = %d\n",
                  __func__, __LINE__,
                  inject_xtra.partNum, total_parts, inject_xtra.partData_len,
                  len_injected);

    memset(&inject_xtra_ind, 0, sizeof(inject_xtra_ind));
    status = locSyncSendReq(QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_REQ_V02,
                            req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                            QMI_LOC_INJECT_PREDICTED_ORBITS_DATA_IND_V02,
                            &inject_xtra_ind);

    if (status != eLOC_CLIENT_SUCCESS ||
        eQMI_LOC_SUCCESS_V02 != inject_xtra_ind.status ||
        inject_xtra.partNum != inject_xtra_ind.partNum)
    {
      LOC_LOGE ("%s:%d]: failed status = %s, inject_pos_ind.status = %s,"
                     " part num = %d, ind.partNum = %d\n", __func__, __LINE__,
                loc_get_v02_client_status_name(status),
                loc_get_v02_qmi_status_name(inject_xtra_ind.status),
                inject_xtra.partNum, inject_xtra_ind.partNum);
    } else {
      len_injected += inject_xtra.partData_len;
      LOC_LOGD("%s:%d]: XTRA injected length: %d\n", __func__, __LINE__,
               len_injected);
    }
  }

  return convertErr(status);
}

/* Request the Xtra Server Url from the modem */
enum loc_api_adapter_err LocApiV02 :: requestXtraServer()
{
  locClientStatusEnumType status = eLOC_CLIENT_SUCCESS;

  locClientReqUnionType req_union;
  qmiLocGetPredictedOrbitsDataSourceIndMsgT_v02 request_xtra_server_ind;

  memset(&request_xtra_server_ind, 0, sizeof(request_xtra_server_ind));

  status = locSyncSendReq(QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_GET_PREDICTED_ORBITS_DATA_SOURCE_IND_V02,
                          &request_xtra_server_ind);

  if (status == eLOC_CLIENT_SUCCESS &&
      eQMI_LOC_SUCCESS_V02 == request_xtra_server_ind.status &&
      false != request_xtra_server_ind.serverList_valid &&
      0 != request_xtra_server_ind.serverList.serverList_len)
  {
    if (request_xtra_server_ind.serverList.serverList_len == 1)
    {
      reportXtraServer(request_xtra_server_ind.serverList.serverList[0].serverUrl,
                       "",
                       "",
                       QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
    }
    else if (request_xtra_server_ind.serverList.serverList_len == 2)
    {
      reportXtraServer(request_xtra_server_ind.serverList.serverList[0].serverUrl,
                       request_xtra_server_ind.serverList.serverList[1].serverUrl,
                       "",
                       QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
    }
    else
    {
      reportXtraServer(request_xtra_server_ind.serverList.serverList[0].serverUrl,
                       request_xtra_server_ind.serverList.serverList[1].serverUrl,
                       request_xtra_server_ind.serverList.serverList[2].serverUrl,
                       QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
    }
  }

  return convertErr(status);
}

void LocApiV02 :: atlOpenStatus(
  int handle, int is_succ, char* apn, uint32_t apnLen, AGpsBearerType bear,
  LocAGpsType /*agpsType*/, LocApnTypeMask apnTypeMask)
{
  sendMsg(new LocApiMsg([this, handle, is_succ,
                        apnStr=std::string(apn, apnLen),
                        bear, apnTypeMask] () {

  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocInformLocationServerConnStatusReqMsgT_v02 conn_status_req;
  qmiLocInformLocationServerConnStatusIndMsgT_v02 conn_status_ind;

  LOC_LOGd("ATL open handle = %d, is_succ = %d, APN = [%s], bearer = %d, "
           "apnTypeMask 0x%X" PRIx32, handle, is_succ, apnStr.c_str(),
           bear, apnTypeMask);

  memset(&conn_status_req, 0, sizeof(conn_status_req));
  memset(&conn_status_ind, 0, sizeof(conn_status_ind));

        // Fill in data
  conn_status_req.connHandle = handle;

  conn_status_req.requestType = eQMI_LOC_SERVER_REQUEST_OPEN_V02;

  if(is_succ)
  {
    conn_status_req.statusType = eQMI_LOC_SERVER_REQ_STATUS_SUCCESS_V02;

    if(!apnStr.empty()) {
        strlcpy(conn_status_req.apnProfile.apnName, apnStr.c_str(),
                sizeof(conn_status_req.apnProfile.apnName) );
    }

    switch(bear)
    {
    case AGPS_APN_BEARER_IPV4:
        conn_status_req.apnProfile.pdnType =
            eQMI_LOC_APN_PROFILE_PDN_TYPE_IPV4_V02;
        conn_status_req.apnProfile_valid = 1;
        break;

    case AGPS_APN_BEARER_IPV6:
        conn_status_req.apnProfile.pdnType =
            eQMI_LOC_APN_PROFILE_PDN_TYPE_IPV6_V02;
        conn_status_req.apnProfile_valid = 1;
        break;

    case AGPS_APN_BEARER_IPV4V6:
        conn_status_req.apnProfile.pdnType =
            eQMI_LOC_APN_PROFILE_PDN_TYPE_IPV4V6_V02;
        conn_status_req.apnProfile_valid = 1;
        break;

    case AGPS_APN_BEARER_INVALID:
        conn_status_req.apnProfile_valid = 0;
        break;

    default:
        LOC_LOGE("%s:%d]:invalid bearer type\n",__func__,__LINE__);
        conn_status_req.apnProfile_valid = 0;
        return;
    }

    // Populate apnTypeMask
    if (0 != apnTypeMask) {
        conn_status_req.apnTypeMask_valid = true;
        conn_status_req.apnTypeMask = convertLocApnTypeMask(apnTypeMask);
    }

  }
  else
  {
    conn_status_req.statusType = eQMI_LOC_SERVER_REQ_STATUS_FAILURE_V02;
  }

  req_union.pInformLocationServerConnStatusReq = &conn_status_req;

  result = locSyncSendReq(QMI_LOC_INFORM_LOCATION_SERVER_CONN_STATUS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_INFORM_LOCATION_SERVER_CONN_STATUS_IND_V02,
                          &conn_status_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != conn_status_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(conn_status_ind.status));
  }

  }));
}


/* close atl connection */
void LocApiV02 :: atlCloseStatus(int handle, int is_succ)
{
  sendMsg(new LocApiMsg([this, handle, is_succ] () {

  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocInformLocationServerConnStatusReqMsgT_v02 conn_status_req;
  qmiLocInformLocationServerConnStatusIndMsgT_v02 conn_status_ind;

  LOC_LOGD("%s:%d]: ATL close handle = %d, is_succ = %d\n",
                 __func__, __LINE__,  handle, is_succ);

  memset(&conn_status_req, 0, sizeof(conn_status_req));
  memset(&conn_status_ind, 0, sizeof(conn_status_ind));

        // Fill in data
  conn_status_req.connHandle = handle;

  conn_status_req.requestType = eQMI_LOC_SERVER_REQUEST_CLOSE_V02;

  if(is_succ)
  {
    conn_status_req.statusType = eQMI_LOC_SERVER_REQ_STATUS_SUCCESS_V02;
  }
  else
  {
    conn_status_req.statusType = eQMI_LOC_SERVER_REQ_STATUS_FAILURE_V02;
  }

  req_union.pInformLocationServerConnStatusReq = &conn_status_req;

  result = locSyncSendReq(QMI_LOC_INFORM_LOCATION_SERVER_CONN_STATUS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_INFORM_LOCATION_SERVER_CONN_STATUS_IND_V02,
                          &conn_status_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != conn_status_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(conn_status_ind.status));
  }
  }));
}

/* set the SUPL version */
LocationError
LocApiV02::setSUPLVersionSync(GnssConfigSuplVersion version)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;

  qmiLocSetProtocolConfigParametersReqMsgT_v02 supl_config_req;
  qmiLocSetProtocolConfigParametersIndMsgT_v02 supl_config_ind;

  LOC_LOGD("%s:%d]: supl version = %d\n",  __func__, __LINE__, version);


  memset(&supl_config_req, 0, sizeof(supl_config_req));
  memset(&supl_config_ind, 0, sizeof(supl_config_ind));

  supl_config_req.suplVersion_valid = 1;

  switch (version) {
    case GNSS_CONFIG_SUPL_VERSION_2_0_0:
      supl_config_req.suplVersion = eQMI_LOC_SUPL_VERSION_2_0_V02;
      break;
    case GNSS_CONFIG_SUPL_VERSION_2_0_2:
      supl_config_req.suplVersion = eQMI_LOC_SUPL_VERSION_2_0_2_V02;
      break;
    case GNSS_CONFIG_SUPL_VERSION_1_0_0:
    default:
      supl_config_req.suplVersion =  eQMI_LOC_SUPL_VERSION_1_0_V02;
      break;
  }

  req_union.pSetProtocolConfigParametersReq = &supl_config_req;

  result = locSyncSendReq(QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_IND_V02,
                          &supl_config_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != supl_config_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(supl_config_ind.status));
     err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

/* set the NMEA types mask */
enum loc_api_adapter_err LocApiV02 :: setNMEATypesSync(uint32_t typesMask)
{
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;

  qmiLocSetNmeaTypesReqMsgT_v02 setNmeaTypesReqMsg;
  qmiLocSetNmeaTypesIndMsgT_v02 setNmeaTypesIndMsg;

  LOC_LOGD(" %s:%d]: setNMEATypes, mask = %u\n", __func__, __LINE__,typesMask);

  memset(&setNmeaTypesReqMsg, 0, sizeof(setNmeaTypesReqMsg));
  memset(&setNmeaTypesIndMsg, 0, sizeof(setNmeaTypesIndMsg));

  setNmeaTypesReqMsg.nmeaSentenceType = typesMask;

  req_union.pSetNmeaTypesReq = &setNmeaTypesReqMsg;

  result = locSyncSendReq(QMI_LOC_SET_NMEA_TYPES_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_NMEA_TYPES_IND_V02,
                          &setNmeaTypesIndMsg);

  // if success
  if ( result != eLOC_CLIENT_SUCCESS )
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
                  __func__, __LINE__,
                  loc_get_v02_client_status_name(result),
                  loc_get_v02_qmi_status_name(setNmeaTypesIndMsg.status));
  }

  return convertErr(result);
}

/* set the configuration for LTE positioning profile (LPP) */
LocationError
LocApiV02::setLPPConfigSync(GnssConfigLppProfile profile)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocSetProtocolConfigParametersReqMsgT_v02 lpp_config_req;
  qmiLocSetProtocolConfigParametersIndMsgT_v02 lpp_config_ind;

  LOC_LOGD("%s:%d]: lpp profile = %u",  __func__, __LINE__, profile);

  memset(&lpp_config_req, 0, sizeof(lpp_config_req));
  memset(&lpp_config_ind, 0, sizeof(lpp_config_ind));

  lpp_config_req.lppConfig_valid = 1;
  switch (profile) {
    case GNSS_CONFIG_LPP_PROFILE_USER_PLANE:
      lpp_config_req.lppConfig = QMI_LOC_LPP_CONFIG_ENABLE_USER_PLANE_V02;
      break;
    case GNSS_CONFIG_LPP_PROFILE_CONTROL_PLANE:
      lpp_config_req.lppConfig = QMI_LOC_LPP_CONFIG_ENABLE_CONTROL_PLANE_V02;
      break;
    case GNSS_CONFIG_LPP_PROFILE_USER_PLANE_AND_CONTROL_PLANE:
      lpp_config_req.lppConfig = QMI_LOC_LPP_CONFIG_ENABLE_USER_PLANE_V02 |
                                 QMI_LOC_LPP_CONFIG_ENABLE_CONTROL_PLANE_V02;
      break;
    case GNSS_CONFIG_LPP_PROFILE_RRLP_ON_LTE:
    default:
      lpp_config_req.lppConfig = 0;
      break;
  }

  req_union.pSetProtocolConfigParametersReq = &lpp_config_req;

  result = locSyncSendReq(QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_IND_V02,
                          &lpp_config_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != lpp_config_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(lpp_config_ind.status));
     err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}


/* set the Sensor Properties */
enum loc_api_adapter_err LocApiV02 :: setSensorPropertiesSync(
        bool gyroBiasVarianceRandomWalk_valid, float gyroBiasVarianceRandomWalk,
        bool accelBiasVarianceRandomWalk_valid, float accelBiasVarianceRandomWalk,
        bool angleBiasVarianceRandomWalk_valid, float angleBiasVarianceRandomWalk,
        bool rateBiasVarianceRandomWalk_valid, float rateBiasVarianceRandomWalk,
        bool velocityBiasVarianceRandomWalk_valid, float velocityBiasVarianceRandomWalk)
{
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;

  qmiLocSetSensorPropertiesReqMsgT_v02 sensor_prop_req;
  qmiLocSetSensorPropertiesIndMsgT_v02 sensor_prop_ind;

  LOC_LOGI("%s:%d]: sensors prop: gyroBiasRandomWalk = %f, accelRandomWalk = %f, "
           "angleRandomWalk = %f, rateRandomWalk = %f, velocityRandomWalk = %f\n",
                 __func__, __LINE__, gyroBiasVarianceRandomWalk, accelBiasVarianceRandomWalk,
           angleBiasVarianceRandomWalk, rateBiasVarianceRandomWalk, velocityBiasVarianceRandomWalk);

  memset(&sensor_prop_req, 0, sizeof(sensor_prop_req));
  memset(&sensor_prop_ind, 0, sizeof(sensor_prop_ind));

  /* Set the validity bit and value for each sensor property */
  sensor_prop_req.gyroBiasVarianceRandomWalk_valid = gyroBiasVarianceRandomWalk_valid;
  sensor_prop_req.gyroBiasVarianceRandomWalk = gyroBiasVarianceRandomWalk;

  sensor_prop_req.accelerationRandomWalkSpectralDensity_valid = accelBiasVarianceRandomWalk_valid;
  sensor_prop_req.accelerationRandomWalkSpectralDensity = accelBiasVarianceRandomWalk;

  sensor_prop_req.angleRandomWalkSpectralDensity_valid = angleBiasVarianceRandomWalk_valid;
  sensor_prop_req.angleRandomWalkSpectralDensity = angleBiasVarianceRandomWalk;

  sensor_prop_req.rateRandomWalkSpectralDensity_valid = rateBiasVarianceRandomWalk_valid;
  sensor_prop_req.rateRandomWalkSpectralDensity = rateBiasVarianceRandomWalk;

  sensor_prop_req.velocityRandomWalkSpectralDensity_valid = velocityBiasVarianceRandomWalk_valid;
  sensor_prop_req.velocityRandomWalkSpectralDensity = velocityBiasVarianceRandomWalk;

  req_union.pSetSensorPropertiesReq = &sensor_prop_req;

  result = locSyncSendReq(QMI_LOC_SET_SENSOR_PROPERTIES_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_SENSOR_PROPERTIES_IND_V02,
                          &sensor_prop_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != sensor_prop_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(sensor_prop_ind.status));
  }

  return convertErr(result);
}

/* set the Sensor Performance Config */
enum loc_api_adapter_err LocApiV02 :: setSensorPerfControlConfigSync(int controlMode,
                                                                        int accelSamplesPerBatch, int accelBatchesPerSec,
                                                                        int gyroSamplesPerBatch, int gyroBatchesPerSec,
                                                                        int accelSamplesPerBatchHigh, int accelBatchesPerSecHigh,
                                                                        int gyroSamplesPerBatchHigh, int gyroBatchesPerSecHigh,
                                                                        int algorithmConfig)
{
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;

  qmiLocSetSensorPerformanceControlConfigReqMsgT_v02 sensor_perf_config_req;
  qmiLocSetSensorPerformanceControlConfigIndMsgT_v02 sensor_perf_config_ind;

  LOC_LOGD("%s:%d]: Sensor Perf Control Config (performanceControlMode)(%u) "
                "accel(#smp,#batches) (%u,%u) gyro(#smp,#batches) (%u,%u) "
                "accel_high(#smp,#batches) (%u,%u) gyro_high(#smp,#batches) (%u,%u) "
                "algorithmConfig(%u)\n",
                __FUNCTION__,
                __LINE__,
                controlMode,
                accelSamplesPerBatch,
                accelBatchesPerSec,
                gyroSamplesPerBatch,
                gyroBatchesPerSec,
                accelSamplesPerBatchHigh,
                accelBatchesPerSecHigh,
                gyroSamplesPerBatchHigh,
                gyroBatchesPerSecHigh,
                algorithmConfig
                );

  memset(&sensor_perf_config_req, 0, sizeof(sensor_perf_config_req));
  memset(&sensor_perf_config_ind, 0, sizeof(sensor_perf_config_ind));

  sensor_perf_config_req.performanceControlMode_valid = (controlMode == 2) ? 0 : 1;
  sensor_perf_config_req.performanceControlMode = (qmiLocSensorPerformanceControlModeEnumT_v02)controlMode;
  sensor_perf_config_req.accelSamplingSpec_valid = 1;
  sensor_perf_config_req.accelSamplingSpec.batchesPerSecond = accelBatchesPerSec;
  sensor_perf_config_req.accelSamplingSpec.samplesPerBatch = accelSamplesPerBatch;
  sensor_perf_config_req.gyroSamplingSpec_valid = 1;
  sensor_perf_config_req.gyroSamplingSpec.batchesPerSecond = gyroBatchesPerSec;
  sensor_perf_config_req.gyroSamplingSpec.samplesPerBatch = gyroSamplesPerBatch;
  sensor_perf_config_req.accelSamplingSpecHigh_valid = 1;
  sensor_perf_config_req.accelSamplingSpecHigh.batchesPerSecond = accelBatchesPerSecHigh;
  sensor_perf_config_req.accelSamplingSpecHigh.samplesPerBatch = accelSamplesPerBatchHigh;
  sensor_perf_config_req.gyroSamplingSpecHigh_valid = 1;
  sensor_perf_config_req.gyroSamplingSpecHigh.batchesPerSecond = gyroBatchesPerSecHigh;
  sensor_perf_config_req.gyroSamplingSpecHigh.samplesPerBatch = gyroSamplesPerBatchHigh;
  sensor_perf_config_req.algorithmConfig_valid = 1;
  sensor_perf_config_req.algorithmConfig = algorithmConfig;

  req_union.pSetSensorPerformanceControlConfigReq = &sensor_perf_config_req;

  result = locSyncSendReq(QMI_LOC_SET_SENSOR_PERFORMANCE_CONTROL_CONFIGURATION_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_SENSOR_PERFORMANCE_CONTROL_CONFIGURATION_IND_V02,
                          &sensor_perf_config_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != sensor_perf_config_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(sensor_perf_config_ind.status));
  }

  return convertErr(result);
}

/* set the Positioning Protocol on A-GLONASS system */
LocationError
LocApiV02::setAGLONASSProtocolSync(GnssConfigAGlonassPositionProtocolMask aGlonassProtocol)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocSetProtocolConfigParametersReqMsgT_v02 aGlonassProtocol_req;
  qmiLocSetProtocolConfigParametersIndMsgT_v02 aGlonassProtocol_ind;

  memset(&aGlonassProtocol_req, 0, sizeof(aGlonassProtocol_req));
  memset(&aGlonassProtocol_ind, 0, sizeof(aGlonassProtocol_ind));

  aGlonassProtocol_req.assistedGlonassProtocolMask_valid = 1;
  if (GNSS_CONFIG_RRC_CONTROL_PLANE_BIT & aGlonassProtocol) {
      aGlonassProtocol_req.assistedGlonassProtocolMask |=
          QMI_LOC_ASSISTED_GLONASS_PROTOCOL_MASK_RRC_CP_V02 ;
  }
  if (GNSS_CONFIG_RRLP_USER_PLANE_BIT & aGlonassProtocol) {
      aGlonassProtocol_req.assistedGlonassProtocolMask |=
          QMI_LOC_ASSISTED_GLONASS_PROTOCOL_MASK_RRLP_UP_V02;
  }
  if (GNSS_CONFIG_LLP_USER_PLANE_BIT & aGlonassProtocol) {
      aGlonassProtocol_req.assistedGlonassProtocolMask |=
          QMI_LOC_ASSISTED_GLONASS_PROTOCOL_MASK_LPP_UP_V02;
  }
  if (GNSS_CONFIG_LLP_CONTROL_PLANE_BIT & aGlonassProtocol) {
      aGlonassProtocol_req.assistedGlonassProtocolMask |=
          QMI_LOC_ASSISTED_GLONASS_PROTOCOL_MASK_LPP_CP_V02;
  }

  req_union.pSetProtocolConfigParametersReq = &aGlonassProtocol_req;

  LOC_LOGD("%s:%d]: aGlonassProtocolMask = 0x%x",  __func__, __LINE__,
                             aGlonassProtocol_req.assistedGlonassProtocolMask);

  result = locSyncSendReq(QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_IND_V02,
                          &aGlonassProtocol_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != aGlonassProtocol_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(aGlonassProtocol_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

LocationError
LocApiV02::setLPPeProtocolCpSync(GnssConfigLppeControlPlaneMask lppeCP)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocSetProtocolConfigParametersReqMsgT_v02 lppe_req;
  qmiLocSetProtocolConfigParametersIndMsgT_v02 lppe_ind;

  memset(&lppe_req, 0, sizeof(lppe_req));
  memset(&lppe_ind, 0, sizeof(lppe_ind));

  lppe_req.lppeCpConfig_valid = 1;
  if (GNSS_CONFIG_LPPE_CONTROL_PLANE_DBH_BIT & lppeCP) {
      lppe_req.lppeCpConfig |= QMI_LOC_LPPE_MASK_CP_DBH_V02;
  }
  if (GNSS_CONFIG_LPPE_CONTROL_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeCP) {
      lppe_req.lppeCpConfig |= QMI_LOC_LPPE_MASK_CP_AP_WIFI_MEASUREMENT_V02;
  }
  if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeCP) {
      lppe_req.lppeCpConfig |= QMI_LOC_LPPE_MASK_CP_AP_SRN_BTLE_MEASUREMENT_V02;
  }
  if (GNSS_CONFIG_LPPE_CONTROL_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeCP) {
      lppe_req.lppeCpConfig |= QMI_LOC_LPPE_MASK_CP_UBP_V02;
  }

  req_union.pSetProtocolConfigParametersReq = &lppe_req;

  LOC_LOGD("%s:%d]: lppeCpConfig = 0x%" PRIx64,  __func__, __LINE__,
           lppe_req.lppeCpConfig);

  result = locSyncSendReq(QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_IND_V02,
                          &lppe_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != lppe_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(lppe_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

LocationError
LocApiV02::setLPPeProtocolUpSync(GnssConfigLppeUserPlaneMask lppeUP)
{
  LocationError err = LOCATION_ERROR_SUCCESS;
  locClientStatusEnumType result = eLOC_CLIENT_SUCCESS;
  locClientReqUnionType req_union;
  qmiLocSetProtocolConfigParametersReqMsgT_v02 lppe_req;
  qmiLocSetProtocolConfigParametersIndMsgT_v02 lppe_ind;

  memset(&lppe_req, 0, sizeof(lppe_req));
  memset(&lppe_ind, 0, sizeof(lppe_ind));
  memset(&req_union, 0, sizeof(req_union));

  lppe_req.lppeUpConfig_valid = 1;
  if (GNSS_CONFIG_LPPE_USER_PLANE_DBH_BIT & lppeUP) {
      lppe_req.lppeUpConfig |= QMI_LOC_LPPE_MASK_UP_DBH_V02;
  }
  if (GNSS_CONFIG_LPPE_USER_PLANE_WLAN_AP_MEASUREMENTS_BIT & lppeUP) {
      lppe_req.lppeUpConfig |= QMI_LOC_LPPE_MASK_UP_AP_WIFI_MEASUREMENT_V02;
  }
  if (GNSS_CONFIG_LPPE_USER_PLANE_SRN_AP_MEASUREMENTS_BIT & lppeUP) {
      lppe_req.lppeUpConfig |= QMI_LOC_LPPE_MASK_UP_AP_SRN_BTLE_MEASUREMENT_V02;
  }
  if (GNSS_CONFIG_LPPE_USER_PLANE_SENSOR_BARO_MEASUREMENTS_BIT & lppeUP) {
      lppe_req.lppeUpConfig |= QMI_LOC_LPPE_MASK_UP_UBP_V02;
  }

  req_union.pSetProtocolConfigParametersReq = &lppe_req;

  LOC_LOGD("%s:%d]: lppeUpConfig = 0x%" PRIx64,  __func__, __LINE__,
           lppe_req.lppeUpConfig);

  result = locSyncSendReq(QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_REQ_V02,
                          req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                          QMI_LOC_SET_PROTOCOL_CONFIG_PARAMETERS_IND_V02,
                          &lppe_ind);

  if(result != eLOC_CLIENT_SUCCESS ||
     eQMI_LOC_SUCCESS_V02 != lppe_ind.status)
  {
    LOC_LOGE ("%s:%d]: Error status = %s, ind..status = %s ",
              __func__, __LINE__,
              loc_get_v02_client_status_name(result),
              loc_get_v02_qmi_status_name(lppe_ind.status));
    err = LOCATION_ERROR_GENERAL_FAILURE;
  }

  return err;
}

/* Convert event mask from loc eng to loc_api_v02 format */
locClientEventMaskType LocApiV02 :: convertMask(
  LOC_API_ADAPTER_EVENT_MASK_T mask)
{
  locClientEventMaskType eventMask = 0;
  LOC_LOGd("adapter mask = 0x%" PRIx64, mask);

  if (mask & LOC_API_ADAPTER_BIT_PARSED_POSITION_REPORT)
      eventMask |= QMI_LOC_EVENT_MASK_POSITION_REPORT_V02;

  if (mask & LOC_API_ADAPTER_BIT_PARSED_UNPROPAGATED_POSITION_REPORT)
      eventMask |= QMI_LOC_EVENT_MASK_UNPROPAGATED_POSITION_REPORT_V02;

  if (mask & LOC_API_ADAPTER_BIT_SATELLITE_REPORT)
      eventMask |= QMI_LOC_EVENT_MASK_GNSS_SV_INFO_V02;

  /* treat NMEA_1Hz and NMEA_POSITION_REPORT the same*/
  if ((mask & LOC_API_ADAPTER_BIT_NMEA_POSITION_REPORT) ||
      (mask & LOC_API_ADAPTER_BIT_NMEA_1HZ_REPORT) )
      eventMask |= QMI_LOC_EVENT_MASK_NMEA_V02;

  if (mask & LOC_API_ADAPTER_BIT_NI_NOTIFY_VERIFY_REQUEST)
      eventMask |= QMI_LOC_EVENT_MASK_NI_NOTIFY_VERIFY_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_ASSISTANCE_DATA_REQUEST)
  {
    eventMask |= QMI_LOC_EVENT_MASK_INJECT_PREDICTED_ORBITS_REQ_V02;
    eventMask |= QMI_LOC_EVENT_MASK_INJECT_TIME_REQ_V02;
  }

  if (mask & LOC_API_ADAPTER_BIT_POSITION_INJECTION_REQUEST)
      eventMask |= QMI_LOC_EVENT_MASK_INJECT_POSITION_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_STATUS_REPORT)
  {
      eventMask |= (QMI_LOC_EVENT_MASK_ENGINE_STATE_V02);
  }

  if (mask & LOC_API_ADAPTER_BIT_LOCATION_SERVER_REQUEST)
      eventMask |= QMI_LOC_EVENT_MASK_LOCATION_SERVER_CONNECTION_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_REQUEST_WIFI)
      eventMask |= QMI_LOC_EVENT_MASK_WIFI_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_SENSOR_STATUS)
      eventMask |= QMI_LOC_EVENT_MASK_SENSOR_STREAMING_READY_STATUS_V02;

  if (mask & LOC_API_ADAPTER_BIT_REQUEST_TIME_SYNC)
      eventMask |= QMI_LOC_EVENT_MASK_TIME_SYNC_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_REPORT_SPI)
      eventMask |= QMI_LOC_EVENT_MASK_SET_SPI_STREAMING_REPORT_V02;

  if (mask & LOC_API_ADAPTER_BIT_REPORT_NI_GEOFENCE)
      eventMask |= QMI_LOC_EVENT_MASK_NI_GEOFENCE_NOTIFICATION_V02;

  if (mask & LOC_API_ADAPTER_BIT_GEOFENCE_GEN_ALERT)
      eventMask |= QMI_LOC_EVENT_MASK_GEOFENCE_GEN_ALERT_V02;

  if (mask & LOC_API_ADAPTER_BIT_REPORT_GENFENCE_BREACH)
      eventMask |= QMI_LOC_EVENT_MASK_GEOFENCE_BREACH_NOTIFICATION_V02;

  if (mask & LOC_API_ADAPTER_BIT_BATCHED_GENFENCE_BREACH_REPORT) {
      if (ContextBase::isMessageSupported(LOC_API_ADAPTER_MESSAGE_BATCHED_GENFENCE_BREACH)) {
          eventMask |= QMI_LOC_EVENT_MASK_GEOFENCE_BATCH_BREACH_NOTIFICATION_V02;
      } else {
          eventMask |= QMI_LOC_EVENT_MASK_GEOFENCE_BREACH_NOTIFICATION_V02;
      }
  }

  if (mask & LOC_API_ADAPTER_BIT_PEDOMETER_CTRL)
      eventMask |= QMI_LOC_EVENT_MASK_PEDOMETER_CONTROL_V02;

  if (mask & LOC_API_ADAPTER_BIT_REPORT_GENFENCE_DWELL)
      eventMask |= QMI_LOC_EVENT_MASK_GEOFENCE_BATCH_DWELL_NOTIFICATION_V02;

  if (mask & LOC_API_ADAPTER_BIT_MOTION_CTRL)
      eventMask |= QMI_LOC_EVENT_MASK_MOTION_DATA_CONTROL_V02;

  if (mask & LOC_API_ADAPTER_BIT_REQUEST_WIFI_AP_DATA)
      eventMask |= QMI_LOC_EVENT_MASK_INJECT_WIFI_AP_DATA_REQ_V02;

  if(mask & LOC_API_ADAPTER_BIT_BATCH_FULL)
      eventMask |= QMI_LOC_EVENT_MASK_BATCH_FULL_NOTIFICATION_V02;

  if(mask & LOC_API_ADAPTER_BIT_BATCH_STATUS)
      eventMask |= QMI_LOC_EVENT_MASK_BATCHING_STATUS_V02;

  if(mask & LOC_API_ADAPTER_BIT_BATCHED_POSITION_REPORT)
      eventMask |= QMI_LOC_EVENT_MASK_LIVE_BATCHED_POSITION_REPORT_V02;

  if(mask & LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT_REPORT)
        eventMask |= QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02;

  if(mask & LOC_API_ADAPTER_BIT_GNSS_SV_POLYNOMIAL_REPORT)
      eventMask |= QMI_LOC_EVENT_MASK_GNSS_SV_POLYNOMIAL_REPORT_V02;

  // for GDT
  if(mask & LOC_API_ADAPTER_BIT_GDT_UPLOAD_BEGIN_REQ)
      eventMask |= QMI_LOC_EVENT_MASK_GDT_UPLOAD_BEGIN_REQ_V02;

  if(mask & LOC_API_ADAPTER_BIT_GDT_UPLOAD_END_REQ)
      eventMask |= QMI_LOC_EVENT_MASK_GDT_UPLOAD_END_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_GNSS_MEASUREMENT)
      eventMask |= QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02;

  if(mask & LOC_API_ADAPTER_BIT_REQUEST_TIMEZONE)
      eventMask |= QMI_LOC_EVENT_MASK_GET_TIME_ZONE_REQ_V02;

  if(mask & LOC_API_ADAPTER_BIT_REQUEST_SRN_DATA)
      eventMask |= QMI_LOC_EVENT_MASK_INJECT_SRN_AP_DATA_REQ_V02 ;

  if (mask & LOC_API_ADAPTER_BIT_FDCL_SERVICE_REQ)
      eventMask |= QMI_LOC_EVENT_MASK_FDCL_SERVICE_REQ_V02;

  if (mask & LOC_API_ADAPTER_BIT_BS_OBS_DATA_SERVICE_REQ)
      eventMask |= QMI_LOC_EVENT_MASK_BS_OBS_DATA_SERVICE_REQ_V02;

  return eventMask;
}

qmiLocLockEnumT_v02 LocApiV02 ::convertGpsLockFromAPItoQMI(GnssConfigGpsLock lock)
{
    switch (lock)
    {
      case GNSS_CONFIG_GPS_LOCK_MO_AND_NI:
        return eQMI_LOC_LOCK_ALL_V02;
      case GNSS_CONFIG_GPS_LOCK_MO:
        return eQMI_LOC_LOCK_MI_V02;
      case GNSS_CONFIG_GPS_LOCK_NI:
        return eQMI_LOC_LOCK_MT_V02;
      case GNSS_CONFIG_GPS_LOCK_NONE:
      default:
        return eQMI_LOC_LOCK_NONE_V02;
    }
}

GnssConfigGpsLock LocApiV02::convertGpsLockFromQMItoAPI(qmiLocLockEnumT_v02 lock)
{
    switch (lock) {
      case eQMI_LOC_LOCK_MI_V02:
        return GNSS_CONFIG_GPS_LOCK_MO;
      case eQMI_LOC_LOCK_MT_V02:
        return GNSS_CONFIG_GPS_LOCK_NI;
      case eQMI_LOC_LOCK_ALL_V02:
        return GNSS_CONFIG_GPS_LOCK_MO_AND_NI;
      case eQMI_LOC_LOCK_NONE_V02:
      default:
        return GNSS_CONFIG_GPS_LOCK_NONE;
    }
}

/* Convert error from loc_api_v02 to loc eng format*/
enum loc_api_adapter_err LocApiV02 :: convertErr(
  locClientStatusEnumType status)
{
  switch( status)
  {
    case eLOC_CLIENT_SUCCESS:
      return LOC_API_ADAPTER_ERR_SUCCESS;

    case eLOC_CLIENT_FAILURE_GENERAL:
      return LOC_API_ADAPTER_ERR_GENERAL_FAILURE;

    case eLOC_CLIENT_FAILURE_UNSUPPORTED:
      return LOC_API_ADAPTER_ERR_UNSUPPORTED;

    case eLOC_CLIENT_FAILURE_INVALID_PARAMETER:
      return LOC_API_ADAPTER_ERR_INVALID_PARAMETER;

    case eLOC_CLIENT_FAILURE_ENGINE_BUSY:
      return LOC_API_ADAPTER_ERR_ENGINE_BUSY;

    case eLOC_CLIENT_FAILURE_PHONE_OFFLINE:
      return LOC_API_ADAPTER_ERR_PHONE_OFFLINE;

    case eLOC_CLIENT_FAILURE_TIMEOUT:
      return LOC_API_ADAPTER_ERR_TIMEOUT;

    case eLOC_CLIENT_FAILURE_INVALID_HANDLE:
      return LOC_API_ADAPTER_ERR_INVALID_HANDLE;

    case eLOC_CLIENT_FAILURE_SERVICE_NOT_PRESENT:
      return LOC_API_ADAPTER_ERR_SERVICE_NOT_PRESENT;

    case eLOC_CLIENT_FAILURE_INTERNAL:
      return LOC_API_ADAPTER_ERR_INTERNAL;

    default:
      return LOC_API_ADAPTER_ERR_FAILURE;
  }
}

/* convert position report to loc eng format and send the converted
   position to loc eng */

void LocApiV02 :: reportPosition (
  const qmiLocEventPositionReportIndMsgT_v02 *location_report_ptr,
  bool unpropagatedPosition)
{
    UlpLocation location;
    LocPosTechMask tech_Mask = LOC_POS_TECH_MASK_DEFAULT;
    LOC_LOGD("Reporting position from V2 Adapter\n");
    memset(&location, 0, sizeof (UlpLocation));
    location.size = sizeof(location);
    location.unpropagatedPosition = unpropagatedPosition;

    GpsLocationExtended locationExtended;
    memset(&locationExtended, 0, sizeof (GpsLocationExtended));
    locationExtended.size = sizeof(locationExtended);
    if( clock_gettime( CLOCK_BOOTTIME, &locationExtended.timeStamp.apTimeStamp)== 0 )
    {
       locationExtended.timeStamp.apTimeStampUncertaintyMs = (float)ap_timestamp_uncertainty;

    }
    else
    {
       locationExtended.timeStamp.apTimeStampUncertaintyMs = FLT_MAX;
       LOC_LOGE("%s:%d Error in clock_gettime() ",__func__, __LINE__);
    }
    LOC_LOGd("QMI_PosPacketTime %" PRIu64 " (sec) %" PRIu64 " (nsec), QMI_spoofReportMask 0x%x",
                 locationExtended.timeStamp.apTimeStamp.tv_sec,
                 locationExtended.timeStamp.apTimeStamp.tv_nsec,
                 location_report_ptr->spoofReportMask);

    // Process the position from final and intermediate reports

    if( (location_report_ptr->sessionStatus == eQMI_LOC_SESS_STATUS_SUCCESS_V02) ||
        (location_report_ptr->sessionStatus == eQMI_LOC_SESS_STATUS_IN_PROGRESS_V02)
        )
    {
        // Latitude & Longitude
        if( (1 == location_report_ptr->latitude_valid) &&
            (1 == location_report_ptr->longitude_valid))
        {
            location.gpsLocation.flags  |= LOC_GPS_LOCATION_HAS_LAT_LONG;
            location.gpsLocation.latitude  = location_report_ptr->latitude;
            location.gpsLocation.longitude = location_report_ptr->longitude;

            // Time stamp (UTC)
            if(location_report_ptr->timestampUtc_valid == 1)
            {
                location.gpsLocation.timestamp = location_report_ptr->timestampUtc;
            }

            // Altitude
            if(location_report_ptr->altitudeWrtEllipsoid_valid == 1  )
            {
                location.gpsLocation.flags  |= LOC_GPS_LOCATION_HAS_ALTITUDE;
                location.gpsLocation.altitude = location_report_ptr->altitudeWrtEllipsoid;
            }

            // Speed
            if(location_report_ptr->speedHorizontal_valid == 1)
            {
                location.gpsLocation.flags  |= LOC_GPS_LOCATION_HAS_SPEED;
                location.gpsLocation.speed = location_report_ptr->speedHorizontal;
            }

            // Heading
            if(location_report_ptr->heading_valid == 1)
            {
                location.gpsLocation.flags  |= LOC_GPS_LOCATION_HAS_BEARING;
                location.gpsLocation.bearing = location_report_ptr->heading;
            }

            // Uncertainty (circular)
            if (location_report_ptr->horUncCircular_valid) {
                location.gpsLocation.flags |= LOC_GPS_LOCATION_HAS_ACCURACY;
                location.gpsLocation.accuracy = location_report_ptr->horUncCircular;
            } else if (location_report_ptr->horUncEllipseSemiMinor_valid &&
                       location_report_ptr->horUncEllipseSemiMajor_valid) {
                location.gpsLocation.flags |= LOC_GPS_LOCATION_HAS_ACCURACY;
                location.gpsLocation.accuracy =
                    sqrt((location_report_ptr->horUncEllipseSemiMinor *
                          location_report_ptr->horUncEllipseSemiMinor) +
                         (location_report_ptr->horUncEllipseSemiMajor *
                          location_report_ptr->horUncEllipseSemiMajor));
            }

            // If horConfidence_valid is true, and horConfidence value is less than 68%
            // then scale the accuracy value to 68% confidence.
            if (location_report_ptr->horConfidence_valid)
            {
                bool is_CircUnc = (location_report_ptr->horUncCircular_valid) ?
                                                                        true : false;
                scaleAccuracyTo68PercentConfidence(location_report_ptr->horConfidence,
                                                   location.gpsLocation,
                                                   is_CircUnc);
            }

            // Technology Mask
            tech_Mask |= location_report_ptr->technologyMask;
            locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_POS_TECH_MASK;
            locationExtended.tech_mask = convertPosTechMask(location_report_ptr->technologyMask);

            //Mark the location source as from GNSS
            location.gpsLocation.flags |= LOCATION_HAS_SOURCE_INFO;
            location.position_source = ULP_LOCATION_IS_FROM_GNSS;

            if(location_report_ptr->spoofReportMask_valid)
            {
                location.gpsLocation.flags |= LOC_GPS_LOCATION_HAS_SPOOF_MASK;
                location.gpsLocation.spoof_mask = (uint32_t)location_report_ptr->spoofReportMask;
            }

            if (location_report_ptr->magneticDeviation_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_MAG_DEV;
                locationExtended.magneticDeviation = location_report_ptr->magneticDeviation;
            }

            if (location_report_ptr->DOP_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_DOP;
                locationExtended.pdop = location_report_ptr->DOP.PDOP;
                locationExtended.hdop = location_report_ptr->DOP.HDOP;
                locationExtended.vdop = location_report_ptr->DOP.VDOP;
            }

            if (location_report_ptr->altitudeWrtMeanSeaLevel_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_ALTITUDE_MEAN_SEA_LEVEL;
                locationExtended.altitudeMeanSeaLevel = location_report_ptr->altitudeWrtMeanSeaLevel;
            }

            if (location_report_ptr->vertUnc_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_VERT_UNC;
               locationExtended.vert_unc = location_report_ptr->vertUnc;
            }

            if (location_report_ptr->speedUnc_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_SPEED_UNC;
               locationExtended.speed_unc = location_report_ptr->speedUnc;
            }
            if (location_report_ptr->headingUnc_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_BEARING_UNC;
               locationExtended.bearing_unc = location_report_ptr->headingUnc;
            }
            if (location_report_ptr->horReliability_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_HOR_RELIABILITY;
               switch(location_report_ptr->horReliability)
               {
                  case eQMI_LOC_RELIABILITY_NOT_SET_V02 :
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_NOT_SET;
                    break;
                  case eQMI_LOC_RELIABILITY_VERY_LOW_V02 :
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_VERY_LOW;
                    break;
                  case eQMI_LOC_RELIABILITY_LOW_V02 :
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_LOW;
                    break;
                  case eQMI_LOC_RELIABILITY_MEDIUM_V02 :
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_MEDIUM;
                    break;
                  case eQMI_LOC_RELIABILITY_HIGH_V02 :
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_HIGH;
                    break;
                  default:
                    locationExtended.horizontal_reliability = LOC_RELIABILITY_NOT_SET;
                    break;
               }
            }
            if (location_report_ptr->vertReliability_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_VERT_RELIABILITY;
               switch(location_report_ptr->vertReliability)
               {
                  case eQMI_LOC_RELIABILITY_NOT_SET_V02 :
                    locationExtended.vertical_reliability = LOC_RELIABILITY_NOT_SET;
                    break;
                  case eQMI_LOC_RELIABILITY_VERY_LOW_V02 :
                    locationExtended.vertical_reliability = LOC_RELIABILITY_VERY_LOW;
                    break;
                  case eQMI_LOC_RELIABILITY_LOW_V02 :
                    locationExtended.vertical_reliability = LOC_RELIABILITY_LOW;
                    break;
                  case eQMI_LOC_RELIABILITY_MEDIUM_V02 :
                    locationExtended.vertical_reliability = LOC_RELIABILITY_MEDIUM;
                    break;
                  case eQMI_LOC_RELIABILITY_HIGH_V02 :
                    locationExtended.vertical_reliability = LOC_RELIABILITY_HIGH;
                    break;
                  default:
                    locationExtended.vertical_reliability = LOC_RELIABILITY_NOT_SET;
                    break;
               }
            }

            if (location_report_ptr->horUncEllipseSemiMajor_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MAJOR;
                locationExtended.horUncEllipseSemiMajor = location_report_ptr->horUncEllipseSemiMajor;
            }
            if (location_report_ptr->horUncEllipseSemiMinor_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_MINOR;
                locationExtended.horUncEllipseSemiMinor = location_report_ptr->horUncEllipseSemiMinor;
            }
            if (location_report_ptr->horUncEllipseOrientAzimuth_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_HOR_ELIP_UNC_AZIMUTH;
                locationExtended.horUncEllipseOrientAzimuth = location_report_ptr->horUncEllipseOrientAzimuth;
            }

            // If the horizontal uncertainty ellipse info is available,
            // calculate the horizontal uncertainty along north and east
            if (location_report_ptr->horUncEllipseSemiMajor_valid &&
                location_report_ptr->horUncEllipseSemiMinor_valid &&
                location_report_ptr->horUncEllipseOrientAzimuth_valid)
            {
                double cosVal = cos((double)locationExtended.horUncEllipseOrientAzimuth);
                double sinVal = sin((double)locationExtended.horUncEllipseOrientAzimuth);
                double major = locationExtended.horUncEllipseSemiMajor;
                double minor = locationExtended.horUncEllipseSemiMinor;

                double northSquare = major*major * cosVal*cosVal + minor*minor * sinVal*sinVal;
                double eastSquare =  major*major * sinVal*sinVal + minor*minor * cosVal*cosVal;

                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_NORTH_STD_DEV;
                locationExtended.northStdDeviation = sqrt(northSquare);

                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_EAST_STD_DEV;
                locationExtended.eastStdDeviation  = sqrt(eastSquare);
            }

            if (location_report_ptr->gnssSvUsedList_valid &&
                      (location_report_ptr->gnssSvUsedList_len != 0))
            {
                uint32_t idx=0;
                uint32_t gnssSvUsedList_len = location_report_ptr->gnssSvUsedList_len;
                uint16_t gnssSvIdUsed = 0;

                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_GNSS_SV_USED_DATA;
                locationExtended.numOfMeasReceived = gnssSvUsedList_len;
                memset(locationExtended.measUsageInfo, 0, sizeof(locationExtended.measUsageInfo));
                // Set of used_in_fix SV ID
                for (idx = 0; idx < gnssSvUsedList_len; idx++)
                {
                    gnssSvIdUsed = location_report_ptr->gnssSvUsedList[idx];
                    locationExtended.measUsageInfo[idx].gnssSvId = gnssSvIdUsed;
                    locationExtended.measUsageInfo[idx].carrierPhaseAmbiguityType =
                        CARRIER_PHASE_AMBIGUITY_RESOLUTION_NONE;
                    if (gnssSvIdUsed <= GPS_SV_PRN_MAX)
                    {
                        locationExtended.gnss_sv_used_ids.gps_sv_used_ids_mask |=
                                                    (1 << (gnssSvIdUsed - GPS_SV_PRN_MIN));
                        locationExtended.measUsageInfo[idx].gnssConstellation =
                                GNSS_LOC_SV_SYSTEM_GPS;
                        locationExtended.measUsageInfo[idx].gnssSignalType =
                                GNSS_SIGNAL_GPS_L1CA;
                    }
                    else if ((gnssSvIdUsed >= GLO_SV_PRN_MIN) && (gnssSvIdUsed <= GLO_SV_PRN_MAX))
                    {
                        locationExtended.gnss_sv_used_ids.glo_sv_used_ids_mask |=
                                                    (1 << (gnssSvIdUsed - GLO_SV_PRN_MIN));
                        locationExtended.measUsageInfo[idx].gnssConstellation =
                                GNSS_LOC_SV_SYSTEM_GLONASS;
                        locationExtended.measUsageInfo[idx].gnssSignalType =
                                GNSS_SIGNAL_GLONASS_G1;
                    }
                    else if ((gnssSvIdUsed >= BDS_SV_PRN_MIN) && (gnssSvIdUsed <= BDS_SV_PRN_MAX))
                    {
                        locationExtended.gnss_sv_used_ids.bds_sv_used_ids_mask |=
                                                    (1 << (gnssSvIdUsed - BDS_SV_PRN_MIN));
                        locationExtended.measUsageInfo[idx].gnssConstellation =
                                GNSS_LOC_SV_SYSTEM_BDS;
                        locationExtended.measUsageInfo[idx].gnssSignalType =
                                GNSS_SIGNAL_BEIDOU_B1;
                    }
                    else if ((gnssSvIdUsed >= GAL_SV_PRN_MIN) && (gnssSvIdUsed <= GAL_SV_PRN_MAX))
                    {
                        locationExtended.gnss_sv_used_ids.gal_sv_used_ids_mask |=
                                                    (1 << (gnssSvIdUsed - GAL_SV_PRN_MIN));
                        locationExtended.measUsageInfo[idx].gnssConstellation =
                                GNSS_LOC_SV_SYSTEM_GALILEO;
                        locationExtended.measUsageInfo[idx].gnssSignalType =
                                GNSS_SIGNAL_GALILEO_E1;
                    }
                    else if ((gnssSvIdUsed >= QZSS_SV_PRN_MIN) && (gnssSvIdUsed <= QZSS_SV_PRN_MAX))
                    {
                        locationExtended.gnss_sv_used_ids.qzss_sv_used_ids_mask |=
                                                    (1 << (gnssSvIdUsed - QZSS_SV_PRN_MIN));
                        locationExtended.measUsageInfo[idx].gnssConstellation =
                                GNSS_LOC_SV_SYSTEM_QZSS;
                        locationExtended.measUsageInfo[idx].gnssSignalType =
                                GNSS_SIGNAL_QZSS_L1CA;
                    }
                }
            }

            if (location_report_ptr->navSolutionMask_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_NAV_SOLUTION_MASK;
               locationExtended.navSolutionMask = convertNavSolutionMask(location_report_ptr->navSolutionMask);
            }

            if (location_report_ptr->gpsTime_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_GPS_TIME;
               locationExtended.gpsTime.gpsWeek = location_report_ptr->gpsTime.gpsWeek;
               locationExtended.gpsTime.gpsTimeOfWeekMs = location_report_ptr->gpsTime.gpsTimeOfWeekMs;
            }

            if (location_report_ptr->extDOP_valid)
            {
                locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_EXT_DOP;
                locationExtended.extDOP.PDOP = location_report_ptr->extDOP.PDOP;
                locationExtended.extDOP.HDOP = location_report_ptr->extDOP.HDOP;
                locationExtended.extDOP.VDOP = location_report_ptr->extDOP.VDOP;
                locationExtended.extDOP.GDOP = location_report_ptr->extDOP.GDOP;
                locationExtended.extDOP.TDOP = location_report_ptr->extDOP.TDOP;
            }

            if (location_report_ptr->velEnu_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_EAST_VEL;
               locationExtended.eastVelocity = location_report_ptr->velEnu[0];
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_NORTH_VEL;
               locationExtended.northVelocity = location_report_ptr->velEnu[1];
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_UP_VEL;
               locationExtended.upVelocity = location_report_ptr->velEnu[2];
            }

            if (location_report_ptr->velUncEnu_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_EAST_VEL_UNC;
               locationExtended.eastVelocityStdDeviation = location_report_ptr->velUncEnu[0];
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_NORTH_VEL_UNC;
               locationExtended.northVelocityStdDeviation = location_report_ptr->velUncEnu[1];
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_UP_VEL_UNC;
               locationExtended.upVelocityStdDeviation = location_report_ptr->velUncEnu[2];
            }
            // fill in GnssSystemTime based on gps timestamp and time uncertainty
            locationExtended.gnssSystemTime.gnssSystemTimeSrc = (Gnss_LocSvSystemEnumType)0;
            if (location_report_ptr->gpsTime_valid)
            {
                locationExtended.gnssSystemTime.gnssSystemTimeSrc = GNSS_LOC_SV_SYSTEM_GPS;
                locationExtended.gnssSystemTime.u.gpsSystemTime.validityMask = 0x0;

                locationExtended.gnssSystemTime.u.gpsSystemTime.systemWeek =
                        locationExtended.gpsTime.gpsWeek;
                locationExtended.gnssSystemTime.u.gpsSystemTime.validityMask |=
                        GNSS_SYSTEM_TIME_WEEK_VALID;

                locationExtended.gnssSystemTime.u.gpsSystemTime.systemMsec =
                        locationExtended.gpsTime.gpsTimeOfWeekMs;
                locationExtended.gnssSystemTime.u.gpsSystemTime.validityMask |=
                        GNSS_SYSTEM_TIME_WEEK_MS_VALID;

                locationExtended.gnssSystemTime.u.gpsSystemTime.systemClkTimeBias = 0.0f;
                locationExtended.gnssSystemTime.u.gpsSystemTime.validityMask |=
                        GNSS_SYSTEM_CLK_TIME_BIAS_VALID;

                if (location_report_ptr->timeUnc_valid)
                {
                    locationExtended.gnssSystemTime.u.gpsSystemTime.systemClkTimeUncMs =
                            locationExtended.timeUncMs;
                    locationExtended.gnssSystemTime.u.gpsSystemTime.validityMask |=
                            GNSS_SYSTEM_CLK_TIME_BIAS_UNC_VALID;
                }
            }

            if (location_report_ptr->timeUnc_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_TIME_UNC;
               locationExtended.timeUncMs = location_report_ptr->timeUnc;
            }

            if (location_report_ptr->leapSeconds_valid)
            {
               locationExtended.flags |= GPS_LOCATION_EXTENDED_HAS_LEAP_SECONDS;
               locationExtended.leapSeconds = location_report_ptr->leapSeconds;
            }

            LocApiBase::reportPosition(location,
                                       locationExtended,
                                       (location_report_ptr->sessionStatus ==
                                        eQMI_LOC_SESS_STATUS_IN_PROGRESS_V02 ?
                                        LOC_SESS_INTERMEDIATE : LOC_SESS_SUCCESS),
                                       tech_Mask);
        }
    }
    else
    {
        LocApiBase::reportPosition(location,
                                   locationExtended,
                                   LOC_SESS_FAILURE);

        LOC_LOGD("%s:%d]: Ignoring position report with sess status = %d, "
                      "fix id = %u\n", __func__, __LINE__,
                      location_report_ptr->sessionStatus,
                      location_report_ptr->fixId );
    }
}

/*convert signal type to carrier frequency*/
float LocApiV02::convertSignalTypeToCarrierFrequency(
    qmiLocGnssSignalTypeMaskT_v02 signalType,
    uint8_t gloFrequency)
{
    float carrierFrequency = 0.0;

    LOC_LOGv("signalType = 0x%" PRIx64 , signalType);
    switch (signalType) {
    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GPS_L1CA_V02:
        carrierFrequency = GPS_L1CA_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GPS_L1C_V02:
        carrierFrequency = GPS_L1C_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GPS_L2C_L_V02:
        carrierFrequency = GPS_L2C_L_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GPS_L5_Q_V02:
        carrierFrequency = GPS_L5_Q_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GLONASS_G1_V02:
        carrierFrequency = GLONASS_G1_CARRIER_FREQUENCY;
        if ((gloFrequency >= 1 && gloFrequency <= 14)) {
            carrierFrequency += ((gloFrequency - 8) * 562500);
        }
        LOC_LOGv("GLO carFreq after conversion = %f", carrierFrequency);
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GLONASS_G2_V02:
        carrierFrequency = GLONASS_G2_CARRIER_FREQUENCY;
        if ((gloFrequency >= 1 && gloFrequency <= 14)) {
            carrierFrequency += ((gloFrequency - 8) * 437500);
        }
        LOC_LOGv("GLO carFreq after conversion = %f", carrierFrequency);
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GALILEO_E1_C_V02:
        carrierFrequency = GALILEO_E1_C_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GALILEO_E5A_Q_V02:
        carrierFrequency = GALILEO_E5A_Q_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_GALILEO_E5B_Q_V02:
        carrierFrequency = GALILEO_E5B_Q_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_BEIDOU_B1_I_V02:
        carrierFrequency = BEIDOU_B1_I_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_BEIDOU_B1C_V02:
        carrierFrequency = BEIDOU_B1C_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_BEIDOU_B2_I_V02:
        carrierFrequency = BEIDOU_B2_I_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_BEIDOU_B2A_I_V02:
        carrierFrequency = BEIDOU_B2A_I_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_QZSS_L1CA_V02:
        carrierFrequency = QZSS_L1CA_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_QZSS_L1S_V02:
        carrierFrequency = QZSS_L1S_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_QZSS_L2C_L_V02:
        carrierFrequency = QZSS_L2C_L_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_QZSS_L5_Q_V02:
        carrierFrequency = QZSS_L5_Q_CARRIER_FREQUENCY;
        break;

    case QMI_LOC_MASK_GNSS_SIGNAL_TYPE_SBAS_L1_CA_V02:
        carrierFrequency = SBAS_L1_CA_CARRIER_FREQUENCY;
        break;

    default:
        break;
    }
    return carrierFrequency;
}

/* convert satellite report to location api format and send the converted
   report to base */
void  LocApiV02 :: reportSv (
    const qmiLocEventGnssSvInfoIndMsgT_v02 *gnss_report_ptr)
{
    GnssSvNotification SvNotify = {};
    int              num_svs_max, i;
    const qmiLocSvInfoStructT_v02 *sv_info_ptr;
    uint8_t gloFrequency = 0;

    num_svs_max = 0;
    if (1 == gnss_report_ptr->expandedSvList_valid) {
        num_svs_max = gnss_report_ptr->expandedSvList_len;
    }
    else if (1 == gnss_report_ptr->svList_valid) {
        num_svs_max = gnss_report_ptr->svList_len;
    }
    if (num_svs_max > GNSS_SV_MAX) {
        num_svs_max = GNSS_SV_MAX;
    }

    SvNotify.size = sizeof(GnssSvNotification);
    if (1 == gnss_report_ptr->svList_valid ||
        1 == gnss_report_ptr->expandedSvList_valid) {
        SvNotify.count = 0;
        for(i = 0; i < num_svs_max; i++) {
            if (1 == gnss_report_ptr->expandedSvList_valid) {
                sv_info_ptr = &(gnss_report_ptr->expandedSvList[i].svInfo);
            }
            else {
                sv_info_ptr = &(gnss_report_ptr->svList[i]);
            }
            if((sv_info_ptr->validMask & QMI_LOC_SV_INFO_MASK_VALID_SYSTEM_V02) &&
               (sv_info_ptr->validMask & QMI_LOC_SV_INFO_MASK_VALID_GNSS_SVID_V02)
                && (sv_info_ptr->gnssSvId != 0 ))
            {
                GnssSvOptionsMask mask = 0;

                LOC_LOGv("i:%d count:%d sys:%d en:0x%X",
                    i, SvNotify.count, sv_info_ptr->system,
                    gnss_report_ptr->gnssSignalTypeList[SvNotify.count]);

                GnssSv &gnssSv_ref = SvNotify.gnssSvs[SvNotify.count];

                gnssSv_ref.size = sizeof(GnssSv);
                switch (sv_info_ptr->system) {
                case eQMI_LOC_SV_SYSTEM_GPS_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId;
                    gnssSv_ref.type = GNSS_SV_TYPE_GPS;
                    break;

                case eQMI_LOC_SV_SYSTEM_GALILEO_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId - 300;
                    gnssSv_ref.type = GNSS_SV_TYPE_GALILEO;
                    break;

                case eQMI_LOC_SV_SYSTEM_SBAS_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId;
                    gnssSv_ref.type = GNSS_SV_TYPE_SBAS;
                    break;

                case eQMI_LOC_SV_SYSTEM_GLONASS_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId;
                    gnssSv_ref.type = GNSS_SV_TYPE_GLONASS;
                    break;

                case eQMI_LOC_SV_SYSTEM_BDS_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId - 200;
                    gnssSv_ref.type = GNSS_SV_TYPE_BEIDOU;
                    break;

                case eQMI_LOC_SV_SYSTEM_QZSS_V02:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId - 192;
                    gnssSv_ref.type = GNSS_SV_TYPE_QZSS;
                    break;

                case eQMI_LOC_SV_SYSTEM_COMPASS_V02:
                default:
                    gnssSv_ref.svId = sv_info_ptr->gnssSvId;
                    gnssSv_ref.type = GNSS_SV_TYPE_UNKNOWN;
                    break;
                }

                if (sv_info_ptr->validMask & QMI_LOC_SV_INFO_MASK_VALID_SNR_V02) {
                    gnssSv_ref.cN0Dbhz = sv_info_ptr->snr;
                }

                if (sv_info_ptr->validMask & QMI_LOC_SV_INFO_MASK_VALID_ELEVATION_V02) {
                    gnssSv_ref.elevation = sv_info_ptr->elevation;
                }

                if (sv_info_ptr->validMask & QMI_LOC_SV_INFO_MASK_VALID_AZIMUTH_V02) {
                    gnssSv_ref.azimuth = sv_info_ptr->azimuth;
                }

                if (sv_info_ptr->validMask &
                    QMI_LOC_SV_INFO_MASK_VALID_SVINFO_MASK_V02) {
                    if (sv_info_ptr->svInfoMask &
                        QMI_LOC_SVINFO_MASK_HAS_EPHEMERIS_V02) {
                        mask |= GNSS_SV_OPTIONS_HAS_EPHEMER_BIT;
                    }
                    if (sv_info_ptr->svInfoMask &
                        QMI_LOC_SVINFO_MASK_HAS_ALMANAC_V02) {
                        mask |= GNSS_SV_OPTIONS_HAS_ALMANAC_BIT;
                    }
                }

                if (gnss_report_ptr->gnssSignalTypeList_valid) {
                    if (SvNotify.count > gnss_report_ptr->gnssSignalTypeList_len - 1) {
                        LOC_LOGv("Frequency not available for this SV");
                    }
                    else {
                        if (1 == gnss_report_ptr->expandedSvList_valid) {
                            gloFrequency = gnss_report_ptr->expandedSvList[i].gloFrequency;
                            LOC_LOGv("gloFrequency = 0x%X", gloFrequency);
                        }

                        if (gnss_report_ptr->gnssSignalTypeList[SvNotify.count] != 0) {
                            gnssSv_ref.carrierFrequencyHz =
                                    convertSignalTypeToCarrierFrequency(
                                        gnss_report_ptr->gnssSignalTypeList[SvNotify.count],
                                        gloFrequency);
                            mask |= GNSS_SV_OPTIONS_HAS_CARRIER_FREQUENCY_BIT;
                        }
                    }
                }

                gnssSv_ref.gnssSvOptionsMask = mask;

                SvNotify.count++;
            }
        }
    }

    LocApiBase::reportSv(SvNotify);
}

/* convert satellite measurementreport to loc eng format and  send the converted
   report to loc eng */
void  LocApiV02 :: reportSvMeasurement (
  const qmiLocEventGnssSvMeasInfoIndMsgT_v02 *gnss_raw_measurement_ptr)
{
  GnssSvMeasurementSet                 svMeasurementSet;
  memset(&svMeasurementSet, 0, sizeof(GnssSvMeasurementSet));
  svMeasurementSet.size = sizeof(svMeasurementSet);

  if( clock_gettime( CLOCK_BOOTTIME, &svMeasurementSet.timeStamp.apTimeStamp)== 0 )
  {
    svMeasurementSet.timeStamp.apTimeStampUncertaintyMs = (float)ap_timestamp_uncertainty;
  }
  else
  {
    svMeasurementSet.timeStamp.apTimeStampUncertaintyMs = FLT_MAX;
    LOC_LOGE("%s:%d Error in clock_gettime() ",__func__, __LINE__);
  }
  LOC_LOGD("%s:%d QMI_MeasPacketTime  %ld (sec)  %ld (nsec)",__func__,__LINE__,
            svMeasurementSet.timeStamp.apTimeStamp.tv_sec,
            svMeasurementSet.timeStamp.apTimeStamp.tv_nsec);

  LOC_LOGI("[SvMeas] SeqNum: %d, MaxMsgNum: %d, MeasValid: %d, #of SV: %d\n",
           gnss_raw_measurement_ptr->seqNum,
           gnss_raw_measurement_ptr->maxMessageNum,
           gnss_raw_measurement_ptr->svMeasurement_valid,
           (gnss_raw_measurement_ptr->svMeasurement_valid)?
           gnss_raw_measurement_ptr->svMeasurement_len : 0);

  svMeasurementSet.seqNum           = gnss_raw_measurement_ptr->seqNum;
  svMeasurementSet.maxMessageNum    = gnss_raw_measurement_ptr->maxMessageNum;

  if(1 == gnss_raw_measurement_ptr->rcvrClockFrequencyInfo_valid)
  {
    qmiLocRcvrClockFrequencyInfoStructT_v02* rcvClockFreqInfo =
      (qmiLocRcvrClockFrequencyInfoStructT_v02*) &gnss_raw_measurement_ptr->rcvrClockFrequencyInfo;

    svMeasurementSet.clockFreq.size         = sizeof(Gnss_LocRcvrClockFrequencyInfoStructType);
    svMeasurementSet.clockFreqValid         = gnss_raw_measurement_ptr->rcvrClockFrequencyInfo_valid;
    svMeasurementSet.clockFreq.clockDrift   =
        gnss_raw_measurement_ptr->rcvrClockFrequencyInfo.clockDrift;
    svMeasurementSet.clockFreq.clockDriftUnc =
        gnss_raw_measurement_ptr->rcvrClockFrequencyInfo.clockDriftUnc;
    svMeasurementSet.clockFreq.sourceOfFreq = (Gnss_LocSourceofFreqEnumType)
        gnss_raw_measurement_ptr->rcvrClockFrequencyInfo.sourceOfFreq;

    LOC_LOGV("FreqInfo:: Drift: %f, DriftUnc: %f",
             svMeasurementSet.clockFreq.clockDrift,
             svMeasurementSet.clockFreq.clockDriftUnc);
  }

  if((1 == gnss_raw_measurement_ptr->leapSecondInfo_valid) &&
     (0 == gnss_raw_measurement_ptr->leapSecondInfo.leapSecUnc) )
  {
    qmiLocLeapSecondInfoStructT_v02* leapSecond =
      (qmiLocLeapSecondInfoStructT_v02*)&gnss_raw_measurement_ptr->leapSecondInfo;

    svMeasurementSet.leapSec.size       = sizeof(Gnss_LeapSecondInfoStructType);
    svMeasurementSet.leapSecValid       = (bool)gnss_raw_measurement_ptr->leapSecondInfo_valid;
    svMeasurementSet.leapSec.leapSec    = gnss_raw_measurement_ptr->leapSecondInfo.leapSec;
    svMeasurementSet.leapSec.leapSecUnc = gnss_raw_measurement_ptr->leapSecondInfo.leapSecUnc;
    LOC_LOGV("leapSecondInfo:: leapSec: %d, leapSecUnc: %d",
      svMeasurementSet.leapSec.leapSec, svMeasurementSet.leapSec.leapSecUnc);
  }

  if(1 == gnss_raw_measurement_ptr->gpsGloInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->gpsGloInterSystemBias;

    getInterSystemTimeBias("gpsGloInterSystemBias",
                           svMeasurementSet.gpsGloInterSystemBias, interSystemBias);
  }

  if(1 == gnss_raw_measurement_ptr->gpsBdsInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->gpsBdsInterSystemBias;

    getInterSystemTimeBias("gpsBdsInterSystemBias",
                           svMeasurementSet.gpsBdsInterSystemBias, interSystemBias);
  }

  if(1 == gnss_raw_measurement_ptr->gpsGalInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->gpsGalInterSystemBias;

    getInterSystemTimeBias("gpsGalInterSystemBias",
                           svMeasurementSet.gpsGalInterSystemBias, interSystemBias);
  }

  if(1 == gnss_raw_measurement_ptr->bdsGloInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->bdsGloInterSystemBias;

    getInterSystemTimeBias("bdsGloInterSystemBias",
                           svMeasurementSet.bdsGloInterSystemBias, interSystemBias);
  }

  if(1 == gnss_raw_measurement_ptr->galGloInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->galGloInterSystemBias;

    getInterSystemTimeBias("galGloInterSystemBias",
                           svMeasurementSet.galGloInterSystemBias, interSystemBias);
  }

  if(1 == gnss_raw_measurement_ptr->galBdsInterSystemBias_valid)
  {
    qmiLocInterSystemBiasStructT_v02* interSystemBias =
      (qmiLocInterSystemBiasStructT_v02*)&gnss_raw_measurement_ptr->galBdsInterSystemBias;

    getInterSystemTimeBias("galBdsInterSystemBias",
                           svMeasurementSet.galBdsInterSystemBias,interSystemBias);
  }

  svMeasurementSet.gnssMeas.size  = sizeof(Gnss_SVMeasurementStructType);
  svMeasurementSet.gnssMeas.system  = (Gnss_LocSvSystemEnumType)gnss_raw_measurement_ptr->system;

  if(1 == gnss_raw_measurement_ptr->systemTime_valid)
  {
    svMeasurementSet.gnssMeas.isSystemTimeValid = gnss_raw_measurement_ptr->systemTime_valid;
    svMeasurementSet.gnssMeas.systemTime.size        = sizeof(Gnss_LocSystemTimeStructType);

    svMeasurementSet.gnssMeas.systemTime.systemWeek  =
        gnss_raw_measurement_ptr->systemTime.systemWeek;

    svMeasurementSet.gnssMeas.systemTime.systemMsec  =
        gnss_raw_measurement_ptr->systemTime.systemMsec;

    svMeasurementSet.gnssMeas.systemTime.systemClkTimeBias  =
        gnss_raw_measurement_ptr->systemTime.systemClkTimeBias;

    svMeasurementSet.gnssMeas.systemTime.systemClkTimeUncMs =
        gnss_raw_measurement_ptr->systemTime.systemClkTimeUncMs;
  }

  if(1 == gnss_raw_measurement_ptr->gloTime_valid)
  {
    svMeasurementSet.gnssMeas.isGloTime_valid     = gnss_raw_measurement_ptr->gloTime_valid;
    svMeasurementSet.gnssMeas.gloTime.size        = sizeof(Gnss_LocGloTimeStructType);

    svMeasurementSet.gnssMeas.gloTime.gloDays     = gnss_raw_measurement_ptr->gloTime.gloDays;
    svMeasurementSet.gnssMeas.gloTime.gloFourYear = gnss_raw_measurement_ptr->gloTime.gloFourYear;
    svMeasurementSet.gnssMeas.gloTime.gloMsec     = gnss_raw_measurement_ptr->gloTime.gloMsec;
    svMeasurementSet.gnssMeas.gloTime.gloClkTimeBias    = gnss_raw_measurement_ptr->gloTime.gloClkTimeBias;
    svMeasurementSet.gnssMeas.gloTime.gloClkTimeUncMs   = gnss_raw_measurement_ptr->gloTime.gloClkTimeUncMs;
  }

  if(1 == gnss_raw_measurement_ptr->systemTimeExt_valid)
  {
    svMeasurementSet.gnssMeas.isSystemTimeExt_valid  = gnss_raw_measurement_ptr->systemTimeExt_valid;
    svMeasurementSet.gnssMeas.systemTimeExt.size     = sizeof(Gnss_LocGnssTimeExtStructType);

    svMeasurementSet.gnssMeas.systemTimeExt.refFCount   = gnss_raw_measurement_ptr->systemTimeExt.refFCount;

    svMeasurementSet.gnssMeas.systemTimeExt.systemRtc_valid  =
      gnss_raw_measurement_ptr->systemTimeExt.systemRtc_valid;

    svMeasurementSet.gnssMeas.systemTimeExt.systemRtcMs =
      gnss_raw_measurement_ptr->systemTimeExt.systemRtcMs;

    svMeasurementSet.gnssMeas.systemTimeExt.sourceOfTime  =
      gnss_raw_measurement_ptr->systemTimeExt.sourceOfTime;

  }

  svMeasurementSet.numClockResets_valid = gnss_raw_measurement_ptr->numClockResets_valid;
  svMeasurementSet.numClockResets = gnss_raw_measurement_ptr->numClockResets;

  if(1 == gnss_raw_measurement_ptr->svMeasurement_valid)
  {

    svMeasurementSet.gnssMeas.numSvs = gnss_raw_measurement_ptr->svMeasurement_len;
    svMeasurementSet.gnssMeasValid   = gnss_raw_measurement_ptr->svMeasurement_valid;

    if(gnss_raw_measurement_ptr->svMeasurement_len > GNSS_LOC_SV_MEAS_LIST_MAX_SIZE)
    {
      //This should not happen normally, anycase limit to Max List Size
      svMeasurementSet.gnssMeas.numSvs = GNSS_LOC_SV_MEAS_LIST_MAX_SIZE;
    }
    svMeasurementSet.gnssMeas.numSvs = gnss_raw_measurement_ptr->svMeasurement_len;
    svMeasurementSet.gnssMeasValid   = gnss_raw_measurement_ptr->svMeasurement_valid;

    uint32_t i = 0, cnt=0;
    for(i=0;i<gnss_raw_measurement_ptr->svMeasurement_len;i++)
    {
      svMeasurementSet.gnssMeas.svMeasurement[i].size = sizeof(Gnss_SVMeasurementStructType);

      if((0 != gnss_raw_measurement_ptr->svMeasurement[i].gnssSvId) &&
         (0 != gnss_raw_measurement_ptr->svMeasurement[i].measurementStatus))
      {
        svMeasurementSet.gnssMeas.svMeasurement[i].gnssSvId =
                 gnss_raw_measurement_ptr->svMeasurement[i].gnssSvId;

        svMeasurementSet.gnssMeas.svMeasurement[i].gloFrequency =
                 gnss_raw_measurement_ptr->svMeasurement[i].gloFrequency;

        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_LOSSOFLOCK_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].lossOfLock = (bool)
                 gnss_raw_measurement_ptr->svMeasurement[i].lossOfLock;
        }

        svMeasurementSet.gnssMeas.svMeasurement[i].svStatus = (Gnss_LocSvSearchStatusEnumT)
                         gnss_raw_measurement_ptr->svMeasurement[i].svStatus;

        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_HEALTH_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].healthStatus_valid = 1;
          svMeasurementSet.gnssMeas.svMeasurement[i].healthStatus = (uint8_t)gnss_raw_measurement_ptr->svMeasurement[i].healthStatus;
        }
        svMeasurementSet.gnssMeas.svMeasurement[i].svInfoMask = (Gnss_LocSvInfoMaskT)
                 gnss_raw_measurement_ptr->svMeasurement[i].svInfoMask;

        svMeasurementSet.gnssMeas.svMeasurement[i].CNo =
                 gnss_raw_measurement_ptr->svMeasurement[i].CNo;

        svMeasurementSet.gnssMeas.svMeasurement[i].gloRfLoss =
                 gnss_raw_measurement_ptr->svMeasurement[i].gloRfLoss;

        svMeasurementSet.gnssMeas.svMeasurement[i].measLatency =
                 gnss_raw_measurement_ptr->svMeasurement[i].measLatency;

        /*SVTimeSpeed*/
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.size = sizeof(Gnss_LocSVTimeSpeedStructType);
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.svMs =
                 gnss_raw_measurement_ptr->svMeasurement[i].svTimeSpeed.svTimeMs;
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.svSubMs =
                 gnss_raw_measurement_ptr->svMeasurement[i].svTimeSpeed.svTimeSubMs;
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.svTimeUncMs =
                 gnss_raw_measurement_ptr->svMeasurement[i].svTimeSpeed.svTimeUncMs;
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.dopplerShift =
                 gnss_raw_measurement_ptr->svMeasurement[i].svTimeSpeed.dopplerShift;
        svMeasurementSet.gnssMeas.svMeasurement[i].svTimeSpeed.dopplerShiftUnc=
                 gnss_raw_measurement_ptr->svMeasurement[i].svTimeSpeed.dopplerShiftUnc;

        svMeasurementSet.gnssMeas.svMeasurement[i].measurementStatus =
                 (uint32_t)gnss_raw_measurement_ptr->svMeasurement[i].measurementStatus;

        svMeasurementSet.gnssMeas.svMeasurement[i].validMeasStatusMask =
                gnss_raw_measurement_ptr->svMeasurement[i].validMeasStatusMask;

        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_MULTIPATH_EST_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].multipathEstValid = 1;
          svMeasurementSet.gnssMeas.svMeasurement[i].multipathEstimate =
            gnss_raw_measurement_ptr->svMeasurement[i].multipathEstimate;
        }

        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_FINE_SPEED_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].fineSpeedValid = 1;

          svMeasurementSet.gnssMeas.svMeasurement[i].fineSpeed  =
            gnss_raw_measurement_ptr->svMeasurement[i].fineSpeed;
        }
        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_FINE_SPEED_UNC_VALID_V02)
        {
           svMeasurementSet.gnssMeas.svMeasurement[i].fineSpeedUncValid = 1;

          svMeasurementSet.gnssMeas.svMeasurement[i].fineSpeedUnc =
            gnss_raw_measurement_ptr->svMeasurement[i].fineSpeedUnc;
        }
        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_CARRIER_PHASE_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].carrierPhaseValid = 1;

          svMeasurementSet.gnssMeas.svMeasurement[i].carrierPhase =
            gnss_raw_measurement_ptr->svMeasurement[i].carrierPhase;
        }
        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_SV_DIRECTION_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].svDirectionValid = 1;

          svMeasurementSet.gnssMeas.svMeasurement[i].svElevation =
            gnss_raw_measurement_ptr->svMeasurement[i].svElevation;
          svMeasurementSet.gnssMeas.svMeasurement[i].svAzimuth =
            gnss_raw_measurement_ptr->svMeasurement[i].svAzimuth;
        }
        if(gnss_raw_measurement_ptr->svMeasurement[i].validMask & QMI_LOC_SV_CYCLESLIP_COUNT_VALID_V02)
        {
          svMeasurementSet.gnssMeas.svMeasurement[i].cycleSlipCountValid = 1;
          svMeasurementSet.gnssMeas.svMeasurement[i].cycleSlipCount =
            gnss_raw_measurement_ptr->svMeasurement[i].cycleSlipCount;
        }

        cnt++;

      }

      svMeasurementSet.gnssMeas.numSvs = cnt;   /*set the measurement length to the actual SVId's filled in the array*/

    }

    if(gnss_raw_measurement_ptr->svMeasurement_len != cnt)
    {
      LOC_LOGW("[SV_MEAS_QMI] #of SV in QMI: %d, Valid SV-id Count: %d",
                 gnss_raw_measurement_ptr->svMeasurement_len,cnt );
    }

    // svCarrierPhseUnc
    LOC_LOGV("[SV_MEAS] svCarrierPhseUnc_valid=%u _len=%u",
            gnss_raw_measurement_ptr->svCarrierPhaseUncertainty_valid,
            gnss_raw_measurement_ptr->svCarrierPhaseUncertainty_len);
    if ((1 == gnss_raw_measurement_ptr->svCarrierPhaseUncertainty_valid) &&
            (gnss_raw_measurement_ptr->svMeasurement_len ==
            gnss_raw_measurement_ptr->svCarrierPhaseUncertainty_len)) {

        for(i=0;i<gnss_raw_measurement_ptr->svCarrierPhaseUncertainty_len;i++) {
            svMeasurementSet.gnssMeas.svMeasurement[i].carrierPhaseUncValid = 1;
            svMeasurementSet.gnssMeas.svMeasurement[i].carrierPhaseUnc =
                    gnss_raw_measurement_ptr->svCarrierPhaseUncertainty[i];
        }
    }

  } //if svClockMeasurement_valid
  else
  {
    LOC_LOGV("%s] [SV_MEAS] SV Measurement Not Valid", __func__);
  }
  //Report SV measurement irrespective of #of SVs for APDR
  LocApiBase::reportSvMeasurement(svMeasurementSet);
}

/* convert satellite polynomial to loc eng format and  send the converted
   report to loc eng */
void  LocApiV02 :: reportSvPolynomial (
  const qmiLocEventGnssSvPolyIndMsgT_v02 *gnss_sv_poly_ptr)
{
  GnssSvPolynomial  svPolynomial;

  memset(&svPolynomial, 0, sizeof(GnssSvPolynomial));
  svPolynomial.size = sizeof(GnssSvPolynomial);
  svPolynomial.is_valid = 0;

  if(0 != gnss_sv_poly_ptr->gnssSvId)
  {
    svPolynomial.gnssSvId       = gnss_sv_poly_ptr->gnssSvId;
    svPolynomial.T0             = gnss_sv_poly_ptr->T0;
    svPolynomial.svPolyFlags    = gnss_sv_poly_ptr->svPolyFlags;

    if(1 == gnss_sv_poly_ptr->gloFrequency_valid)
    {
      svPolynomial.is_valid  |= ULP_GNSS_SV_POLY_BIT_GLO_FREQ;
      svPolynomial.freqNum   = gnss_sv_poly_ptr->gloFrequency;
    }
    if(1 == gnss_sv_poly_ptr->IODE_valid)
    {
      svPolynomial.is_valid  |= ULP_GNSS_SV_POLY_BIT_IODE;
      svPolynomial.iode       = gnss_sv_poly_ptr->IODE;
     }

    if(1 == gnss_sv_poly_ptr->svPosUnc_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_SV_POSUNC;
      svPolynomial.svPosUnc = gnss_sv_poly_ptr->svPosUnc;
    }

    if(1 == gnss_sv_poly_ptr->svPolyFlagValid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_FLAG;
      svPolynomial.svPolyFlags  = gnss_sv_poly_ptr->svPolyFlags;
    }

    if(1 == gnss_sv_poly_ptr->polyCoeffXYZ0_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_POLYCOEFF_XYZ0;
      for(int i=0;i<GNSS_SV_POLY_XYZ_0_TH_ORDER_COEFF_MAX_SIZE;i++)
      {
        svPolynomial.polyCoeffXYZ0[i] = gnss_sv_poly_ptr->polyCoeffXYZ0[i];
      }
    }

    if(1 == gnss_sv_poly_ptr->polyCoefXYZN_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_POLYCOEFF_XYZN;
      for(int i=0;i<GNSS_SV_POLY_XYZ_N_TH_ORDER_COEFF_MAX_SIZE;i++)
      {
        svPolynomial.polyCoefXYZN[i] = gnss_sv_poly_ptr->polyCoefXYZN[i];
      }
    }

    if(1 == gnss_sv_poly_ptr->polyCoefClockBias_valid)
    {

      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_POLYCOEFF_OTHER;
      for(int i=0;i<GNSS_SV_POLY_SV_CLKBIAS_COEFF_MAX_SIZE;i++)
      {
        svPolynomial.polyCoefOther[i] = gnss_sv_poly_ptr->polyCoefClockBias[i];
      }
    }

    if(1 == gnss_sv_poly_ptr->ionoDot_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_IONODOT;
      svPolynomial.ionoDot = gnss_sv_poly_ptr->ionoDot;
    }
    if(1 == gnss_sv_poly_ptr->ionoDelay_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_IONODELAY;
      svPolynomial.ionoDelay = gnss_sv_poly_ptr->ionoDelay;
    }

    if(1 == gnss_sv_poly_ptr->sbasIonoDot_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_SBAS_IONODOT;
      svPolynomial.sbasIonoDot = gnss_sv_poly_ptr->sbasIonoDot;
    }
    if(1 == gnss_sv_poly_ptr->sbasIonoDelay_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_SBAS_IONODELAY;
      svPolynomial.sbasIonoDelay = gnss_sv_poly_ptr->sbasIonoDelay;
    }
    if(1 == gnss_sv_poly_ptr->tropoDelay_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_TROPODELAY;
      svPolynomial.tropoDelay = gnss_sv_poly_ptr->tropoDelay;
    }
    if(1 == gnss_sv_poly_ptr->elevation_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_ELEVATION;
      svPolynomial.elevation = gnss_sv_poly_ptr->elevation;
    }
    if(1 == gnss_sv_poly_ptr->elevationDot_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_ELEVATIONDOT;
      svPolynomial.elevationDot = gnss_sv_poly_ptr->elevationDot;
    }
    if(1 == gnss_sv_poly_ptr->elenationUnc_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_ELEVATIONUNC;
      svPolynomial.elevationUnc = gnss_sv_poly_ptr->elenationUnc;
    }
    if(1 == gnss_sv_poly_ptr->velCoef_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_VELO_COEFF;
      for(int i=0;i<GNSS_SV_POLY_VELOCITY_COEF_MAX_SIZE;i++)
      {
        svPolynomial.velCoef[i] = gnss_sv_poly_ptr->velCoef[i];
      }
    }
    if(1 == gnss_sv_poly_ptr->enhancedIOD_valid)
    {
      svPolynomial.is_valid |= ULP_GNSS_SV_POLY_BIT_ENHANCED_IOD;
      svPolynomial.enhancedIOD = gnss_sv_poly_ptr->enhancedIOD;
    }

    LocApiBase::reportSvPolynomial(svPolynomial);

    LOC_LOGV("[SV_POLY_QMI] SV-Id:%d\n", svPolynomial.gnssSvId);
  }
  else
  {
     LOC_LOGV("[SV_POLY]  INVALID SV-Id:%d", svPolynomial.gnssSvId);
  }
} //reportSvPolynomial



/* convert engine state report to loc eng format and send the converted
   report to loc eng */
void LocApiV02 :: reportEngineState (
    const qmiLocEventEngineStateIndMsgT_v02 *engine_state_ptr)
{

  LOC_LOGV("%s:%d]: state = %d\n", __func__, __LINE__,
                 engine_state_ptr->engineState);

  struct MsgUpdateEngineState : public LocMsg {
      LocApiV02* mpLocApiV02;
      bool mEngineOn;
      inline MsgUpdateEngineState(LocApiV02* pLocApiV02, bool engineOn) :
                 LocMsg(), mpLocApiV02(pLocApiV02), mEngineOn(engineOn) {}
      inline virtual void proc() const {
          // If EngineOn is true and InSession is false and Engine is just turned off,
          // then unregister the gps tracking specific event masks
          if (mpLocApiV02->mEngineOn && !mpLocApiV02->mInSession && !mEngineOn) {
              mpLocApiV02->registerEventMask(mpLocApiV02->mQmiMask);
          }
          mpLocApiV02->mEngineOn = mEngineOn;

          if (mEngineOn) {
              // if EngineOn and not InSession, then we have already stopped
              // the fix, so do not send ENGINE_ON
              if (mpLocApiV02->mInSession) {
                  mpLocApiV02->reportStatus(LOC_GPS_STATUS_ENGINE_ON);
                  mpLocApiV02->reportStatus(LOC_GPS_STATUS_SESSION_BEGIN);
              }
          } else {
              mpLocApiV02->reportStatus(LOC_GPS_STATUS_SESSION_END);
              mpLocApiV02->reportStatus(LOC_GPS_STATUS_ENGINE_OFF);
              mpLocApiV02->registerEventMask(mpLocApiV02->mQmiMask);
              for (auto resender : mpLocApiV02->mResenders) {
                  LOC_LOGV("%s:%d]: resend failed command.", __func__, __LINE__);
                  resender();
              }
              mpLocApiV02->mResenders.clear();
          }
      }
  };

  if (engine_state_ptr->engineState == eQMI_LOC_ENGINE_STATE_ON_V02)
  {
    sendMsg(new MsgUpdateEngineState(this, true));
  }
  else if (engine_state_ptr->engineState == eQMI_LOC_ENGINE_STATE_OFF_V02)
  {
    sendMsg(new MsgUpdateEngineState(this, false));
  }
  else
  {
    reportStatus(LOC_GPS_STATUS_NONE);
  }

}

/* convert fix session state report to loc eng format and send the converted
   report to loc eng */
void LocApiV02 :: reportFixSessionState (
    const qmiLocEventFixSessionStateIndMsgT_v02 *fix_session_state_ptr)
{
  LocGpsStatusValue status;
  LOC_LOGD("%s:%d]: state = %d\n", __func__, __LINE__,
                fix_session_state_ptr->sessionState);

  status = LOC_GPS_STATUS_NONE;
  if (fix_session_state_ptr->sessionState == eQMI_LOC_FIX_SESSION_STARTED_V02)
  {
    status = LOC_GPS_STATUS_SESSION_BEGIN;
  }
  else if (fix_session_state_ptr->sessionState
           == eQMI_LOC_FIX_SESSION_FINISHED_V02)
  {
    status = LOC_GPS_STATUS_SESSION_END;
  }
  reportStatus(status);
}

/* convert NMEA report to loc eng format and send the converted
   report to loc eng */
void LocApiV02 :: reportNmea (
  const qmiLocEventNmeaIndMsgT_v02 *nmea_report_ptr)
{
    if (NULL == nmea_report_ptr) {
        return;
    }

    const char* p_nmea = NULL;
    uint32_t q_nmea_len = 0;

    if (nmea_report_ptr->expandedNmea_valid) {
        p_nmea = nmea_report_ptr->expandedNmea;
        q_nmea_len = strlen(nmea_report_ptr->expandedNmea);
        if (q_nmea_len > QMI_LOC_EXPANDED_NMEA_STRING_MAX_LENGTH_V02) {
            q_nmea_len = QMI_LOC_EXPANDED_NMEA_STRING_MAX_LENGTH_V02;
        }
    }
    else
    {
        p_nmea = nmea_report_ptr->nmea;
        q_nmea_len = strlen(nmea_report_ptr->nmea);
        if (q_nmea_len > QMI_LOC_NMEA_STRING_MAX_LENGTH_V02) {
            q_nmea_len = QMI_LOC_NMEA_STRING_MAX_LENGTH_V02;
        }
    }

    if ((NULL != p_nmea) && (q_nmea_len > 0)) {
        LocApiBase::reportNmea(p_nmea, q_nmea_len);
    }
}

/* convert and report an ATL request to loc engine */
void LocApiV02 :: reportAtlRequest(
  const qmiLocEventLocationServerConnectionReqIndMsgT_v02 * server_request_ptr)
{
  uint32_t connHandle = server_request_ptr->connHandle;

  if(server_request_ptr->requestType == eQMI_LOC_SERVER_REQUEST_OPEN_V02 )
  {
    LocAGpsType agpsType = LOC_AGPS_TYPE_ANY;
    LocApnTypeMask apnTypeMask = 0;

    // Check if bearer type indicates WLAN
    if (server_request_ptr->bearerType_valid) {
        switch(server_request_ptr->bearerType) {
        case eQMI_LOC_BEARER_TYPE_WLAN_V02:
            agpsType = LOC_AGPS_TYPE_WIFI;
            break;
        default:
            break;
        }
    }

    // Check the WWAN Type
    if (LOC_AGPS_TYPE_ANY == agpsType) {
        switch(server_request_ptr->wwanType)
        {
        case eQMI_LOC_WWAN_TYPE_INTERNET_V02:
          agpsType = LOC_AGPS_TYPE_WWAN_ANY;
          break;
        case eQMI_LOC_WWAN_TYPE_AGNSS_V02:
          agpsType = LOC_AGPS_TYPE_SUPL;
          break;
        case eQMI_LOC_WWAN_TYPE_AGNSS_EMERGENCY_V02:
          agpsType = LOC_AGPS_TYPE_SUPL_ES;
          break;
        default:
          agpsType = LOC_AGPS_TYPE_WWAN_ANY;
          break;
        }
    }

    if (server_request_ptr->apnTypeMask_valid) {
        apnTypeMask = convertQmiLocApnTypeMask(server_request_ptr->apnTypeMask);
    }
    LOC_LOGd("handle=%d agpsType=0x%X apnTypeMask=0x%X",
        connHandle, agpsType, apnTypeMask);
    requestATL(connHandle, agpsType, apnTypeMask);
  }
  // service the ATL close request
  else if (server_request_ptr->requestType == eQMI_LOC_SERVER_REQUEST_CLOSE_V02)
  {
    releaseATL(connHandle);
  }
}

/* conver the NI report to loc eng format and send t loc engine */
void LocApiV02 :: reportNiRequest(
    const qmiLocEventNiNotifyVerifyReqIndMsgT_v02 *ni_req_ptr)
{
  GnssNiNotification notif = {};
  notif.messageEncoding = GNSS_NI_ENCODING_TYPE_NONE ;
  notif.requestorEncoding = GNSS_NI_ENCODING_TYPE_NONE;
  notif.timeoutResponse = GNSS_NI_RESPONSE_NO_RESPONSE;
  notif.timeout = LOC_NI_NO_RESPONSE_TIME;

  /*Handle Vx request */
  if(ni_req_ptr->NiVxInd_valid == 1)
  {
     const qmiLocNiVxNotifyVerifyStructT_v02 *vx_req = &(ni_req_ptr->NiVxInd);

     notif.type = GNSS_NI_TYPE_VOICE;

     // Requestor ID, the requestor id recieved is NULL terminated
     hexcode(notif.requestor, sizeof notif.requestor,
             (char *)vx_req->requestorId, vx_req->requestorId_len );
  }

  /* Handle UMTS CP request*/
  else if(ni_req_ptr->NiUmtsCpInd_valid == 1)
  {
    const qmiLocNiUmtsCpNotifyVerifyStructT_v02 *umts_cp_req =
       &ni_req_ptr->NiUmtsCpInd;

    notif.type = GNSS_NI_TYPE_CONTROL_PLANE;

    /* notificationText should always be a NULL terminated string */
    hexcode(notif.message, sizeof notif.message,
            (char *)umts_cp_req->notificationText,
            umts_cp_req->notificationText_len);

    /* Store requestor ID */
    hexcode(notif.requestor, sizeof(notif.requestor),
            (char *)umts_cp_req->requestorId.codedString,
            umts_cp_req->requestorId.codedString_len);

   /* convert encodings */
    notif.messageEncoding = convertNiEncoding(umts_cp_req->dataCodingScheme);

    notif.requestorEncoding =
      convertNiEncoding(umts_cp_req->requestorId.dataCodingScheme);

    /* LCS address (using extras field) */
    if ( umts_cp_req->clientAddress_len != 0)
    {
      char lcs_addr[32]; // Decoded LCS address for UMTS CP NI

      // Copy LCS Address into notif.extras in the format: Address = 012345
      strlcat(notif.extras, LOC_NI_NOTIF_KEY_ADDRESS, sizeof (notif.extras));
      strlcat(notif.extras, " = ", sizeof notif.extras);
      int addr_len = 0;
      const char *address_source = NULL;
      address_source = (char *)umts_cp_req->clientAddress;
      // client Address is always NULL terminated
      addr_len = decodeAddress(lcs_addr, sizeof(lcs_addr), address_source,
                               umts_cp_req->clientAddress_len);

      // The address is ASCII string
      if (addr_len)
      {
        strlcat(notif.extras, lcs_addr, sizeof notif.extras);
      }
    }

  }
  else if(ni_req_ptr->NiSuplInd_valid == 1)
  {
    const qmiLocNiSuplNotifyVerifyStructT_v02 *supl_req =
      &ni_req_ptr->NiSuplInd;

    notif.type = GNSS_NI_TYPE_SUPL;

    // Client name
    if (supl_req->valid_flags & QMI_LOC_SUPL_CLIENT_NAME_MASK_V02)
    {
      hexcode(notif.message, sizeof(notif.message),
              (char *)supl_req->clientName.formattedString,
              supl_req->clientName.formattedString_len);
      LOC_LOGV("%s:%d]: SUPL NI: client_name: %s \n", __func__, __LINE__,
          notif.message);
    }
    else
    {
      LOC_LOGV("%s:%d]: SUPL NI: client_name not present.",
          __func__, __LINE__);
    }

    // Requestor ID
    if (supl_req->valid_flags & QMI_LOC_SUPL_REQUESTOR_ID_MASK_V02)
    {
      hexcode(notif.requestor, sizeof notif.requestor,
              (char*)supl_req->requestorId.formattedString,
              supl_req->requestorId.formattedString_len );

      LOC_LOGV("%s:%d]: SUPL NI: requestor: %s \n", __func__, __LINE__,
          notif.requestor);
    }
    else
    {
      LOC_LOGV("%s:%d]: SUPL NI: requestor not present.",
          __func__, __LINE__);
    }

    // Encoding type
    if (supl_req->valid_flags & QMI_LOC_SUPL_DATA_CODING_SCHEME_MASK_V02)
    {
      notif.messageEncoding = convertNiEncoding(supl_req->dataCodingScheme);

      notif.requestorEncoding = convertNiEncoding(supl_req->dataCodingScheme);
    }
    else
    {
      notif.messageEncoding = notif.requestorEncoding = GNSS_NI_ENCODING_TYPE_NONE;
    }

    // ES SUPL
    if(ni_req_ptr->suplEmergencyNotification_valid ==1)
    {
        const qmiLocEmergencyNotificationStructT_v02 *supl_emergency_request =
        &ni_req_ptr->suplEmergencyNotification;

        notif.type = GNSS_NI_TYPE_EMERGENCY_SUPL;
    }

  } //ni_req_ptr->NiSuplInd_valid == 1
  else
  {
    LOC_LOGE("%s:%d]: unknown request event \n",__func__, __LINE__);
    return;
  }

  // Set default_response & notify_flags
  convertNiNotifyVerifyType(&notif, ni_req_ptr->notificationType);

  qmiLocEventNiNotifyVerifyReqIndMsgT_v02 *ni_req_copy_ptr =
    (qmiLocEventNiNotifyVerifyReqIndMsgT_v02 *)malloc(sizeof(*ni_req_copy_ptr));

  if( NULL != ni_req_copy_ptr)
  {
    memcpy(ni_req_copy_ptr, ni_req_ptr, sizeof(*ni_req_copy_ptr));

    requestNiNotify(notif, (const void*)ni_req_copy_ptr);
  }
  else
  {
    LOC_LOGE("%s:%d]: Error copying NI request\n", __func__, __LINE__);
  }

}

/* If Confidence value is less than 68%, then scale the accuracy value to
   68%.confidence.*/
void LocApiV02 :: scaleAccuracyTo68PercentConfidence(
                                                const uint8_t confidenceValue,
                                                LocGpsLocation &gpsLocation,
                                                const bool isCircularUnc)
{
  if (confidenceValue < 68)
  {
    // Circular uncertainty is at 63%.confidence. Scale factor should be
    // 1.072(from 63% -> 68%)
    uint8_t realConfidence = (isCircularUnc) ? 63:confidenceValue;
    // get scaling value based on 2D% confidence scaling table
    for (uint8_t iter = 0; iter < CONF_SCALER_ARRAY_MAX; iter++)
    {
      if (realConfidence <= confScalers[iter].confidence)
      {
        LOC_LOGD("Confidence: %d, Scaler value:%f",
                realConfidence,confScalers[iter].scaler_to_68);
        gpsLocation.accuracy *= confScalers[iter].scaler_to_68;
        break;
      }
    }
  }
}

/* Report the Xtra Server Url from the modem to HAL*/
void LocApiV02 :: reportXtraServerUrl(
                const qmiLocEventInjectPredictedOrbitsReqIndMsgT_v02*
                server_request_ptr)
{

  if (server_request_ptr->serverList.serverList_len == 1)
  {
    reportXtraServer(server_request_ptr->serverList.serverList[0].serverUrl,
                     "",
                     "",
                     QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
  }
  else if (server_request_ptr->serverList.serverList_len == 2)
  {
    reportXtraServer(server_request_ptr->serverList.serverList[0].serverUrl,
                     server_request_ptr->serverList.serverList[1].serverUrl,
                     "",
                     QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
  }
  else
  {
    reportXtraServer(server_request_ptr->serverList.serverList[0].serverUrl,
                     server_request_ptr->serverList.serverList[1].serverUrl,
                     server_request_ptr->serverList.serverList[2].serverUrl,
                     QMI_LOC_MAX_SERVER_ADDR_LENGTH_V02);
  }

}

/* convert Ni Encoding type from QMI_LOC to loc eng format */
GnssNiEncodingType LocApiV02 ::convertNiEncoding(
  qmiLocNiDataCodingSchemeEnumT_v02 loc_encoding)
{
   GnssNiEncodingType enc = GNSS_NI_ENCODING_TYPE_NONE;

   switch (loc_encoding)
   {
     case eQMI_LOC_NI_SUPL_UTF8_V02:
       enc = GNSS_NI_ENCODING_TYPE_UTF8;
       break;
     case eQMI_LOC_NI_SUPL_UCS2_V02:
       enc = GNSS_NI_ENCODING_TYPE_UCS2;
       break;
     case eQMI_LOC_NI_SUPL_GSM_DEFAULT_V02:
       enc = GNSS_NI_ENCODING_TYPE_GSM_DEFAULT;
       break;
     case eQMI_LOC_NI_SS_LANGUAGE_UNSPEC_V02:
       enc = GNSS_NI_ENCODING_TYPE_GSM_DEFAULT; // SS_LANGUAGE_UNSPEC = GSM
       break;
     default:
       break;
   }

   return enc;
}

/*convert NI notify verify type from QMI LOC to loc eng format*/
bool LocApiV02 :: convertNiNotifyVerifyType (
  GnssNiNotification *notif,
  qmiLocNiNotifyVerifyEnumT_v02 notif_priv)
{
  switch (notif_priv)
   {
   case eQMI_LOC_NI_USER_NO_NOTIFY_NO_VERIFY_V02:
      notif->options = 0;
      break;

   case eQMI_LOC_NI_USER_NOTIFY_ONLY_V02:
      notif->options = GNSS_NI_OPTIONS_NOTIFICATION_BIT;
      break;

   case eQMI_LOC_NI_USER_NOTIFY_VERIFY_ALLOW_NO_RESP_V02:
      notif->options = GNSS_NI_OPTIONS_NOTIFICATION_BIT | GNSS_NI_OPTIONS_VERIFICATION_BIT;
      notif->timeoutResponse = GNSS_NI_RESPONSE_ACCEPT;
      break;

   case eQMI_LOC_NI_USER_NOTIFY_VERIFY_NOT_ALLOW_NO_RESP_V02:
      notif->options = GNSS_NI_OPTIONS_NOTIFICATION_BIT | GNSS_NI_OPTIONS_VERIFICATION_BIT;
      notif->timeoutResponse = GNSS_NI_RESPONSE_DENY;
      break;

   case eQMI_LOC_NI_USER_NOTIFY_VERIFY_PRIVACY_OVERRIDE_V02:
      notif->options = GNSS_NI_OPTIONS_PRIVACY_OVERRIDE_BIT;
      break;

   default:
      return false;
   }

   return true;
}

/* convert and report GNSS measurement data to loc eng */
void LocApiV02 :: reportGnssMeasurementData(
  const qmiLocEventGnssSvMeasInfoIndMsgT_v02& gnss_measurement_report_ptr)
{
    LOC_LOGV ("%s:%d]: entering\n", __func__, __LINE__);

    static GnssMeasurementsNotification measurementsNotify = {};

    int svMeasurement_len = 0;
    static int meas_index = 0;
    static bool bGPSreceived = false;
    static int msInWeek = -1;
    bool bAgcIsPresent = false;

    LOC_LOGd("SeqNum: %d, MaxMsgNum: %d",
        gnss_measurement_report_ptr.seqNum,
        gnss_measurement_report_ptr.maxMessageNum);

    if (gnss_measurement_report_ptr.seqNum > gnss_measurement_report_ptr.maxMessageNum) {
        LOC_LOGe("%s:%d]: Invalid seqNum, do not proceed");
        return;
    }

    if (1 == gnss_measurement_report_ptr.seqNum)
    {
        meas_index = 0;
        bGPSreceived = false;
        msInWeek = -1;
        memset(&measurementsNotify, 0, sizeof(GnssMeasurementsNotification));
        measurementsNotify.size = sizeof(GnssMeasurementsNotification);
    }

    // number of measurements
    if (gnss_measurement_report_ptr.svMeasurement_valid) {
        svMeasurement_len =
            gnss_measurement_report_ptr.svMeasurement_len;
        measurementsNotify.count += svMeasurement_len;
        if (measurementsNotify.count > GNSS_MEASUREMENTS_MAX) {
            LOC_LOGv("count should not be > 64, limiting it");
            measurementsNotify.count = GNSS_MEASUREMENTS_MAX;
        }
        LOC_LOGv("there are %d SV measurements now, total=%zu\n",
                  svMeasurement_len,
                  measurementsNotify.count);
        if (svMeasurement_len != 0) {
            // the array of measurements
            LOC_LOGv("Measurements received for GNSS system %d",
                     gnss_measurement_report_ptr.system);

            for (int index = 0; index < svMeasurement_len; index++) {
                LOC_LOGv("index=%d meas_index=%d", index, meas_index);
                if (convertGnssMeasurements(measurementsNotify.measurements[meas_index],
                    gnss_measurement_report_ptr,
                    index)) {
                    bAgcIsPresent = true;
                }
                meas_index++;
                if (GNSS_MEASUREMENTS_MAX == meas_index) {
                    LOC_LOGv("meas_index cannot exceed 64, limiting it");
                    break;
                }
            }
        }
    } else {
        LOC_LOGv("there is no valid GNSS measurement for system %d",
                 gnss_measurement_report_ptr.system);
    }

    // the GPS clock time reading
    if (eQMI_LOC_SV_SYSTEM_GPS_V02 == gnss_measurement_report_ptr.system) {
        bGPSreceived = true;
        msInWeek = convertGnssClock(measurementsNotify.clock,
                                gnss_measurement_report_ptr);
    }
    if (gnss_measurement_report_ptr.maxMessageNum == gnss_measurement_report_ptr.seqNum
            && meas_index > 0 && true == bGPSreceived) {
        // calling the base
        if (bAgcIsPresent) {
            /* If we can get AGC from QMI LOC there is no need to get it from NMEA */
            msInWeek = -1;
        }
        LocApiBase::reportGnssMeasurementData(measurementsNotify, msInWeek);
    }
}

/* convert and report ODCPI request */
void LocApiV02::requestOdcpi(const qmiLocEventWifiReqIndMsgT_v02& qmiReq)
{
    LOC_LOGv("ODCPI Request: requestType %d", qmiReq.requestType);

    OdcpiRequestInfo req = {};
    req.size = sizeof(OdcpiRequestInfo);

    if (eQMI_LOC_WIFI_START_PERIODIC_HI_FREQ_FIXES_V02 == qmiReq.requestType ||
            eQMI_LOC_WIFI_START_PERIODIC_KEEP_WARM_V02 == qmiReq.requestType) {
        req.type = ODCPI_REQUEST_TYPE_START;
    } else if (eQMI_LOC_WIFI_STOP_PERIODIC_FIXES_V02 == qmiReq.requestType){
        req.type = ODCPI_REQUEST_TYPE_STOP;
    } else {
        LOC_LOGe("Invalid request type");
        return;
    }

    if (qmiReq.e911Mode_valid) {
        req.isEmergencyMode = qmiReq.e911Mode == 1 ? true : false;
    }

    if (qmiReq.tbfInMs_valid) {
        req.tbfMillis = qmiReq.tbfInMs;
    }

    LocApiBase::requestOdcpi(req);
}

void LocApiV02::wifiStatusInformSync()
{
    qmiLocNotifyWifiStatusReqMsgT_v02 wifiStatusReq;
    memset(&wifiStatusReq, 0, sizeof(wifiStatusReq));
    wifiStatusReq.wifiStatus = eQMI_LOC_WIFI_STATUS_AVAILABLE_V02;

    LOC_LOGv("Informing wifi status available.");
    LOC_SEND_SYNC_REQ(NotifyWifiStatus, NOTIFY_WIFI_STATUS, wifiStatusReq);
}

#define FIRST_BDS_D2_SV_PRN 1
#define LAST_BDS_D2_SV_PRN  5
#define IS_BDS_GEO_SV(svId, gnssType) ( ((gnssType == GNSS_SV_TYPE_BEIDOU) && \
                                        (svId <= LAST_BDS_D2_SV_PRN) && \
                                        (svId >= FIRST_BDS_D2_SV_PRN)) ? true : false )

/*convert GnssMeasurement type from QMI LOC to loc eng format*/
bool LocApiV02 :: convertGnssMeasurements (GnssMeasurementsData& measurementData,
    const qmiLocEventGnssSvMeasInfoIndMsgT_v02& gnss_measurement_report_ptr,
    int index)
{
    uint8_t gloFrequency = 0;
    bool bAgcIsPresent = false;

    LOC_LOGV ("%s:%d]: entering\n", __func__, __LINE__);

    qmiLocSVMeasurementStructT_v02 gnss_measurement_info;

    gnss_measurement_info = gnss_measurement_report_ptr.svMeasurement[index];

    // size
    measurementData.size = sizeof(GnssMeasurementsData);

    // flag initiation
    measurementData.flags = 0;

    // constellation and svid
    switch (gnss_measurement_report_ptr.system)
    {
        case eQMI_LOC_SV_SYSTEM_GPS_V02:
            measurementData.svType = GNSS_SV_TYPE_GPS;
            measurementData.svId = gnss_measurement_info.gnssSvId;
            break;

        case eQMI_LOC_SV_SYSTEM_GALILEO_V02:
            measurementData.svType = GNSS_SV_TYPE_GALILEO;
            measurementData.svId = gnss_measurement_info.gnssSvId + 1 - GAL_SV_PRN_MIN;
            break;

        case eQMI_LOC_SV_SYSTEM_SBAS_V02:
            measurementData.svType = GNSS_SV_TYPE_SBAS;
            measurementData.svId = gnss_measurement_info.gnssSvId;
            break;

        case eQMI_LOC_SV_SYSTEM_GLONASS_V02:
            measurementData.svType = GNSS_SV_TYPE_GLONASS;
            if (gnss_measurement_info.gnssSvId != 255) // OSN is known
            {
                measurementData.svId = gnss_measurement_info.gnssSvId + 1 - GLO_SV_PRN_MIN;
            }
            else // OSN is not known, report FCN
            {
                measurementData.svId = gnss_measurement_info.gloFrequency + 92;
            }
            gloFrequency = gnss_measurement_info.gloFrequency;
            break;

        case eQMI_LOC_SV_SYSTEM_BDS_V02:
            measurementData.svType = GNSS_SV_TYPE_BEIDOU;
            measurementData.svId = gnss_measurement_info.gnssSvId + 1 - BDS_SV_PRN_MIN;
            break;

        case eQMI_LOC_SV_SYSTEM_QZSS_V02:
            measurementData.svType = GNSS_SV_TYPE_QZSS;
            measurementData.svId = gnss_measurement_info.gnssSvId;
            break;

        default:
            measurementData.svType = GNSS_SV_TYPE_UNKNOWN;
            measurementData.svId = gnss_measurement_info.gnssSvId;
            break;
    }

    // time_offset_ns
    if (0 != gnss_measurement_info.measLatency)
    {
        LOC_LOGV("%s:%d]: measLatency is not 0\n", __func__, __LINE__);
    }
    measurementData.timeOffsetNs = 0.0;

    // stateMask & receivedSvTimeNs & received_gps_tow_uncertainty_ns
    uint64_t validMask = gnss_measurement_info.measurementStatus &
                         gnss_measurement_info.validMeasStatusMask;
    uint64_t bitSynMask = QMI_LOC_MASK_MEAS_STATUS_BE_CONFIRM_V02 |
                          QMI_LOC_MASK_MEAS_STATUS_SB_VALID_V02;
    double gpsTowUncNs = (double)gnss_measurement_info.svTimeSpeed.svTimeUncMs * 1e6;
    bool isGloTimeValid = false;

    if ((GNSS_SV_TYPE_GLONASS == measurementData.svType) &&
        (gnss_measurement_report_ptr.gloTime_valid) &&
        (gnss_measurement_report_ptr.gloTime.gloFourYear != 255) && /* 255 is unknown */
        (gnss_measurement_report_ptr.gloTime.gloDays != 65535)) { /* 65535 is unknown */
        isGloTimeValid = true;
    }

    uint64_t galSVstateMask = 0;

    if (GNSS_SV_TYPE_GALILEO == measurementData.svType) {
        galSVstateMask = GNSS_MEASUREMENTS_STATE_GAL_E1BC_CODE_LOCK_BIT;

        if (gnss_measurement_info.measurementStatus &
                QMI_LOC_MASK_MEAS_STATUS_100MS_STAT_BIT_VALID_V02) {
            galSVstateMask |= GNSS_MEASUREMENTS_STATE_GAL_E1C_2ND_CODE_LOCK_BIT;
        }
        if (gnss_measurement_info.measurementStatus &
                QMI_LOC_MASK_MEAS_STATUS_2S_STAT_BIT_VALID_V02) {
            galSVstateMask |= GNSS_MEASUREMENTS_STATE_GAL_E1B_PAGE_SYNC_BIT;
        }
    }

    if (validMask & QMI_LOC_MASK_MEAS_STATUS_MS_VALID_V02) {
        /* sub-frame decode & TOW decode */
        measurementData.stateMask = GNSS_MEASUREMENTS_STATE_SUBFRAME_SYNC_BIT |
                                    GNSS_MEASUREMENTS_STATE_TOW_DECODED_BIT |
                                    GNSS_MEASUREMENTS_STATE_BIT_SYNC_BIT |
                                    GNSS_MEASUREMENTS_STATE_CODE_LOCK_BIT;
        if (true == isGloTimeValid) {
            measurementData.stateMask |= (GNSS_MEASUREMENTS_STATE_GLO_STRING_SYNC_BIT |
                GNSS_MEASUREMENTS_STATE_GLO_TOD_DECODED_BIT);
        }
        measurementData.stateMask |= galSVstateMask;

        if (IS_BDS_GEO_SV(measurementData.svId, measurementData.svType)) {
            /* BDS_GEO SV transmitting D2 signal */
            measurementData.stateMask |= (GNSS_MEASUREMENTS_STATE_BDS_D2_BIT_SYNC_BIT |
                GNSS_MEASUREMENTS_STATE_BDS_D2_SUBFRAME_SYNC_BIT);
        }
        measurementData.receivedSvTimeNs =
            (int64_t)(((double)gnss_measurement_info.svTimeSpeed.svTimeMs +
             (double)gnss_measurement_info.svTimeSpeed.svTimeSubMs) * 1e6);

        measurementData.receivedSvTimeUncertaintyNs = (int64_t)gpsTowUncNs;

    } else if ((validMask & bitSynMask) == bitSynMask) {
        /* bit sync */
        measurementData.stateMask = GNSS_MEASUREMENTS_STATE_BIT_SYNC_BIT |
                                    GNSS_MEASUREMENTS_STATE_CODE_LOCK_BIT;
        measurementData.stateMask |= galSVstateMask;
        measurementData.receivedSvTimeNs =
            (int64_t)(fmod(((double)gnss_measurement_info.svTimeSpeed.svTimeMs +
                  (double)gnss_measurement_info.svTimeSpeed.svTimeSubMs), 20) * 1e6);
        measurementData.receivedSvTimeUncertaintyNs = (int64_t)gpsTowUncNs;

    } else if (validMask & QMI_LOC_MASK_MEAS_STATUS_SM_VALID_V02) {
        /* code lock */
        measurementData.stateMask = GNSS_MEASUREMENTS_STATE_CODE_LOCK_BIT;
        measurementData.stateMask |= galSVstateMask;
        measurementData.receivedSvTimeNs =
             (int64_t)((double)gnss_measurement_info.svTimeSpeed.svTimeSubMs * 1e6);
        measurementData.receivedSvTimeUncertaintyNs = (int64_t)gpsTowUncNs;

    } else {
        /* by default */
        measurementData.stateMask = GNSS_MEASUREMENTS_STATE_UNKNOWN_BIT;
        measurementData.receivedSvTimeNs = 0;
        measurementData.receivedSvTimeUncertaintyNs = 0;
    }

    // carrierToNoiseDbHz
    measurementData.carrierToNoiseDbHz = gnss_measurement_info.CNo/10.0;

    if (QMI_LOC_MASK_MEAS_STATUS_VELOCITY_FINE_V02 == (gnss_measurement_info.measurementStatus & QMI_LOC_MASK_MEAS_STATUS_VELOCITY_FINE_V02))
    {
        LOC_LOGV ("%s:%d]: FINE mS=0x%4" PRIX64 " fS=%f fSU=%f dS=%f dSU=%f\n", __func__, __LINE__,
        gnss_measurement_info.measurementStatus,
        gnss_measurement_info.fineSpeed, gnss_measurement_info.fineSpeedUnc,
        gnss_measurement_info.svTimeSpeed.dopplerShift, gnss_measurement_info.svTimeSpeed.dopplerShiftUnc);
        // pseudorangeRateMps
        measurementData.pseudorangeRateMps = gnss_measurement_info.fineSpeed;

        // pseudorangeRateUncertaintyMps
        measurementData.pseudorangeRateUncertaintyMps = gnss_measurement_info.fineSpeedUnc;
    }
    else
    {
        LOC_LOGV ("%s:%d]: COARSE mS=0x%4" PRIX64 " fS=%f fSU=%f dS=%f dSU=%f\n", __func__, __LINE__,
        gnss_measurement_info.measurementStatus,
        gnss_measurement_info.fineSpeed, gnss_measurement_info.fineSpeedUnc,
        gnss_measurement_info.svTimeSpeed.dopplerShift, gnss_measurement_info.svTimeSpeed.dopplerShiftUnc);
        // pseudorangeRateMps
        measurementData.pseudorangeRateMps = gnss_measurement_info.svTimeSpeed.dopplerShift;

        // pseudorangeRateUncertaintyMps
        measurementData.pseudorangeRateUncertaintyMps = gnss_measurement_info.svTimeSpeed.dopplerShiftUnc;
    }

    // accumulated_delta_range_state
    measurementData.adrStateMask = GNSS_MEASUREMENTS_ACCUMULATED_DELTA_RANGE_STATE_UNKNOWN;

    // carrier frequency
    if (gnss_measurement_report_ptr.gnssSignalType_valid) {
        LOC_LOGv("gloFrequency = 0x%X, sigType=0x%X",
                 gloFrequency, gnss_measurement_report_ptr.gnssSignalType);
        measurementData.carrierFrequencyHz = convertSignalTypeToCarrierFrequency(
                gnss_measurement_report_ptr.gnssSignalType, gloFrequency);
        measurementData.flags |= GNSS_MEASUREMENTS_DATA_CARRIER_FREQUENCY_BIT;
    }
    else {
        LOC_LOGv("gnss_measurement_report_ptr.gnssSignalType_valid = 0");
    }
    // multipath_indicator
    measurementData.multipathIndicator = GNSS_MEASUREMENTS_MULTIPATH_INDICATOR_UNKNOWN;

    // AGC
    if (gnss_measurement_report_ptr.jammerIndicator_valid) {
        if (GNSS_INVALID_JAMMER_IND !=
            gnss_measurement_report_ptr.jammerIndicator.agcMetricDb) {
            measurementData.agcLevelDb =
                (double)gnss_measurement_report_ptr.jammerIndicator.agcMetricDb / 100.0;
            measurementData.flags |= GNSS_MEASUREMENTS_DATA_AUTOMATIC_GAIN_CONTROL_BIT;
        }
        LOC_LOGv("AGC is valid: agcMetricDb = 0x%X bpMetricDb = 0x%X",
                 gnss_measurement_report_ptr.jammerIndicator.agcMetricDb,
                 gnss_measurement_report_ptr.jammerIndicator.bpMetricDb);
        bAgcIsPresent = true;
    }
    else {
        LOC_LOGv("AGC is invalid");
        bAgcIsPresent = false;
    }

    LOC_LOGV(" %s:%d]: GNSS measurement raw data received from modem:"
             " Input => gnssSvId=%d CNo=%d measurementStatus=0x%04x%04x"
             "  dopplerShift=%f dopplerShiftUnc=%f fineSpeed=%f fineSpeedUnc=%f"
             "  svTimeMs=%u svTimeSubMs=%f svTimeUncMs=%f"
             "  svStatus=0x%02x validMeasStatusMask=0x%04x%04x"
             " GNSS measurement data after conversion:"
             " Output => size=%zu svid=%d time_offset_ns=%f state=%d"
             "  received_sv_time_in_ns=%" PRIu64 " received_sv_time_uncertainty_in_ns=%" PRIu64
             " c_n0_dbhz=%g"
             "  pseudorange_rate_mps=%g pseudorange_rate_uncertainty_mps=%g"
             " carrierFrequencyHz=%.2f",
            __func__, __LINE__,
             gnss_measurement_info.gnssSvId,                                    // %d
             gnss_measurement_info.CNo,                                         // %d
             (uint32_t)(gnss_measurement_info.measurementStatus >> 32),         // %04x Upper 32
             (uint32_t)(gnss_measurement_info.measurementStatus & 0xFFFFFFFF),  // %04x Lower 32
             gnss_measurement_info.svTimeSpeed.dopplerShift,                    // %f
             gnss_measurement_info.svTimeSpeed.dopplerShiftUnc,                 // %f
             gnss_measurement_info.fineSpeed,                                   // %f
             gnss_measurement_info.fineSpeedUnc,                                // %f
             gnss_measurement_info.svTimeSpeed.svTimeMs,                        // %u
             gnss_measurement_info.svTimeSpeed.svTimeSubMs,                     // %f
             gnss_measurement_info.svTimeSpeed.svTimeUncMs,                     // %f
             (uint32_t)(gnss_measurement_info.svStatus),                        // %02x
             (uint32_t)(gnss_measurement_info.validMeasStatusMask >> 32),       // %04x Upper 32
             (uint32_t)(gnss_measurement_info.validMeasStatusMask & 0xFFFFFFFF),// %04x Lower 32
             measurementData.size,                                              // %zu
             measurementData.svId,                                              // %d
             measurementData.timeOffsetNs,                                      // %f
             measurementData.stateMask,                                         // %d
             measurementData.receivedSvTimeNs,                                  // %PRIu64
             measurementData.receivedSvTimeUncertaintyNs,                       // %PRIu64
             measurementData.carrierToNoiseDbHz,                                // %g
             measurementData.pseudorangeRateMps,                                // %g
             measurementData.pseudorangeRateUncertaintyMps,                     // %g
             measurementData.carrierFrequencyHz);                               // %f

    return bAgcIsPresent;
}

/*convert GnssMeasurementsClock type from QMI LOC to loc eng format*/
int LocApiV02 :: convertGnssClock (GnssMeasurementsClock& clock,
    const qmiLocEventGnssSvMeasInfoIndMsgT_v02& gnss_measurement_info)
{
    static uint32_t oldRefFCount = 0;
    static uint32_t newRefFCount = 0;
    static uint32_t oldDiscCount = 0;
    static uint32_t newDiscCount = 0;
    static uint32_t localDiscCount = 0;
    int msInWeek = -1;

    LOC_LOGV ("%s:%d]: entering\n", __func__, __LINE__);

    // size
    clock.size = sizeof(GnssMeasurementsClock);

    // flag initiation
    GnssMeasurementsClockFlagsMask flags = 0;

    if (gnss_measurement_info.systemTimeExt_valid &&
        gnss_measurement_info.numClockResets_valid) {
        newRefFCount = gnss_measurement_info.systemTimeExt.refFCount;
        newDiscCount = gnss_measurement_info.numClockResets;
        if ((true == mMeasurementsStarted) ||
            (oldDiscCount != newDiscCount) ||
            (newRefFCount <= oldRefFCount))
        {
            if (true == mMeasurementsStarted)
            {
                mMeasurementsStarted = false;
            }
            localDiscCount++;
        }
        oldDiscCount = newDiscCount;
        oldRefFCount = newRefFCount;

        // timeNs & timeUncertaintyNs
        clock.timeNs = (int64_t)gnss_measurement_info.systemTimeExt.refFCount * 1e6;
        clock.hwClockDiscontinuityCount = localDiscCount;
        clock.timeUncertaintyNs = 0.0;

        msInWeek = (int)gnss_measurement_info.systemTime.systemMsec;
        if (gnss_measurement_info.systemTime_valid) {
            uint16_t systemWeek = gnss_measurement_info.systemTime.systemWeek;
            uint32_t systemMsec = gnss_measurement_info.systemTime.systemMsec;
            float sysClkBias = gnss_measurement_info.systemTime.systemClkTimeBias;
            float sysClkUncMs = gnss_measurement_info.systemTime.systemClkTimeUncMs;
            bool isTimeValid = (sysClkUncMs <= 16.0f); // 16ms

            if (systemWeek != C_GPS_WEEK_UNKNOWN && isTimeValid) {
                // fullBiasNs, biasNs & biasUncertaintyNs
                int64_t totalMs = ((int64_t)systemWeek) *
                                  ((int64_t)WEEK_MSECS) + ((int64_t)systemMsec);
                int64_t gpsTimeNs = totalMs * 1000000 - (int64_t)(sysClkBias * 1e6);
                clock.fullBiasNs = clock.timeNs - gpsTimeNs;
                clock.biasNs = sysClkBias * 1e6 - (double)((int64_t)(sysClkBias * 1e6));
                clock.biasUncertaintyNs = (double)sysClkUncMs * 1e6;
                flags |= (GNSS_MEASUREMENTS_CLOCK_FLAGS_FULL_BIAS_BIT |
                          GNSS_MEASUREMENTS_CLOCK_FLAGS_BIAS_BIT |
                          GNSS_MEASUREMENTS_CLOCK_FLAGS_BIAS_UNCERTAINTY_BIT);
            }
        }
    }

    // driftNsps & driftUncertaintyNsps
    if (gnss_measurement_info.rcvrClockFrequencyInfo_valid)
    {
        double driftMPS = gnss_measurement_info.rcvrClockFrequencyInfo.clockDrift;
        double driftUncMPS = gnss_measurement_info.rcvrClockFrequencyInfo.clockDriftUnc;

        clock.driftNsps = driftMPS * MPS_TO_NSPS;
        clock.driftUncertaintyNsps = driftUncMPS * MPS_TO_NSPS;

        flags |= (GNSS_MEASUREMENTS_CLOCK_FLAGS_DRIFT_BIT |
                  GNSS_MEASUREMENTS_CLOCK_FLAGS_DRIFT_UNCERTAINTY_BIT);
    }

    clock.flags = flags;

    LOC_LOGV(" %s:%d]: GNSS measurement clock data received from modem: \n", __func__, __LINE__);
    LOC_LOGV(" Input => systemTime_valid=%d systemTimeExt_valid=%d numClockResets_valid=%d\n",
             gnss_measurement_info.systemTime_valid,                      // %d
             gnss_measurement_info.systemTimeExt_valid,                   // %d
        gnss_measurement_info.numClockResets_valid);                 // %d

    LOC_LOGV("  systemWeek=%d systemMsec=%d systemClkTimeBias=%f\n",
             gnss_measurement_info.systemTime.systemWeek,                 // %d
             gnss_measurement_info.systemTime.systemMsec,                 // %d
        gnss_measurement_info.systemTime.systemClkTimeBias);         // %f

    LOC_LOGV("  systemClkTimeUncMs=%f refFCount=%d numClockResets=%d\n",
             gnss_measurement_info.systemTime.systemClkTimeUncMs,         // %f
        gnss_measurement_info.systemTimeExt.refFCount,               // %d
        gnss_measurement_info.numClockResets);                       // %d

    LOC_LOGV("  clockDrift=%f clockDriftUnc=%f\n",
        gnss_measurement_info.rcvrClockFrequencyInfo.clockDrift,     // %f
        gnss_measurement_info.rcvrClockFrequencyInfo.clockDriftUnc); // %f


    LOC_LOGV(" %s:%d]: GNSS measurement clock after conversion: \n", __func__, __LINE__);
    LOC_LOGV(" Output => timeNs=%" PRId64 "\n",
        clock.timeNs);                       // %PRId64

    LOC_LOGV("  fullBiasNs=%" PRId64 " biasNs=%g bias_uncertainty_ns=%g\n",
        clock.fullBiasNs,                    // %PRId64
        clock.biasNs,                        // %g
        clock.biasUncertaintyNs);            // %g

    LOC_LOGV("  driftNsps=%g drift_uncertainty_nsps=%g\n",
        clock.driftNsps,                     // %g
        clock.driftUncertaintyNsps);         // %g

    LOC_LOGV("  hw_clock_discontinuity_count=%d flags=0x%04x\n",
        clock.hwClockDiscontinuityCount,     // %lld
        clock.flags);                        // %04x

    return msInWeek;
}

/* event callback registered with the loc_api v02 interface */
void LocApiV02 :: eventCb(locClientHandleType /*clientHandle*/,
  uint32_t eventId, locClientEventIndUnionType eventPayload)
{
  LOC_LOGd("event id = 0x%X", eventId);

  switch(eventId)
  {
    //Position Report
    case QMI_LOC_EVENT_POSITION_REPORT_IND_V02:
      reportPosition(eventPayload.pPositionReportEvent);
      break;

    // Satellite report
    case QMI_LOC_EVENT_GNSS_SV_INFO_IND_V02:
      reportSv(eventPayload.pGnssSvInfoReportEvent);
      break;

    // Status report
    case QMI_LOC_EVENT_ENGINE_STATE_IND_V02:
      reportEngineState(eventPayload.pEngineState);
      break;

    case QMI_LOC_EVENT_FIX_SESSION_STATE_IND_V02:
      reportFixSessionState(eventPayload.pFixSessionState);
      break;

    // NMEA
    case QMI_LOC_EVENT_NMEA_IND_V02:
      reportNmea(eventPayload.pNmeaReportEvent);
      break;

    // XTRA request
    case QMI_LOC_EVENT_INJECT_PREDICTED_ORBITS_REQ_IND_V02:
      LOC_LOGD("%s:%d]: XTRA download request\n", __func__,
                    __LINE__);
      reportXtraServerUrl(eventPayload.pInjectPredictedOrbitsReqEvent);
      requestXtraData();
      break;

    // time request
    case QMI_LOC_EVENT_INJECT_TIME_REQ_IND_V02:
      LOC_LOGD("%s:%d]: Time request\n", __func__,
                    __LINE__);
      requestTime();
      break;

    //position request
    case QMI_LOC_EVENT_INJECT_POSITION_REQ_IND_V02:
      LOC_LOGD("%s:%d]: Position request\n", __func__,
                    __LINE__);
      requestLocation();
      break;

    // NI request
    case QMI_LOC_EVENT_NI_NOTIFY_VERIFY_REQ_IND_V02:
      reportNiRequest(eventPayload.pNiNotifyVerifyReqEvent);
      break;

    // AGPS connection request
    case QMI_LOC_EVENT_LOCATION_SERVER_CONNECTION_REQ_IND_V02:
      reportAtlRequest(eventPayload.pLocationServerConnReqEvent);
      break;

    case QMI_LOC_EVENT_GNSS_MEASUREMENT_REPORT_IND_V02:
      LOC_LOGD("%s:%d]: GNSS Measurement Report\n", __func__,
               __LINE__);
      reportSvMeasurement(eventPayload.pGnssSvRawInfoEvent);
      reportGnssMeasurementData(*eventPayload.pGnssSvRawInfoEvent); /*TBD merge into one function*/
      break;

    case QMI_LOC_EVENT_SV_POLYNOMIAL_REPORT_IND_V02:
      LOC_LOGD("%s:%d]: GNSS SV Polynomial Ind\n", __func__,
               __LINE__);
      reportSvPolynomial(eventPayload.pGnssSvPolyInfoEvent);
      break;

    //Unpropagated position report
    case QMI_LOC_EVENT_UNPROPAGATED_POSITION_REPORT_IND_V02:
      reportPosition(eventPayload.pPositionReportEvent, true);
      break;

    case QMI_LOC_GET_BLACKLIST_SV_IND_V02:
      LOC_LOGd("GET blacklist SV Ind");
      reportGnssSvIdConfig(*eventPayload.pGetBlacklistSvEvent);
      break;

    case QMI_LOC_GET_CONSTELLATION_CONTROL_IND_V02:
      LOC_LOGd("GET constellation Ind");
      reportGnssSvTypeConfig(*eventPayload.pGetConstellationConfigEvent);
      break;

    case  QMI_LOC_EVENT_WIFI_REQ_IND_V02:
      LOC_LOGd("WIFI Req Ind");
      requestOdcpi(*eventPayload.pWifiReqEvent);
      break;
  }
}

/* Call the service LocAdapterBase down event*/
void LocApiV02 :: errorCb(locClientHandleType /*handle*/,
                             locClientErrorEnumType errorId)
{
  if(errorId == eLOC_CLIENT_ERROR_SERVICE_UNAVAILABLE)
  {
    LOC_LOGE("%s:%d]: Service unavailable error\n",
                  __func__, __LINE__);

    handleEngineDownEvent();
  }
}

void LocApiV02 ::getWwanZppFix()
{
    sendMsg(new LocApiMsg([this] () {

    locClientReqUnionType req_union;
    qmiLocGetAvailWwanPositionReqMsgT_v02 zpp_req;
    memset(&zpp_req, 0, sizeof(zpp_req));

    req_union.pGetAvailWwanPositionReq = &zpp_req;

    LOC_LOGD("%s:%d]: Get ZPP Fix from available wwan position\n", __func__, __LINE__);
    locClientStatusEnumType status =
            locClientSendReq(QMI_LOC_GET_AVAILABLE_WWAN_POSITION_REQ_V02, req_union);

    if (status != eLOC_CLIENT_SUCCESS) {
        LOC_LOGe("error! status = %s\n", loc_get_v02_client_status_name(status));
    }
    }));
}

void LocApiV02 ::getBestAvailableZppFix()
{
    sendMsg(new LocApiMsg([this] () {

    locClientReqUnionType req_union;
    qmiLocGetBestAvailablePositionReqMsgT_v02 zpp_req;

    memset(&zpp_req, 0, sizeof(zpp_req));
    req_union.pGetBestAvailablePositionReq = &zpp_req;

    LOC_LOGd("Get ZPP Fix from best available source\n");

    locClientStatusEnumType status =
            locClientSendReq(QMI_LOC_GET_BEST_AVAILABLE_POSITION_REQ_V02, req_union);

    if (status != eLOC_CLIENT_SUCCESS) {
        LOC_LOGe("error! status = %s\n", loc_get_v02_client_status_name(status));
    }
    }));
}

LocationError LocApiV02 :: setGpsLockSync(GnssConfigGpsLock lock)
{
    LocationError err = LOCATION_ERROR_SUCCESS;
    qmiLocSetEngineLockReqMsgT_v02 setEngineLockReq;
    qmiLocSetEngineLockIndMsgT_v02 setEngineLockInd;
    locClientStatusEnumType status;
    locClientReqUnionType req_union;

    setEngineLockReq.lockType = convertGpsLockFromAPItoQMI((GnssConfigGpsLock)lock);;
    setEngineLockReq.subType_valid = true;
    setEngineLockReq.subType = eQMI_LOC_LOCK_ALL_SUB_V02;
    req_union.pSetEngineLockReq = &setEngineLockReq;
    memset(&setEngineLockInd, 0, sizeof(setEngineLockInd));
    status = locSyncSendReq(QMI_LOC_SET_ENGINE_LOCK_REQ_V02,
                            req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                            QMI_LOC_SET_ENGINE_LOCK_IND_V02,
                            &setEngineLockInd);
    if (eLOC_CLIENT_SUCCESS != status || eQMI_LOC_SUCCESS_V02 != setEngineLockInd.status) {
        LOC_LOGE("%s:%d]: Set engine lock failed. status: %s, ind status:%s\n",
            __func__, __LINE__,
            loc_get_v02_client_status_name(status),
            loc_get_v02_qmi_status_name(setEngineLockInd.status));
        err = LOCATION_ERROR_GENERAL_FAILURE;
    }
    LOC_LOGd("exit\n");
    return err;
}
/*
  Returns
  Current value of GPS Lock on success
  -1 on failure
*/
int LocApiV02 :: getGpsLock(uint8_t subType)
{
    qmiLocGetEngineLockReqMsgT_v02 getEngineLockReq;
    qmiLocGetEngineLockIndMsgT_v02 getEngineLockInd;
    locClientStatusEnumType status;
    locClientReqUnionType req_union;
    int ret=0;
    memset(&getEngineLockInd, 0, sizeof(getEngineLockInd));

    //Passing req_union as a parameter even though this request has no payload
    //since NULL or 0 gives an error during compilation
    getEngineLockReq.subType_valid = true;
    getEngineLockReq.subType = (qmiLocLockSubInfoEnumT_v02)subType;
    req_union.pGetEngineLockReq = &getEngineLockReq;
    status = locSyncSendReq(QMI_LOC_GET_ENGINE_LOCK_REQ_V02,
                            req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                            QMI_LOC_GET_ENGINE_LOCK_IND_V02,
                            &getEngineLockInd);
    if(status != eLOC_CLIENT_SUCCESS || getEngineLockInd.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGE("%s:%d]: Set engine lock failed. status: %s, ind status:%s\n",
                 __func__, __LINE__,
                 loc_get_v02_client_status_name(status),
                 loc_get_v02_qmi_status_name(getEngineLockInd.status));
        ret = -1;
    }
    else {
        if(getEngineLockInd.lockType_valid) {
            ret = (int)getEngineLockInd.lockType;
        }
        else {
            LOC_LOGE("%s:%d]: Lock Type not valid\n", __func__, __LINE__);
            ret = -1;
        }
    }
    return ret;
}

LocationError
LocApiV02:: setXtraVersionCheckSync(uint32_t check)
{
    LocationError err = LOCATION_ERROR_SUCCESS;
    qmiLocSetXtraVersionCheckReqMsgT_v02 req;
    qmiLocSetXtraVersionCheckIndMsgT_v02 ind;
    locClientStatusEnumType status;
    locClientReqUnionType req_union;

    LOC_LOGD("%s:%d]: Enter. check: %u", __func__, __LINE__, check);
    memset(&req, 0, sizeof(req));
    memset(&ind, 0, sizeof(ind));
    switch (check) {
    case 0:
        req.xtraVersionCheckMode = eQMI_LOC_XTRA_VERSION_CHECK_DISABLE_V02;
        break;
    case 1:
        req.xtraVersionCheckMode = eQMI_LOC_XTRA_VERSION_CHECK_AUTO_V02;
        break;
    case 2:
        req.xtraVersionCheckMode = eQMI_LOC_XTRA_VERSION_CHECK_XTRA2_V02;
        break;
    case 3:
        req.xtraVersionCheckMode = eQMI_LOC_XTRA_VERSION_CHECK_XTRA3_V02;
        break;
    default:
        req.xtraVersionCheckMode = eQMI_LOC_XTRA_VERSION_CHECK_DISABLE_V02;
        break;
    }

    req_union.pSetXtraVersionCheckReq = &req;
    status = locSyncSendReq(QMI_LOC_SET_XTRA_VERSION_CHECK_REQ_V02,
                            req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                            QMI_LOC_SET_XTRA_VERSION_CHECK_IND_V02,
                            &ind);
    if(status != eLOC_CLIENT_SUCCESS || ind.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGE("%s:%d]: Set xtra version check failed. status: %s, ind status:%s\n",
                 __func__, __LINE__,
                 loc_get_v02_client_status_name(status),
                 loc_get_v02_qmi_status_name(ind.status));
        err = LOCATION_ERROR_GENERAL_FAILURE;
    }

    LOC_LOGD("%s:%d]: Exit. err: %u", __func__, __LINE__, err);
    return err;
}

void LocApiV02 :: installAGpsCert(const LocDerEncodedCertificate* pData,
                                  size_t numberOfCerts,
                                  uint32_t slotBitMask)
{
    LOC_LOGD("%s:%d]:, slot mask=%u number of certs=%zu",
            __func__, __LINE__, slotBitMask, numberOfCerts);

    uint8_t certIndex = 0;
    for (uint8_t slot = 0; slot <= LOC_AGPS_CERTIFICATE_MAX_SLOTS-1; slot++, slotBitMask >>= 1)
    {
        if (slotBitMask & 1) //slot is writable
        {
            if (certIndex < numberOfCerts && pData[certIndex].data && pData[certIndex].length > 0)
            {
                LOC_LOGD("%s:%d]:, Inject cert#%u slot=%u length=%zu",
                         __func__, __LINE__, certIndex, slot, pData[certIndex].length);

                locClientReqUnionType req_union;
                locClientStatusEnumType status;
                qmiLocInjectSuplCertificateReqMsgT_v02 injectCertReq;
                qmiLocInjectSuplCertificateIndMsgT_v02 injectCertInd;

                memset(&injectCertReq, 0, sizeof(injectCertReq));
                memset(&injectCertInd, 0, sizeof(injectCertInd));
                injectCertReq.suplCertId = slot;
                injectCertReq.suplCertData_len = pData[certIndex].length;
                memcpy(injectCertReq.suplCertData, pData[certIndex].data, pData[certIndex].length);

                req_union.pInjectSuplCertificateReq = &injectCertReq;

                status = locSyncSendReq(QMI_LOC_INJECT_SUPL_CERTIFICATE_REQ_V02,
                                        req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                                        QMI_LOC_INJECT_SUPL_CERTIFICATE_IND_V02,
                                        &injectCertInd);

                if (status != eLOC_CLIENT_SUCCESS ||
                    eQMI_LOC_SUCCESS_V02 != injectCertInd.status)
                {
                    LOC_LOGE ("%s:%d]: inject-error status = %s, set_server_ind.status = %s",
                              __func__,__LINE__,
                              loc_get_v02_client_status_name(status),
                              loc_get_v02_qmi_status_name(injectCertInd.status));
                }

                certIndex++; //move to next cert

            } else {

                LOC_LOGD("%s:%d]:, Delete slot=%u",
                         __func__, __LINE__, slot);

                // A fake cert is injected first before delete is called to workaround
                // an issue that is seen with trying to delete an empty slot.
                {
                    locClientReqUnionType req_union;
                    locClientStatusEnumType status;
                    qmiLocInjectSuplCertificateReqMsgT_v02 injectFakeCertReq;
                    qmiLocInjectSuplCertificateIndMsgT_v02 injectFakeCertInd;

                    memset(&injectFakeCertReq, 0, sizeof(injectFakeCertReq));
                    memset(&injectFakeCertInd, 0, sizeof(injectFakeCertInd));
                    injectFakeCertReq.suplCertId = slot;
                    injectFakeCertReq.suplCertData_len = 1;
                    injectFakeCertReq.suplCertData[0] = 1;

                    req_union.pInjectSuplCertificateReq = &injectFakeCertReq;

                    status = locSyncSendReq(QMI_LOC_INJECT_SUPL_CERTIFICATE_REQ_V02,
                                            req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                                            QMI_LOC_INJECT_SUPL_CERTIFICATE_IND_V02,
                                            &injectFakeCertInd);

                    if (status != eLOC_CLIENT_SUCCESS ||
                        eQMI_LOC_SUCCESS_V02 != injectFakeCertInd.status)
                    {
                        LOC_LOGE ("%s:%d]: inject-fake-error status = %s, set_server_ind.status = %s",
                                  __func__,__LINE__,
                                  loc_get_v02_client_status_name(status),
                                  loc_get_v02_qmi_status_name(injectFakeCertInd.status));
                    }
                }

                locClientReqUnionType req_union;
                locClientStatusEnumType status;
                qmiLocDeleteSuplCertificateReqMsgT_v02 deleteCertReq;
                qmiLocDeleteSuplCertificateIndMsgT_v02 deleteCertInd;

                memset(&deleteCertReq, 0, sizeof(deleteCertReq));
                memset(&deleteCertInd, 0, sizeof(deleteCertInd));
                deleteCertReq.suplCertId = slot;
                deleteCertReq.suplCertId_valid = 1;

                req_union.pDeleteSuplCertificateReq = &deleteCertReq;

                status = locSyncSendReq(QMI_LOC_DELETE_SUPL_CERTIFICATE_REQ_V02,
                                        req_union, LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                                        QMI_LOC_DELETE_SUPL_CERTIFICATE_IND_V02,
                                        &deleteCertInd);

                if (status != eLOC_CLIENT_SUCCESS ||
                    eQMI_LOC_SUCCESS_V02 != deleteCertInd.status)
                {
                    LOC_LOGE("%s:%d]: delete-error status = %s, set_server_ind.status = %s",
                              __func__,__LINE__,
                              loc_get_v02_client_status_name(status),
                              loc_get_v02_qmi_status_name(deleteCertInd.status));
                }
            }
        } else {
            LOC_LOGD("%s:%d]:, Not writable slot=%u",
                     __func__, __LINE__, slot);
        }
    }
}

int LocApiV02::setSvMeasurementConstellation(const locClientEventMaskType mask)
{
    enum loc_api_adapter_err ret_val = LOC_API_ADAPTER_ERR_SUCCESS;
    qmiLocSetGNSSConstRepConfigReqMsgT_v02 setGNSSConstRepConfigReq;
    qmiLocSetGNSSConstRepConfigIndMsgT_v02 setGNSSConstRepConfigInd;
    locClientStatusEnumType status;
    locClientReqUnionType req_union;

    qmiLocGNSSConstellEnumT_v02 svConstellation = eQMI_SYSTEM_GPS_V02 |
                                                eQMI_SYSTEM_GLO_V02 |
                                                eQMI_SYSTEM_BDS_V02 |
                                                eQMI_SYSTEM_GAL_V02 |
                                                eQMI_SYSTEM_QZSS_V02;
    LOC_LOGD("%s] set GNSS measurement to report constellation: %" PRIu64 " "
            "report mask = 0x%" PRIx64 "\n",
            __func__, svConstellation, mask);

    memset(&setGNSSConstRepConfigReq, 0, sizeof(setGNSSConstRepConfigReq));

    setGNSSConstRepConfigReq.measReportConfig_valid = true;
    if (mask & QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02) {
        setGNSSConstRepConfigReq.measReportConfig = svConstellation;
    }

    setGNSSConstRepConfigReq.svPolyReportConfig_valid = true;
    if (mask & QMI_LOC_EVENT_MASK_GNSS_SV_POLYNOMIAL_REPORT_V02) {
        setGNSSConstRepConfigReq.svPolyReportConfig = svConstellation;
    }
    req_union.pSetGNSSConstRepConfigReq = &setGNSSConstRepConfigReq;
    memset(&setGNSSConstRepConfigInd, 0, sizeof(setGNSSConstRepConfigInd));

    status = locSyncSendReq(QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG_V02,
        req_union,
        LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
        QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG_IND_V02,
        &setGNSSConstRepConfigInd);

    if (status != eLOC_CLIENT_SUCCESS ||
        (setGNSSConstRepConfigInd.status != eQMI_LOC_SUCCESS_V02 &&
            setGNSSConstRepConfigInd.status != eQMI_LOC_ENGINE_BUSY_V02)) {
        LOC_LOGE("%s:%d]: Set GNSS constellation failed. status: %s, ind status:%s\n",
            __func__, __LINE__,
            loc_get_v02_client_status_name(status),
            loc_get_v02_qmi_status_name(setGNSSConstRepConfigInd.status));
        ret_val = LOC_API_ADAPTER_ERR_GENERAL_FAILURE;
    }
    else {
        LOC_LOGD("%s:%d]: Set GNSS constellation succeeded.\n",
            __func__, __LINE__);
    }

    return ret_val;
}


bool LocApiV02 :: cacheGnssMeasurementSupport()
{
    bool gnssMeasurementSupported = false;

    /*for GNSS Measurement service, use
      QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG_V02
      to check if modem support this feature or not*/
    LOC_LOGD("%s:%d]: set GNSS measurement.\n", __func__, __LINE__);

    if (LOC_API_ADAPTER_ERR_SUCCESS ==
        setSvMeasurementConstellation(QMI_LOC_EVENT_MASK_GNSS_MEASUREMENT_REPORT_V02)) {
        gnssMeasurementSupported = true;
    }

    LOC_LOGV("%s:%d]: gnssMeasurementSupported is %d\n", __func__, __LINE__,
            gnssMeasurementSupported);

    return gnssMeasurementSupported;
}

locClientStatusEnumType LocApiV02::locSyncSendReq(uint32_t req_id,
        locClientReqUnionType req_payload, uint32_t timeout_msec,
        uint32_t ind_id, void* ind_payload_ptr) {
    locClientStatusEnumType status = loc_sync_send_req(clientHandle, req_id, req_payload,
            timeout_msec, ind_id, ind_payload_ptr);
    if (eLOC_CLIENT_FAILURE_ENGINE_BUSY == status ||
            (eLOC_CLIENT_SUCCESS == status && nullptr != ind_payload_ptr &&
            eLOC_CLIENT_FAILURE_ENGINE_BUSY == *((qmiLocStatusEnumT_v02*)ind_payload_ptr))) {
        if (mResenders.empty()) {
            registerEventMask(mQmiMask | QMI_LOC_EVENT_MASK_ENGINE_STATE_V02);
        }
        LOC_LOGD("%s:%d]: Engine busy, cache req: %d", __func__, __LINE__, req_id);
        mResenders.push_back([=](){
                // ignore indicator, we use nullptr as the last parameter
                loc_sync_send_req(clientHandle, req_id, req_payload,
                    timeout_msec, ind_id, nullptr);
                });
    }
    return status;
}

void LocApiV02 ::
handleWwanZppFixIndication(const qmiLocGetAvailWwanPositionIndMsgT_v02& zpp_ind)
{
    LocGpsLocation zppLoc;
    memset(&zppLoc, 0, sizeof(zppLoc));

    LOC_LOGD("Got Wwan Zpp fix location validity (lat:%d, lon:%d, timestamp:%d accuracy:%d)\n "
             "(%.7f, %.7f), timestamp %" PRIu64 ", accuracy %f",
             zpp_ind.latitude_valid,
             zpp_ind.longitude_valid,
             zpp_ind.timestampUtc_valid,
             zpp_ind.horUncCircular_valid,
             zpp_ind.latitude,
             zpp_ind.longitude,
             zpp_ind.timestampUtc,
             zpp_ind.horUncCircular);

    if ((zpp_ind.latitude_valid == false) ||
        (zpp_ind.longitude_valid == false) ||
        (zpp_ind.horUncCircular_valid == false)) {
        LOC_LOGE(" Location not valid lat=%u lon=%u unc=%u",
                 zpp_ind.latitude_valid,
                 zpp_ind.longitude_valid,
                 zpp_ind.horUncCircular_valid);
    } else {

        zppLoc.size = sizeof(LocGpsLocation);
        if (zpp_ind.timestampUtc_valid) {
            zppLoc.timestamp = zpp_ind.timestampUtc;
        } else {
            /* The UTC time from modem is not valid.
            In this case, we use current system time instead.*/

            struct timespec time_info_current;
            clock_gettime(CLOCK_REALTIME,&time_info_current);
            zppLoc.timestamp = (time_info_current.tv_sec)*1e3 +
                               (time_info_current.tv_nsec)/1e6;
            LOC_LOGD("zpp timestamp got from system: %" PRIu64, zppLoc.timestamp);
        }

        zppLoc.flags = LOC_GPS_LOCATION_HAS_LAT_LONG | LOC_GPS_LOCATION_HAS_ACCURACY;
        zppLoc.latitude = zpp_ind.latitude;
        zppLoc.longitude = zpp_ind.longitude;
        zppLoc.accuracy = zpp_ind.horUncCircular;

        // If horCircularConfidence_valid is true, and horCircularConfidence value
        // is less than 68%, then scale the accuracy value to 68% confidence.
        if (zpp_ind.horCircularConfidence_valid)
        {
            scaleAccuracyTo68PercentConfidence(zpp_ind.horCircularConfidence,
                                               zppLoc, true);
        }

        if (zpp_ind.altitudeWrtEllipsoid_valid) {
            zppLoc.flags |= LOC_GPS_LOCATION_HAS_ALTITUDE;
            zppLoc.altitude = zpp_ind.altitudeWrtEllipsoid;
        }

        if (zpp_ind.vertUnc_valid) {
            zppLoc.flags |= LOC_GPS_LOCATION_HAS_VERT_UNCERTAINITY;
            zppLoc.vertUncertainity = zpp_ind.vertUnc;
        }
    }

    LocApiBase::reportWwanZppFix(zppLoc);
}

void LocApiV02::
    handleZppBestAvailableFixIndication(const qmiLocGetBestAvailablePositionIndMsgT_v02 &zpp_ind)
{
    LocGpsLocation zppLoc;
    GpsLocationExtended location_extended;
    LocPosTechMask tech_mask;

    memset(&zppLoc, 0, sizeof(zppLoc));
    zppLoc.size = sizeof(zppLoc);

    memset(&location_extended, 0, sizeof(location_extended));
    location_extended.size = sizeof(location_extended);

    tech_mask = LOC_POS_TECH_MASK_DEFAULT;

    LOC_LOGD("Got Zpp fix location validity (lat:%d, lon:%d, timestamp:%d accuracy:%d)"
            " (%.7f, %.7f), timestamp %" PRIu64 ", accuracy %f",
            zpp_ind.latitude_valid,
            zpp_ind.longitude_valid,
            zpp_ind.timestampUtc_valid,
            zpp_ind.horUncCircular_valid,
            zpp_ind.latitude,
            zpp_ind.longitude,
            zpp_ind.timestampUtc,
            zpp_ind.horUncCircular);

        if (zpp_ind.timestampUtc_valid) {
            zppLoc.timestamp = zpp_ind.timestampUtc;
        } else {
            /* The UTC time from modem is not valid.
                    In this case, we use current system time instead.*/

          struct timespec time_info_current;
          clock_gettime(CLOCK_REALTIME,&time_info_current);
          zppLoc.timestamp = (time_info_current.tv_sec)*1e3 +
                  (time_info_current.tv_nsec)/1e6;
          LOC_LOGD("zpp timestamp got from system: %" PRIu64, zppLoc.timestamp);
        }

        if (zpp_ind.latitude_valid && zpp_ind.longitude_valid &&
                zpp_ind.horUncCircular_valid ) {
            zppLoc.flags = LOC_GPS_LOCATION_HAS_LAT_LONG | LOC_GPS_LOCATION_HAS_ACCURACY;
            zppLoc.latitude = zpp_ind.latitude;
            zppLoc.longitude = zpp_ind.longitude;
            zppLoc.accuracy = zpp_ind.horUncCircular;

            // If horCircularConfidence_valid is true, and horCircularConfidence value
            // is less than 68%, then scale the accuracy value to 68% confidence.
            if (zpp_ind.horCircularConfidence_valid)
            {
                scaleAccuracyTo68PercentConfidence(zpp_ind.horCircularConfidence,
                                                   zppLoc, true);
            }

            if (zpp_ind.altitudeWrtEllipsoid_valid) {
                zppLoc.flags |= LOC_GPS_LOCATION_HAS_ALTITUDE;
                zppLoc.altitude = zpp_ind.altitudeWrtEllipsoid;
            }

            if (zpp_ind.horSpeed_valid) {
                zppLoc.flags |= LOC_GPS_LOCATION_HAS_SPEED;
                zppLoc.speed = zpp_ind.horSpeed;
            }

            if (zpp_ind.heading_valid) {
                zppLoc.flags |= LOC_GPS_LOCATION_HAS_BEARING;
                zppLoc.bearing = zpp_ind.heading;
            }

            if (zpp_ind.vertUnc_valid) {
                location_extended.flags |= GPS_LOCATION_EXTENDED_HAS_VERT_UNC;
                location_extended.vert_unc = zpp_ind.vertUnc;
            }

            if (zpp_ind.horSpeedUnc_valid) {
                location_extended.flags |= GPS_LOCATION_EXTENDED_HAS_SPEED_UNC;
                location_extended.speed_unc = zpp_ind.horSpeedUnc;
            }

            if (zpp_ind.headingUnc_valid) {
                location_extended.flags |= GPS_LOCATION_EXTENDED_HAS_BEARING_UNC;
                location_extended.bearing_unc = zpp_ind.headingUnc;
            }

            if (zpp_ind.technologyMask_valid) {
                tech_mask = zpp_ind.technologyMask;
            }

            if(zpp_ind.spoofReportMask_valid) {
                zppLoc.flags |= LOC_GPS_LOCATION_HAS_SPOOF_MASK;
                zppLoc.spoof_mask = (uint32_t)zpp_ind.spoofReportMask;
                LOC_LOGD("%s:%d QMI_spoofReportMask:0x%x", __func__, __LINE__,
                             (uint8_t)zppLoc.spoof_mask);
            }
        }

        LocApiBase::reportZppBestAvailableFix(zppLoc, location_extended, tech_mask);
}


LocPosTechMask LocApiV02 :: convertPosTechMask(
  qmiLocPosTechMaskT_v02 mask)
{
   LocPosTechMask locTechMask = LOC_POS_TECH_MASK_DEFAULT;

   if (mask & QMI_LOC_POS_TECH_MASK_SATELLITE_V02)
      locTechMask |= LOC_POS_TECH_MASK_SATELLITE;

   if (mask & QMI_LOC_POS_TECH_MASK_CELLID_V02)
      locTechMask |= LOC_POS_TECH_MASK_CELLID;

   if (mask & QMI_LOC_POS_TECH_MASK_WIFI_V02)
      locTechMask |= LOC_POS_TECH_MASK_WIFI;

   if (mask & QMI_LOC_POS_TECH_MASK_SENSORS_V02)
      locTechMask |= LOC_POS_TECH_MASK_SENSORS;

   if (mask & QMI_LOC_POS_TECH_MASK_REFERENCE_LOCATION_V02)
      locTechMask |= LOC_POS_TECH_MASK_REFERENCE_LOCATION;

   if (mask & QMI_LOC_POS_TECH_MASK_INJECTED_COARSE_POSITION_V02)
      locTechMask |= LOC_POS_TECH_MASK_INJECTED_COARSE_POSITION;

   if (mask & QMI_LOC_POS_TECH_MASK_AFLT_V02)
      locTechMask |= LOC_POS_TECH_MASK_AFLT;

   if (mask & QMI_LOC_POS_TECH_MASK_HYBRID_V02)
      locTechMask |= LOC_POS_TECH_MASK_HYBRID;

   return locTechMask;
}

LocNavSolutionMask LocApiV02 :: convertNavSolutionMask(
  qmiLocNavSolutionMaskT_v02 mask)
{
   LocNavSolutionMask locNavMask = 0;

   if (mask & QMI_LOC_NAV_MASK_SBAS_CORRECTION_IONO_V02)
      locNavMask |= LOC_NAV_MASK_SBAS_CORRECTION_IONO;

   if (mask & QMI_LOC_NAV_MASK_SBAS_CORRECTION_FAST_V02)
      locNavMask |= LOC_NAV_MASK_SBAS_CORRECTION_FAST;

   if (mask & QMI_LOC_POS_TECH_MASK_WIFI_V02)
      locNavMask |= LOC_POS_TECH_MASK_WIFI;

   if (mask & QMI_LOC_NAV_MASK_SBAS_CORRECTION_LONG_V02)
      locNavMask |= LOC_NAV_MASK_SBAS_CORRECTION_LONG;

   if (mask & QMI_LOC_NAV_MASK_SBAS_INTEGRITY_V02)
      locNavMask |= LOC_NAV_MASK_SBAS_INTEGRITY;

   return locNavMask;
}

qmiLocApnTypeMaskT_v02 LocApiV02::convertLocApnTypeMask(LocApnTypeMask mask)
{
    qmiLocApnTypeMaskT_v02 qmiMask = 0;

    if (mask & LOC_APN_TYPE_MASK_DEFAULT) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_DEFAULT_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_IMS) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_IMS_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_MMS) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_MMS_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_DUN) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_DUN_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_SUPL) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_SUPL_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_HIPRI) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_HIPRI_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_FOTA) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_FOTA_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_CBS) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_CBS_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_IA) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_IA_V02;
    }
    if (mask & LOC_APN_TYPE_MASK_EMERGENCY) {
        qmiMask |= QMI_LOC_APN_TYPE_MASK_EMERGENCY_V02;
    }

    return qmiMask;
}

LocApnTypeMask LocApiV02::convertQmiLocApnTypeMask(qmiLocApnTypeMaskT_v02 qmiMask)
{
    LocApnTypeMask mask = 0;

    if (qmiMask & QMI_LOC_APN_TYPE_MASK_DEFAULT_V02) {
        mask |= LOC_APN_TYPE_MASK_DEFAULT;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_IMS_V02) {
        mask |= LOC_APN_TYPE_MASK_IMS;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_MMS_V02) {
        mask |= LOC_APN_TYPE_MASK_MMS;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_DUN_V02) {
        mask |= LOC_APN_TYPE_MASK_DUN;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_SUPL_V02) {
        mask |= LOC_APN_TYPE_MASK_SUPL;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_HIPRI_V02) {
        mask |= LOC_APN_TYPE_MASK_HIPRI;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_FOTA_V02) {
        mask |= LOC_APN_TYPE_MASK_FOTA;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_CBS_V02) {
        mask |= LOC_APN_TYPE_MASK_CBS;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_IA_V02) {
        mask |= LOC_APN_TYPE_MASK_IA;
    }
    if (qmiMask & QMI_LOC_APN_TYPE_MASK_EMERGENCY_V02) {
        mask |= LOC_APN_TYPE_MASK_EMERGENCY;
    }

    return mask;
}

GnssConfigSuplVersion
LocApiV02::convertSuplVersion(const uint32_t suplVersion)
{
    switch (suplVersion) {
        case 0x00020000:
            return GNSS_CONFIG_SUPL_VERSION_2_0_0;
        case 0x00020002:
            return GNSS_CONFIG_SUPL_VERSION_2_0_2;
        case 0x00010000:
        default:
            return GNSS_CONFIG_SUPL_VERSION_1_0_0;
    }
}

GnssConfigLppProfile
LocApiV02::convertLppProfile(const uint32_t lppProfile)
{
    switch (lppProfile) {
        case 1:
            return GNSS_CONFIG_LPP_PROFILE_USER_PLANE;
        case 2:
            return GNSS_CONFIG_LPP_PROFILE_CONTROL_PLANE;
        case 3:
            return GNSS_CONFIG_LPP_PROFILE_USER_PLANE_AND_CONTROL_PLANE;
        case 0:
        default:
            return GNSS_CONFIG_LPP_PROFILE_RRLP_ON_LTE;
    }
}

GnssConfigLppeControlPlaneMask
LocApiV02::convertLppeCp(const uint32_t lppeControlPlaneMask)
{
    GnssConfigLppeControlPlaneMask mask = 0;
    if ((1<<0) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_DBH_BIT;
    }
    if ((1<<1) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_WLAN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<2) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_SRN_AP_MEASUREMENTS_BIT;
    }
    if ((1<<3) & lppeControlPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_CONTROL_PLANE_SENSOR_BARO_MEASUREMENTS_BIT;
    }
    return mask;
}

GnssConfigLppeUserPlaneMask
LocApiV02::convertLppeUp(const uint32_t lppeUserPlaneMask)
{
    GnssConfigLppeUserPlaneMask mask = 0;
    if ((1 << 0) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_DBH_BIT;
    }
    if ((1 << 1) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_WLAN_AP_MEASUREMENTS_BIT;
    }
    if ((1 << 2) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_SRN_AP_MEASUREMENTS_BIT;
    }
    if ((1 << 3) & lppeUserPlaneMask) {
        mask |= GNSS_CONFIG_LPPE_USER_PLANE_SENSOR_BARO_MEASUREMENTS_BIT;
    }
    return mask;
}

LocationError
LocApiV02::setBlacklistSvSync(const GnssSvIdConfig& config)
{
    locClientStatusEnumType status = eLOC_CLIENT_FAILURE_GENERAL;
    locClientReqUnionType req_union = {};

    qmiLocSetBlacklistSvReqMsgT_v02 setBlacklistSvMsg;
    qmiLocGenReqStatusIndMsgT_v02 genReqStatusIndMsg;

    // Clear all fields
    memset(&setBlacklistSvMsg, 0, sizeof(setBlacklistSvMsg));
    memset(&genReqStatusIndMsg, 0, sizeof(genReqStatusIndMsg));

    // Fill in the request details
    setBlacklistSvMsg.glo_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.glo_persist_blacklist_sv = config.gloBlacklistSvMask;
    setBlacklistSvMsg.glo_clear_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.glo_clear_persist_blacklist_sv = ~config.gloBlacklistSvMask;

    setBlacklistSvMsg.bds_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.bds_persist_blacklist_sv = config.bdsBlacklistSvMask;
    setBlacklistSvMsg.bds_clear_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.bds_clear_persist_blacklist_sv = ~config.bdsBlacklistSvMask;

    setBlacklistSvMsg.qzss_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.qzss_persist_blacklist_sv = config.qzssBlacklistSvMask;
    setBlacklistSvMsg.qzss_clear_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.qzss_clear_persist_blacklist_sv = ~config.qzssBlacklistSvMask;

    setBlacklistSvMsg.gal_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.gal_persist_blacklist_sv = config.galBlacklistSvMask;
    setBlacklistSvMsg.gal_clear_persist_blacklist_sv_valid = true;
    setBlacklistSvMsg.gal_clear_persist_blacklist_sv = ~config.galBlacklistSvMask;

    // Update in request union
    req_union.pSetBlacklistSvReq = &setBlacklistSvMsg;

    // Send the request
    status = loc_sync_send_req(clientHandle,
                               QMI_LOC_SET_BLACKLIST_SV_REQ_V02,
                               req_union,
                               LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                               QMI_LOC_SET_BLACKLIST_SV_IND_V02,
                               &genReqStatusIndMsg);
    if(status != eLOC_CLIENT_SUCCESS ||
            genReqStatusIndMsg.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGe("Set Blacklist SV failed. status: %s ind status %s",
                 loc_get_v02_client_status_name(status),
                 loc_get_v02_qmi_status_name(genReqStatusIndMsg.status));
        return LOCATION_ERROR_GENERAL_FAILURE;
    }

    return LOCATION_ERROR_SUCCESS;
}

void
LocApiV02::setBlacklistSv(const GnssSvIdConfig& config)
{
    sendMsg(new LocApiMsg([this, config] () {
        setBlacklistSvSync(config);
    }));
}

void LocApiV02::getBlacklistSv()
{
    sendMsg(new LocApiMsg([this] () {

    locClientStatusEnumType status = eLOC_CLIENT_FAILURE_GENERAL;
    locClientReqUnionType req_union = {};

    // Nothing to update in request union

    // Send the request
    status = locClientSendReq(QMI_LOC_GET_BLACKLIST_SV_REQ_V02, req_union);
    if(status != eLOC_CLIENT_SUCCESS) {
        LOC_LOGe("Get Blacklist SV failed. status: %s",
                 loc_get_v02_client_status_name(status));
    }

    }));
}

void
LocApiV02::setConstellationControl(const GnssSvTypeConfig& config)
{
    sendMsg(new LocApiMsg([this, config] () {

    locClientStatusEnumType status = eLOC_CLIENT_FAILURE_GENERAL;
    locClientReqUnionType req_union = {};

    qmiLocSetConstellationConfigReqMsgT_v02 setConstellationConfigMsg;
    qmiLocGenReqStatusIndMsgT_v02 genReqStatusIndMsg;

    // Clear all fields
    memset (&setConstellationConfigMsg, 0, sizeof(setConstellationConfigMsg));
    memset(&genReqStatusIndMsg, 0, sizeof(genReqStatusIndMsg));

    // Fill in the request details
    setConstellationConfigMsg.resetConstellations = false;

    if (config.enabledSvTypesMask != 0) {
        setConstellationConfigMsg.enableMask_valid = true;
        setConstellationConfigMsg.enableMask = config.enabledSvTypesMask;
    }
    if (config.blacklistedSvTypesMask != 0) {
        setConstellationConfigMsg.disableMask_valid = true;
        setConstellationConfigMsg.disableMask = config.blacklistedSvTypesMask;
    }

    // Update in request union
    req_union.pSetConstellationConfigReq = &setConstellationConfigMsg;

    // Send the request
    status = loc_sync_send_req(clientHandle,
                               QMI_LOC_SET_CONSTELLATION_CONTROL_REQ_V02,
                               req_union,
                               LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                               QMI_LOC_SET_CONSTELLATION_CONTROL_IND_V02,
                               &genReqStatusIndMsg);
    if(status != eLOC_CLIENT_SUCCESS ||
            genReqStatusIndMsg.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGe("Set Constellation Config failed. status: %s ind status %s",
                 loc_get_v02_client_status_name(status),
                 loc_get_v02_qmi_status_name(genReqStatusIndMsg.status));
    }

    }));
}

void
LocApiV02::getConstellationControl()
{
    sendMsg(new LocApiMsg([this] () {

    locClientStatusEnumType status = eLOC_CLIENT_FAILURE_GENERAL;
    locClientReqUnionType req_union = {};

    // Nothing to update in request union

    // Send the request
    status = locClientSendReq(QMI_LOC_GET_CONSTELLATION_CONTROL_REQ_V02, req_union);
    if(status != eLOC_CLIENT_SUCCESS) {
        LOC_LOGe("Get Constellation failed. status: %s",
                 loc_get_v02_client_status_name(status));
    }

    }));
}

void
LocApiV02::resetConstellationControl()
{
    sendMsg(new LocApiMsg([this] () {

    locClientStatusEnumType status = eLOC_CLIENT_FAILURE_GENERAL;
    locClientReqUnionType req_union = {};

    qmiLocSetConstellationConfigReqMsgT_v02 setConstellationConfigMsg;
    qmiLocGenReqStatusIndMsgT_v02 genReqStatusIndMsg;

    // Clear all fields
    memset (&setConstellationConfigMsg, 0, sizeof(setConstellationConfigMsg));
    memset(&genReqStatusIndMsg, 0, sizeof(genReqStatusIndMsg));

    // Fill in the request details
    setConstellationConfigMsg.resetConstellations = true;

    // Update in request union
    req_union.pSetConstellationConfigReq = &setConstellationConfigMsg;

    // Send the request
    status = loc_sync_send_req(clientHandle,
                               QMI_LOC_SET_CONSTELLATION_CONTROL_REQ_V02,
                               req_union,
                               LOC_ENGINE_SYNC_REQUEST_TIMEOUT,
                               QMI_LOC_SET_CONSTELLATION_CONTROL_IND_V02,
                               &genReqStatusIndMsg);
    if(status != eLOC_CLIENT_SUCCESS ||
            genReqStatusIndMsg.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGe("Reset Constellation Config failed. "
                 "status: %s ind status %s",
                 loc_get_v02_client_status_name(status),
                 loc_get_v02_qmi_status_name(genReqStatusIndMsg.status));
    }

    }));
}

void
LocApiV02::reportGnssSvIdConfig(
        const qmiLocGetBlacklistSvIndMsgT_v02& ind)
{
    // Validate status
    if (ind.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGe("Ind failure status %d", ind.status);
        return;
    }

    // Parse all fields
    GnssSvIdConfig config = {};
    config.size = sizeof(GnssSvIdConfig);
    if (ind.bds_persist_blacklist_sv_valid) {
        config.bdsBlacklistSvMask = ind.bds_persist_blacklist_sv;
    }
    if (ind.gal_persist_blacklist_sv_valid) {
        config.galBlacklistSvMask = ind.gal_persist_blacklist_sv;
    }
    if (ind.qzss_persist_blacklist_sv_valid) {
        config.qzssBlacklistSvMask = ind.qzss_persist_blacklist_sv;
    }
    if (ind.glo_persist_blacklist_sv_valid) {
        config.gloBlacklistSvMask = ind.glo_persist_blacklist_sv;
    }

    // Pass on GnssSvConfig
    LocApiBase::reportGnssSvIdConfig(config);
}

void
LocApiV02::reportGnssSvTypeConfig(
        const qmiLocGetConstellationConfigIndMsgT_v02& ind)
{
    // Validate status
    if (ind.status != eQMI_LOC_SUCCESS_V02) {
        LOC_LOGe("Ind failure status %d", ind.status);
        return;
    }

    // Parse all fields
    GnssSvTypeConfig config = {};
    config.size = sizeof(GnssSvTypeConfig);
    convertToGnssSvTypeConfig(ind, config);

    // Pass on GnssSvConfig
    LocApiBase::reportGnssSvTypeConfig(config);
}

void
LocApiV02::convertToGnssSvTypeConfig(
        const qmiLocGetConstellationConfigIndMsgT_v02& ind,
        GnssSvTypeConfig& config)
{
    if (ind.bds_status_valid &&
            (ind.bds_status == eQMI_LOC_CONSTELLATION_ENABLED_MANDATORY_V02 ||
                    ind.bds_status == eQMI_LOC_CONSTELLATION_ENABLED_INTERNALLY_V02 ||
                    ind.bds_status == eQMI_LOC_CONSTELLATION_ENABLED_BY_CLIENT_V02)) {
        config.enabledSvTypesMask |= GNSS_SV_TYPES_MASK_BDS_BIT;
    }
    if (ind.glonass_status_valid &&
            (ind.glonass_status == eQMI_LOC_CONSTELLATION_ENABLED_MANDATORY_V02 ||
                    ind.glonass_status == eQMI_LOC_CONSTELLATION_ENABLED_INTERNALLY_V02 ||
                    ind.glonass_status == eQMI_LOC_CONSTELLATION_ENABLED_BY_CLIENT_V02)) {
        config.enabledSvTypesMask |= GNSS_SV_TYPES_MASK_GLO_BIT;
    }
    if (ind.galileo_status_valid &&
            (ind.galileo_status == eQMI_LOC_CONSTELLATION_ENABLED_MANDATORY_V02 ||
                    ind.galileo_status == eQMI_LOC_CONSTELLATION_ENABLED_INTERNALLY_V02 ||
                    ind.galileo_status == eQMI_LOC_CONSTELLATION_ENABLED_BY_CLIENT_V02)) {
        config.enabledSvTypesMask |= GNSS_SV_TYPES_MASK_GAL_BIT;
    }
    if (ind.qzss_status_valid &&
            (ind.qzss_status == eQMI_LOC_CONSTELLATION_ENABLED_MANDATORY_V02 ||
                    ind.qzss_status == eQMI_LOC_CONSTELLATION_ENABLED_INTERNALLY_V02 ||
                    ind.qzss_status == eQMI_LOC_CONSTELLATION_ENABLED_BY_CLIENT_V02)) {
        config.enabledSvTypesMask |= GNSS_SV_TYPES_MASK_QZSS_BIT;
    }
}

qmiLocPowerModeEnumT_v02 LocApiV02::convertPowerMode(GnssPowerMode powerMode)
{
    switch (powerMode) {
    case GNSS_POWER_MODE_M1:
        return eQMI_LOC_POWER_MODE_IMPROVED_ACCURACY_V02;
    case GNSS_POWER_MODE_M2:
        return eQMI_LOC_POWER_MODE_NORMAL_V02;
    case GNSS_POWER_MODE_M3:
        return eQMI_LOC_POWER_MODE_BACKGROUND_DEFINED_POWER_V02;
    case GNSS_POWER_MODE_M4:
        return eQMI_LOC_POWER_MODE_BACKGROUND_DEFINED_TIME_V02;
    case GNSS_POWER_MODE_M5:
        return eQMI_LOC_POWER_MODE_BACKGROUND_KEEP_WARM_V02;
    default:
        LOC_LOGE("Invalid power mode %d", powerMode);
    }

    return QMILOCPOWERMODEENUMT_MIN_ENUM_VAL_V02;
}
