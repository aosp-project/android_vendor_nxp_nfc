/******************************************************************************
 *
 *  Copyright 2018 NXP
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/
#define LOG_TAG "JcDnld_DWP"
#include "DwpEseUpdater.h"
#include <cutils/log.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <JcDnld.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include "EseUpdateChecker.h"
#include "EseAdaptation.h"
#include "DwpSeChannelCallback.h"
#include "DwpSeEvtCallback.h"
#include <phNxpNciHal_Adaptation.h>
#include <phNxpConfig.h>
#include "hal_nxpese.h"
#include <vendor/nxp/nxpese/1.0/INxpEse.h>
#include "phNxpNfc_IntfApi.h"

using vendor::nxp::nxpese::V1_0::INxpEse;
using android::hardware::hidl_vec;
using android::sp;
using android::hardware::Return;
using android::hardware::Void;

android::sp<INxpEse> mHalNxpEse;
uint8_t datahex(char c);
se_extns_entry se_intf, nfc_intf;
void* eSEClientUpdate_NFC_ThreadHandler(void* data);
extern EseAdaptation *gpEseAdapt;

DwpEseUpdater DwpEseUpdater::sEseClientInstance;
spSeChannel DwpEseUpdater::sDwpSeChannelCallback = nullptr;
spSeEvt DwpEseUpdater::sDwpSeEventCallback = nullptr;
DwpEseUpdater::DwpEseUpdater() {}

DwpEseUpdater &DwpEseUpdater::getInstance() { return sEseClientInstance; }

void DwpEseUpdater::checkIfEseClientUpdateReqd()
{
  ALOGD("%s enter:  ", __func__);
  bool nfcSEIntfPresent = false;
  char nfcterminal[5];
  nfc_intf = eseUpdateChecker.checkEseUpdateRequired(ESE_INTF_NFC);
  se_intf = eseUpdateChecker.checkEseUpdateRequired(ESE_INTF_SPI);

  if(eseUpdateChecker.isWiredSeTerminalAvailable(nfcterminal)) {
    nfcSEIntfPresent = true;
    ALOGD("%s SMB intf  is present  ", __func__);
  }
  if((nfc_intf.isJcopUpdateRequired|| nfc_intf.isLSUpdateRequired) && (nfcSEIntfPresent)) {
    DwpEseUpdater::setDwpEseClientState(ESE_UPDATE_STARTED);
  }
  else
  {
     nfc_intf.isJcopUpdateRequired = false;
     nfc_intf.isLSUpdateRequired = false;
     ALOGD("%s LS and JCOP download not required ", __func__);
  }
}

void IoctlCallback_DwpClient(hidl_vec<uint8_t> outputData) {
  const char* func = "IoctlCallback_DwpClient";
  ese_nxp_ExtnOutputData_t* pOutData =
      (ese_nxp_ExtnOutputData_t*)&outputData[0];
  ALOGD_IF(nfc_debug_enabled, "%s Ioctl Type=%lu", func,
           (unsigned long)pOutData->ioctlType);
  EseAdaptation* pAdaptation = (EseAdaptation*)pOutData->context;
  /*Output Data from stub->Proxy is copied back to output data
   * This data will be sent back to libese*/
  memcpy(&pAdaptation->mCurrentIoctlData->out, &outputData[0],
         sizeof(ese_nxp_ExtnOutputData_t));
}

SESTATUS DwpEseUpdater::doEseUpdateIfReqd() {
  ALOGD("%s enter:  ", __func__);
  eSeClientUpdateHandler();
  return SESTATUS_SUCCESS;
}

void DwpEseUpdater::eSEClientUpdate_NFC_Thread()
{
  SESTATUS status = SESTATUS_FAILED;
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attr, &eSEClientUpdate_NFC_ThreadHandler, NULL) != 0) {
    ALOGD("Thread creation failed");
    status = SESTATUS_FAILED;
  } else {
    status = SESTATUS_SUCCESS;
    ALOGD("Thread creation success");
  }
    pthread_attr_destroy(&attr);
}

void* eSEClientUpdate_NFC_ThreadHandler(void* data) {
  (void)data;
  ALOGD("%s Enter\n", __func__);
  DwpEseUpdater::eSEUpdate_SeqHandler();
  ALOGD("%s Exit eSEClientUpdate_Thread\n", __func__);
  return NULL;

}

