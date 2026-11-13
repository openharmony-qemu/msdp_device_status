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

#ifdef DEVICE_STATUS_SENSOR_ENABLE
#include "devicestatus_algorithm_manager.h"
#include <climits>

#include <cerrno>
#include <string>

#include <linux/netlink.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "devicestatus_common.h"
#include "devicestatus_define.h"

#undef LOG_TAG
#define LOG_TAG "AlgoMgr"

namespace OHOS {
namespace Msdp {
namespace DeviceStatus {

bool AlgoMgr::StartSensor(Type type)
{
    CALL_DEBUG_ENTER;
    int32_t sensorType = GetSensorTypeId(type);
    if (sensorType == RET_ERR) {
        FI_HILOGE("Failed to get sensorType");
        return false;
    }
    if (!CheckSensorTypeId(sensorType)) {
        FI_HILOGE("Sensor type mismatch");
        return false;
    }

    SENSOR_DATA_CB.Init();
    if (!SENSOR_DATA_CB.RegisterCallbackSensor(sensorType)) {
        FI_HILOGE("Failed to register callback sensor");
        return false;
    }

    return true;
}

ErrCode AlgoMgr::RegisterCallback(std::shared_ptr<MsdpAlgoCallback> callback)
{
    if (callback == nullptr) { return RET_ERR; }
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return RET_ERR; }
    if (!callAlgoNums_.empty() && callback_ != callback) { return RET_ERR; }
    callback_ = std::move(callback);
    return RET_OK;
}

ErrCode AlgoMgr::UnregisterCallback()
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !callAlgoNums_.empty()) { return RET_ERR; }
    callback_.reset();
    return RET_OK;
}

bool AlgoMgr::CheckSensorTypeId(int32_t sensorTypeId)
{
    int32_t count = -1;
    SensorInfo *sensorInfo = nullptr;
    int32_t ret = GetAllSensors(&sensorInfo, &count);
    if (ret != 0) {
        FI_HILOGE("Get all sensors failed");
        return false;
    }
    SensorInfo *pt = sensorInfo + count;
    for (SensorInfo *ps = sensorInfo; ps < pt; ++ps) {
        if (ps->sensorTypeId == sensorTypeId) {
            FI_HILOGI("Get sensor sensorTypeId: %{public}d", sensorTypeId);
            return true;
        }
    }
    FI_HILOGE("Get sensor failed");
    return false;
}

int32_t AlgoMgr::GetSensorTypeId(Type type)
{
    FI_HILOGI("Get sensor type: %{public}d", type);
    switch (type) {
        case Type::TYPE_STILL:
        case Type::TYPE_ABSOLUTE_STILL: {
            return SensorTypeId::SENSOR_TYPE_ID_ACCELEROMETER;
        }
        case Type::TYPE_HORIZONTAL_POSITION: {
            return SensorTypeId::SENSOR_TYPE_ID_ACCELEROMETER;
        }
        case Type::TYPE_VERTICAL_POSITION: {
            return SensorTypeId::SENSOR_TYPE_ID_ACCELEROMETER;
        }
        default: {
            FI_HILOGW("GetSensorTypeId failed");
            break;
        }
    }
    return RET_ERR;
}

