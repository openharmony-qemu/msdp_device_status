/*
 * Copyright (c) 2022-2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "devicestatus_msdp_client_impl.h"

#include <string>

#include <dlfcn.h>

#include "devicestatus_define.h"
#include "devicestatus_dumper.h"
#include "fi_log.h"

#undef LOG_TAG
#define LOG_TAG "DeviceStatusMsdpClientImpl"

namespace OHOS {
namespace Msdp {
namespace DeviceStatus {
namespace {
#ifdef __aarch64__
const std::string DEVICESTATUS_MOCK_LIB_PATH { "/system/lib64/libdevicestatus_mock.z.so" };
const std::string DEVICESTATUS_ALGO_LIB_PATH { "/system/lib64/libdevicestatus_algo.z.so" };
#else
const std::string DEVICESTATUS_MOCK_LIB_PATH { "/system/lib/libdevicestatus_mock.z.so" };
const std::string DEVICESTATUS_ALGO_LIB_PATH { "/system/lib/libdevicestatus_algo.z.so" };
#endif
using ClientType = Type;
using ClientValue = OnChangedValue;
} // namespace

DeviceStatusMsdpClientImpl::DeviceStatusMsdpClientImpl()
{
    for (int32_t type = 0; type < static_cast<int32_t>(Type::TYPE_MAX); ++type) {
        mockCallCounts_[static_cast<Type>(type)] = 0;
    }
}

ErrCode DeviceStatusMsdpClientImpl::InitMsdpImpl(Type type)
{
    CALL_DEBUG_ENTER;
    if (GetSensorHdi(type) == RET_OK) {
        FI_HILOGI("GetSensorHdi is support");
        return RET_OK;
    }
    #ifdef DEVICE_STATUS_SENSOR_ENABLE
    if (AlgoHandle(type) == RET_OK) {
        FI_HILOGI("AlgoHandle is support");
        return RET_OK;
    }
    #else
    FI_HILOGE("Enable:sensor is not exist");
    #endif // DEVICE_STATUS_SENSOR_ENABLE
#ifndef DEVICE_STATUS_DISABLE_MOCK_FALLBACK
    if (MockHandle(type) == RET_OK) {
        FI_HILOGI("MockHandle is support");
        return RET_OK;
    }
#else
    FI_HILOGE("No real sensor backend supports this type; mock fallback is disabled");
#endif
    return RET_ERR;
}

ErrCode DeviceStatusMsdpClientImpl::MockHandle(Type type)
{
    if (type > TYPE_MAX || type < TYPE_INVALID) {
        FI_HILOGE("Type invalid");
        return RET_ERR;
    }
    if (StartMock(type) == RET_ERR) {
        FI_HILOGE("Start mock Library failed");
        return RET_ERR;
    }
    std::unique_lock lock(mutex_);
    CHKPR(iMock_, RET_ERR);
    iMock_->Enable(type);
    auto iter = mockCallCounts_.find(type);
    if (iter == mockCallCounts_.end()) {
        auto ret = mockCallCounts_.emplace(type, 0);
        if (!ret.second) {
            FI_HILOGW("type is duplicated");
            return RET_ERR;
        }
    } else {
        iter->second++;
    }
    RegisterMock();
    FI_HILOGI("mockCallCounts_:%{public}d", mockCallCounts_[type]);
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::AlgoHandle(Type type)
{
    if (algoRetiring_) { return RET_ERR; }
    std::unique_lock lifecycle(algoLifecycleMutex_, std::try_to_lock);
    if (!lifecycle.owns_lock() || algoRetiring_ || GetAlgoAbility(type) != RET_OK) { return RET_ERR; }
    auto existing = algoCallCounts_.find(type);
    if (existing != algoCallCounts_.end()) {
        if (existing->second == UINT32_MAX) { return RET_ERR; }
        ++existing->second;
        return RET_OK;
    }
    if (weak_from_this().expired() || StartAlgo(type) != RET_OK || iAlgo_ == nullptr) { return RET_ERR; }
    if (RegisterAlgo() != RET_OK || iAlgo_->Enable(type) != RET_OK) {
        if (algoCallCounts_.empty()) { UnloadAlgoLibraryLocked(); }
        return RET_ERR;
    }
    algoCallCounts_.emplace(type, 1);
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::StartAlgo(Type type)
{
    if (LoadAlgoLibrary() == RET_ERR) {
        FI_HILOGE("Load algo Library failed");
        return RET_ERR;
    }
    // GetAlgoInst owns mutex_; do not recursively lock the same std::mutex.
    iAlgo_ = GetAlgoInst(type);
    CHKPR(iAlgo_, RET_ERR);
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::StartMock(Type type)
{
    if (LoadMockLibrary() == RET_ERR) {
        FI_HILOGE("Load mock Library failed");
        return RET_ERR;
    }
    std::unique_lock lock(mutex_);
    iMock_ = GetMockInst(type);
    if (iMock_ == nullptr) {
        FI_HILOGE("Get mock module failed");
        return RET_ERR;
    }
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::GetSensorHdi(Type type)
{
    return RET_ERR;
}

ErrCode DeviceStatusMsdpClientImpl::GetAlgoAbility(Type type)
{
    if ((type == Type::TYPE_STILL) || (type == Type::TYPE_ABSOLUTE_STILL) || (type == Type::TYPE_HORIZONTAL_POSITION) ||
        (type == Type::TYPE_VERTICAL_POSITION)) {
        FI_HILOGI("Support ability type:%{public}d", type);
        return RET_OK;
    }
    FI_HILOGI("Not support ability");
    return RET_ERR;
}

ErrCode DeviceStatusMsdpClientImpl::Disable(Type type)
{
    CALL_DEBUG_ENTER;
    return (((SensorHdiDisable(type) == RET_OK) || (AlgoDisable(type) == RET_OK) || (MockDisable(type) == RET_OK)) ?
        RET_OK : RET_ERR);
}

ErrCode DeviceStatusMsdpClientImpl::SensorHdiDisable(Type type)
{
    return RET_ERR;
}

ErrCode DeviceStatusMsdpClientImpl::AlgoDisable(Type type)
{
#ifdef DEVICE_STATUS_SENSOR_ENABLE
    if (algoRetiring_) { return RET_ERR; }
    std::unique_lock lifecycle(algoLifecycleMutex_, std::try_to_lock);
    if (!lifecycle.owns_lock() || algoRetiring_ || iAlgo_ == nullptr) { return RET_ERR; }
    auto found = algoCallCounts_.find(type);
    if (found == algoCallCounts_.end()) { return RET_ERR; }
    if (found->second > 1) { --found->second; return RET_OK; }
    auto self = weak_from_this().lock();
    if (self == nullptr || iAlgo_->Disable(type) != RET_OK) { return RET_ERR; }
    algoCallCounts_.erase(found);
    if (algoCallCounts_.empty()) {
        algoRetiring_ = true;
        // This code is in the service DSO, not the algorithm being retired.
        // It waits for the actual worker's join after any reentrant callback returns.
        std::thread([self] { self->RetireAlgo(); }).detach();
    }
    return RET_OK;
#else
    (void)type;
    return RET_ERR;
#endif
}

ErrCode DeviceStatusMsdpClientImpl::MockDisable(Type type)
{
    CALL_DEBUG_ENTER;
    std::unique_lock lock(mutex_);
    CHKPR(iMock_, RET_ERR);
    auto iter = mockCallCounts_.find(type);
    if (iter == mockCallCounts_.end()) {
        FI_HILOGE("Failed to find record type");
        return RET_ERR;
    }
    if (iter->second == 0) {
        mockCallCounts_.erase(type);
    } else {
        iMock_->DisableCount(type);
        iMock_->Disable(type);
        UnregisterMock();
    }
    iter->second--;
    if (mockCallCounts_.empty()) {
        if (UnloadMockLibrary() == RET_ERR) {
            FI_HILOGE("Failed to close library");
            return RET_ERR;
        }
        iMock_ = nullptr;
        callBacksMgr_ = nullptr;
    }
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::ImplCallback(const Data &data)
{
    CallbackManager callback;
    { std::lock_guard lock(callMutex_); callback = callBacksMgr_; }
    return callback ? callback(data) : RET_ERR;
}

ErrCode DeviceStatusMsdpClientImpl::RegisterImpl(const CallbackManager &callback)
{
    if (!callback) { return RET_ERR; }
    std::lock_guard lock(callMutex_);
    callBacksMgr_ = callback;
    return RET_OK;
}

void DeviceStatusMsdpClientImpl::OnResult(const Data &data)
{
    FI_HILOGD("type:%{public}d, value:%{public}d", data.type, data.value);
    MsdpCallback(data);
}

ErrCode DeviceStatusMsdpClientImpl::RegisterMock()
{
    CALL_DEBUG_ENTER;
    if (iMock_ != nullptr) {
        std::shared_ptr<IMsdp::MsdpAlgoCallback> callback = shared_from_this();
        iMock_->RegisterCallback(callback);
    }
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::UnregisterMock()
{
    CALL_DEBUG_ENTER;
    CHKPR(iMock_, RET_ERR);
    iMock_->UnregisterCallback();
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::RegisterAlgo()
{
    if (iAlgo_ == nullptr) { return RET_ERR; }
    auto self = weak_from_this().lock();
    if (self == nullptr) { return RET_ERR; }
    return iAlgo_->RegisterCallback(self);
}

ErrCode DeviceStatusMsdpClientImpl::UnregisterAlgo()
{
    CALL_DEBUG_ENTER;
    CHKPR(iAlgo_, RET_ERR);
    iAlgo_->UnregisterCallback();
    return RET_OK;
}

int32_t DeviceStatusMsdpClientImpl::MsdpCallback(const Data &data)
{
    DS_DUMPER->PushDeviceStatus(data);
    SaveObserverData(data);
    return ImplCallback(data);
}

Data DeviceStatusMsdpClientImpl::SaveObserverData(const Data &data)
{
    std::lock_guard lock(callMutex_);
    deviceStatusDatas_[data.type] = data.value;
    return data;
}

std::map<ClientType, ClientValue> DeviceStatusMsdpClientImpl::GetObserverData() const
{
    std::unique_lock callLock(callMutex_);
    return deviceStatusDatas_;
}

void DeviceStatusMsdpClientImpl::GetDeviceStatusTimestamp()
{}

void DeviceStatusMsdpClientImpl::GetLongtitude()
{}

void DeviceStatusMsdpClientImpl::GetLatitude()
{}

ErrCode DeviceStatusMsdpClientImpl::LoadMockLibrary()
{
    CALL_DEBUG_ENTER;
    std::unique_lock lock(mutex_);
    if (mock_.handle != nullptr) {
        FI_HILOGW("mock handle is not nullptr");
        return RET_OK;
    }
    std::string dlName = DEVICESTATUS_MOCK_LIB_PATH;
    char libRealPath[PATH_MAX] = { 0 };
    if (realpath(dlName.c_str(), libRealPath) == nullptr) {
        FI_HILOGE("Get absolute algoPath is error, errno:%{public}d", errno);
        return RET_ERR;
    }
    mock_.handle = dlopen(libRealPath, RTLD_LAZY);
    if (mock_.handle == nullptr) {
        FI_HILOGE("Cannot load library error:%{public}s", dlerror());
        return RET_ERR;
    }
    FI_HILOGI("Start create pointer");
    mock_.create = reinterpret_cast<LoadMockLibraryFunc>(dlsym(mock_.handle, "Create"));
    FI_HILOGI("Start destroy pointer");
    mock_.destroy = reinterpret_cast<LoadMockLibraryPtr>(dlsym(mock_.handle, "Destroy"));
    if ((mock_.create == nullptr) || (mock_.destroy == nullptr)) {
        FI_HILOGE("%{public}s dlsym Create or Destroy failed",
            dlName.c_str());
        dlclose(mock_.handle);
        mock_.Clear();
        if (mock_.handle == nullptr) {
            return RET_OK;
        }
        FI_HILOGE("Load mock failed");
        return RET_ERR;
    }
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::UnloadMockLibrary()
{
    CALL_DEBUG_ENTER;
    std::unique_lock lock(mutex_);
    CHKPR(mock_.handle, RET_ERR);
    if (mock_.pAlgorithm != nullptr) {
        mock_.destroy(mock_.pAlgorithm);
        mock_.pAlgorithm = nullptr;
    }
    dlclose(mock_.handle);
    mock_.Clear();
    return RET_OK;
}

IMsdp* DeviceStatusMsdpClientImpl::GetMockInst(Type type)
{
    CALL_DEBUG_ENTER;
    CHKPP(mock_.handle);
    if (type > TYPE_MAX || type < TYPE_INVALID) {
        FI_HILOGE("Type invalid");
        return nullptr;
    }
    if (mock_.pAlgorithm == nullptr) {
        mock_.pAlgorithm = mock_.create();
        mockCallCounts_[type] = 0;
    }
    return mock_.pAlgorithm;
}

ErrCode DeviceStatusMsdpClientImpl::LoadAlgoLibrary()
{
    CALL_DEBUG_ENTER;
    std::unique_lock lock(mutex_);
    if (algo_.handle != nullptr) {
        FI_HILOGE("Algo handle has exists");
        return RET_OK;
    }
    std::string dlName = DEVICESTATUS_ALGO_LIB_PATH;
    char libRealPath[PATH_MAX] = { 0 };
    if (realpath(dlName.c_str(), libRealPath) == nullptr) {
        FI_HILOGE("Get absolute algoPath is error, errno:%{public}d", errno);
        return RET_ERR;
    }
    algo_.handle = dlopen(libRealPath, RTLD_LAZY);
    if (algo_.handle == nullptr) {
        FI_HILOGE("Cannot load library error:%{public}s", dlerror());
        return RET_ERR;
    }
    FI_HILOGI("Start create pointer");
    algo_.create = reinterpret_cast<LoadMockLibraryFunc>(dlsym(algo_.handle, "Create"));
    FI_HILOGI("Start destroy pointer");
    algo_.destroy = reinterpret_cast<LoadMockLibraryPtr>(dlsym(algo_.handle, "Destroy"));
    if ((algo_.create == nullptr) || (algo_.destroy == nullptr)) {
        FI_HILOGE("%{public}s dlsym Create or Destroy failed",
            dlName.c_str());
        dlclose(algo_.handle);
        algo_.Clear();
        FI_HILOGE("Load algo failed");
        return RET_ERR;
    }
    return RET_OK;
}

ErrCode DeviceStatusMsdpClientImpl::UnloadAlgoLibrary()
{
    std::unique_lock lifecycle(algoLifecycleMutex_, std::try_to_lock);
    if (!lifecycle.owns_lock()) { return RET_ERR; }
    return UnloadAlgoLibraryLocked();
}

void DeviceStatusMsdpClientImpl::RetireAlgo()
{
    std::lock_guard lifecycle(algoLifecycleMutex_);
    if (UnloadAlgoLibraryLocked() != RET_OK) {
        FI_HILOGE("Algorithm teardown incomplete; retaining mapped module");
    }
    algoRetiring_ = false;
}

ErrCode DeviceStatusMsdpClientImpl::UnloadAlgoLibraryLocked()
{
    if (!algoCallCounts_.empty()) { return RET_ERR; }
    if (iAlgo_ != nullptr) {
        if (iAlgo_->DisableCount(TYPE_INVALID) != RET_OK) { return RET_ERR; }
        if (iAlgo_->UnregisterCallback() != RET_OK) { return RET_ERR; }
    }
    std::unique_lock lock(mutex_);
    if (algo_.handle == nullptr) { return RET_OK; }
    if (algo_.pAlgorithm != nullptr) {
        algo_.destroy(algo_.pAlgorithm);
        algo_.pAlgorithm = nullptr;
    }
    iAlgo_ = nullptr;
    if (dlclose(algo_.handle) != 0) { return RET_ERR; }
    algo_.Clear();
    return RET_OK;
}

IMsdp* DeviceStatusMsdpClientImpl::GetAlgoInst(Type type)
{
    CALL_DEBUG_ENTER;
    (void)type;
    std::unique_lock lock(mutex_);
    CHKPP(algo_.handle);
    if (algo_.pAlgorithm == nullptr) {
        algo_.pAlgorithm = algo_.create();
    }
    CHKPP(algo_.pAlgorithm);
    return algo_.pAlgorithm;
}
} // namespace DeviceStatus
} // namespace Msdp
} // namespace OHOS