void DwpEseUpdater::eSeClientUpdateHandler() {

  ALOGD("%s Enter\n", __func__);

  if (nfc_intf.isJcopUpdateRequired)
    DwpEseUpdater::setDwpEseClientState(ESE_JCOP_UPDATE_REQUIRED);
  else if (se_intf.isJcopUpdateRequired) {
    DwpEseUpdater::setSpiEseClientState(ESE_JCOP_UPDATE_REQUIRED);
    return;
  }

  if((eseUpdateDwp == ESE_JCOP_UPDATE_REQUIRED) ||
    (eseUpdateDwp == ESE_LS_UPDATE_REQUIRED))
    DwpEseUpdater::eSEUpdate_SeqHandler();

  ALOGD("%s Exit \n", __func__);

}

SESTATUS DwpEseUpdater::handleJcopOsDownload() {
  ALOGD("%s enter:  ", __func__);

  uint8_t status ;
  status = 0;

  phNxpNfc_InitLib();

  usleep(50 * 1000);
  ALOGE("%s: after init", __FUNCTION__);
  nfc_debug_enabled = true;

  sDwpSeChannelCallback = std::make_shared<DwpSeChannelCallback>();
  sDwpSeEventCallback = std::make_shared<DwpSeEvtCallback>();
  jcDnld.registerSeCallback(sDwpSeChannelCallback, sDwpSeEventCallback);
  status = (SESTATUS) jcDnld.doUpdate();
  sleep(1);

  ALOGD("%s pthread_exit\n", __func__);
  return (SESTATUS)status;
}

void DwpEseUpdater::setSpiEseClientState(uint8_t state)
{
  ALOGE("%s: State = %d", __FUNCTION__, state);
  eseUpdateSpi = (ESE_UPDATE_STATE)state;
}

void DwpEseUpdater::setDwpEseClientState(uint8_t state)
{
  ALOGE("%s: State = %d", __FUNCTION__, state);
  eseUpdateDwp = (ESE_UPDATE_STATE)state;
}

void DwpEseUpdater::sendeSEUpdateState(uint8_t state) {
  ese_nxp_IoctlInOutData_t inpOutData;
  gpEseAdapt = &EseAdaptation::GetInstance();
  gpEseAdapt->Initialize();
  ALOGE("%s: State = %d", __FUNCTION__, state);
  memset(&inpOutData, 0x00, sizeof(ese_nxp_IoctlInOutData_t));
  inpOutData.inp.data.nxpCmd.cmd_len = sizeof(state);
  memcpy(inpOutData.inp.data.nxpCmd.p_cmd, &state,sizeof(state));
  inpOutData.inp.data_source = 2;
  phNxpNciHal_ioctl(HAL_ESE_IOCTL_NFC_JCOP_DWNLD, &inpOutData);
}

SESTATUS DwpEseUpdater::eSEUpdate_SeqHandler() {
    switch(eseUpdateDwp)
    {
      case ESE_UPDATE_STARTED:
        break;
      case ESE_JCOP_UPDATE_REQUIRED:
        ALOGE("%s: ESE_JCOP_UPDATE_REQUIRED", __FUNCTION__);
        if(nfc_intf.isJcopUpdateRequired) {
          if(nfc_intf.sJcopUpdateInterface == ESE_INTF_NFC) {
            DwpEseUpdater::handleJcopOsDownload();
            return SESTATUS_SUCCESS;
          }
          else if(nfc_intf.sJcopUpdateInterface == ESE_INTF_SPI) {
            return SESTATUS_SUCCESS;
          }
        }
      break;
      case ESE_JCOP_UPDATE_COMPLETED:
      case ESE_LS_UPDATE_REQUIRED:
        ALOGD("LSUpdate DWP commented");
      case ESE_LS_UPDATE_COMPLETED:
      case ESE_UPDATE_COMPLETED:
      {
        ese_nxp_IoctlInOutData_t inpOutData;
        DwpEseUpdater::setDwpEseClientState(ESE_UPDATE_COMPLETED);
        ALOGD("LSUpdate Thread not required inform NFC to restart");
        memset(&inpOutData, 0x00, sizeof(ese_nxp_IoctlInOutData_t));
        inpOutData.inp.data_source = 2;
        usleep(50 * 1000);
        phNxpNciHal_ioctl(HAL_NFC_IOCTL_ESE_UPDATE_COMPLETE, &inpOutData);
      }
      break;
    }
    return SESTATUS_SUCCESS;
}