ErrCode AlgoMgr::Enable(Type type)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || callback_ == nullptr || stoppingTypes_.count(type) != 0) { return RET_ERR; }
    auto existing = callAlgoNums_.find(type);
    if (existing != callAlgoNums_.end()) {
        if (existing->second == INT32_MAX) { return RET_ERR; }
        ++existing->second;
        return RET_OK;
    }
    if (GetSensorTypeId(type) == RET_ERR || !StartSensor(type)) { return RET_ERR; }
    std::shared_ptr<AlgoBase> instance;
    switch (type) {
        case TYPE_STILL: instance = std::make_shared<AlgoAbsoluteStill>(); break;
        case TYPE_ABSOLUTE_STILL: instance = std::make_shared<AlgoAbsoluteStill>(); break;
        case TYPE_HORIZONTAL_POSITION: instance = std::make_shared<AlgoHorizontal>(); break;
        case TYPE_VERTICAL_POSITION: instance = std::make_shared<AlgoVertical>(); break;
        default: UnregisterSensor(type); return RET_ERR;
    }
    instance->RegisterCallback(callback_);
    if (!instance->Init(type)) { UnregisterSensor(type); return RET_ERR; }
    switch (type) {
        case TYPE_STILL: publicStill_ = std::static_pointer_cast<AlgoAbsoluteStill>(instance); break;
        case TYPE_ABSOLUTE_STILL: still_ = std::static_pointer_cast<AlgoAbsoluteStill>(instance); break;
        case TYPE_HORIZONTAL_POSITION: horizontalPosition_ = std::static_pointer_cast<AlgoHorizontal>(instance); break;
        case TYPE_VERTICAL_POSITION: verticalPosition_ = std::static_pointer_cast<AlgoVertical>(instance); break;
        default: return RET_ERR;
    }
    callAlgoNums_[type] = 1;
    return RET_OK;
}

ErrCode AlgoMgr::Disable(Type type)
{
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { return RET_ERR; }
    auto found = callAlgoNums_.find(type);
    if (found == callAlgoNums_.end()) { return RET_ERR; }
    if (found->second > 1) { --found->second; return RET_OK; }
    std::shared_ptr<AlgoBase> instance;
    switch (type) {
        case TYPE_STILL: instance = publicStill_; break;
        case TYPE_ABSOLUTE_STILL: instance = still_; break;
        case TYPE_HORIZONTAL_POSITION: instance = horizontalPosition_; break;
        case TYPE_VERTICAL_POSITION: instance = verticalPosition_; break;
        default: return RET_ERR;
    }
    stoppingTypes_.insert(type);
    if (instance != nullptr && quiescedTypes_.count(type) == 0) {
        if (!SENSOR_DATA_CB.UnsubscribeSensorEvent(type, nullptr)) { return RET_ERR; }
        quiescedTypes_.insert(type);
    }
    // Failed physical teardown remains retryable; never claim it was stopped.
    if (UnregisterSensor(type) != RET_OK) { return RET_ERR; }
    switch (type) {
        case TYPE_STILL: publicStill_.reset(); break;
        case TYPE_ABSOLUTE_STILL: still_.reset(); break;
        case TYPE_HORIZONTAL_POSITION: horizontalPosition_.reset(); break;
        case TYPE_VERTICAL_POSITION: verticalPosition_.reset(); break;
        default: return RET_ERR;
    }
    stoppingTypes_.erase(type);
    quiescedTypes_.erase(type);
    callAlgoNums_.erase(found);
    return RET_OK;
}

ErrCode AlgoMgr::DisableCount(Type type)
{
    (void)type;
    { std::lock_guard lock(mutex_); if (!callAlgoNums_.empty()) { return RET_ERR; } }
    // Called by the service-side retirement worker, outside provider callbacks.
    return SENSOR_DATA_CB.Unregister() ? RET_OK : RET_ERR;
}

ErrCode AlgoMgr::UnregisterSensor(Type type)
{
    int32_t sensorType = GetSensorTypeId(type);
    if (sensorType == RET_ERR) { return RET_ERR; }
    return SENSOR_DATA_CB.UnregisterCallbackSensor(sensorType) ? RET_OK : RET_ERR;
}

extern "C" IMsdp *Create(void)
{
    CALL_DEBUG_ENTER;
    return new (std::nothrow) AlgoMgr();
}

extern "C" void Destroy(const IMsdp* algorithm)
{
    CALL_DEBUG_ENTER;
    if (algorithm != nullptr) {
        FI_HILOGD("algorithm is not nullptr");
        delete algorithm;
    }
}
} // namespace DeviceStatus
} // namespace Msdp
} // namespace OHOS
#endif // DEVICE_STATUS_SENSOR_ENABLE
