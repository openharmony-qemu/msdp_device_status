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
#include "sensor_data_callback.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "devicestatus_define.h"
#include "include/util.h"

#undef LOG_TAG
#define LOG_TAG "SensorDataCallback"

namespace OHOS {
namespace Msdp {
namespace DeviceStatus {
namespace {
constexpr int32_t RATE_MILLISEC { 100100100 };
thread_local const SensorDataCallback* currentSensorCallback = nullptr;
constexpr size_t MAX_PENDING_ACCEL_SAMPLES { 256 };
} // namespace

SensorDataCallback::SensorDataCallback() {}
SensorDataCallback::~SensorDataCallback()
{
    {
        std::lock_guard lock(callbackMutex_);
        for (auto& entry : algoMap_) { entry.second->active = false; }
        algoMap_.clear();
    }
    {
        std::lock_guard lock(sensorMutex_);
        physicalUsers_ = 0;
        StopPhysicalLocked();
    }
    Unregister();
}

void SensorDataCallback::Init()
{
    std::lock_guard lock(initMutex_);
    if (workerStopping_ || algorithmThread_ != nullptr) { return; }
    {
        std::lock_guard dataLock(dataMutex_);
        alive_ = true;
        accelDataList_.clear();
    }
    algorithmThread_ = std::make_unique<std::thread>([this] { AlgorithmLoop(); });
}

bool SensorDataCallback::Unregister()
{
    { std::lock_guard lock(callbackMutex_); if (!algoMap_.empty()) { return false; } }
    {
        std::lock_guard lock(sensorMutex_);
        if (physicalUsers_ != 0 || !StopPhysicalLocked()) { return false; }
    }
    std::unique_ptr<std::thread> retired;
    {
        std::lock_guard lock(initMutex_);
        if (workerStopping_) { return false; }
        if (algorithmThread_ == nullptr) { return true; }
        if (algorithmThread_->get_id() == std::this_thread::get_id()) { return false; }
        workerStopping_ = true;
        { std::lock_guard dataLock(dataMutex_); alive_ = false; accelDataList_.clear(); }
        dataReady_.notify_all();
        retired = std::move(algorithmThread_);
    }
    if (retired->joinable()) { retired->join(); }
    { std::lock_guard lock(initMutex_); workerStopping_ = false; }
    return true;
}

bool SensorDataCallback::SubscribeSensorEvent(int32_t sensorTypeId, SensorCallback callback,
    std::shared_ptr<void> owner)
{
    if (!callback || owner == nullptr) { return false; }
    auto slot = std::make_shared<Slot>();
    slot->callback = std::move(callback);
    slot->owner = std::move(owner);
    std::scoped_lock lock(callbackMutex_, dataMutex_);
    if (!alive_ || sampleSequence_ == UINT64_MAX) { return false; }
    slot->firstSequence = sampleSequence_ + 1;
    return algoMap_.emplace(sensorTypeId, std::move(slot)).second;
}

bool SensorDataCallback::UnsubscribeSensorEvent(int32_t sensorTypeId, SensorCallback callback)
{
    (void)callback;
    std::unique_lock lock(callbackMutex_);
    auto found = algoMap_.find(sensorTypeId);
    if (found == algoMap_.end()) { return false; }
    auto slot = found->second;
    slot->active = false;
    // The callback itself holds Slot::owner until it returns; never wait on self.
    // Timeout leaves the inactive slot/owner registered so teardown can be retried.
    if (currentSensorCallback != this && !slot->drained.wait_for(lock, std::chrono::seconds(1),
        [&slot] { return slot->inFlight == 0; })) {
        return false;
    }
    algoMap_.erase(found);
    return true;
}

bool SensorDataCallback::NotifyCallback(int32_t sensorTypeId, AccelData* data, uint64_t sequence)
{
    CHKPF(data);
    std::vector<std::shared_ptr<Slot>> slots;
    {
        std::lock_guard lock(callbackMutex_);
        for (const auto& entry : algoMap_) { slots.push_back(entry.second); }
    }
    for (const auto& slot : slots) {
        {
            std::lock_guard lock(callbackMutex_);
            if (!slot->active || sequence < slot->firstSequence) { continue; }
            ++slot->inFlight;
        }
        auto previous = currentSensorCallback;
        currentSensorCallback = this;
        slot->callback(sensorTypeId, data);
        currentSensorCallback = previous;
        {
            std::lock_guard lock(callbackMutex_);
            --slot->inFlight;
            slot->drained.notify_all();
        }
    }
    return true;
}

bool SensorDataCallback::PushData(int32_t sensorTypeId, uint8_t* data)
{
    CALL_DEBUG_ENTER;
    CHKPF(data);
    if (sensorTypeId != SENSOR_TYPE_ID_ACCELEROMETER) {
        return false;
    }
    AccelData sample;
    std::memcpy(&sample, data, sizeof(sample));
    const AccelData* acclData = &sample;
    if (!std::isfinite(acclData->x) || !std::isfinite(acclData->y) || !std::isfinite(acclData->z) ||
        std::fabs(acclData->x) > ACC_VALID_THRHD || std::fabs(acclData->y) > ACC_VALID_THRHD ||
        std::fabs(acclData->z) > ACC_VALID_THRHD) {
        FI_HILOGE("Acc data is invalid");
        return false;
    }
    {
        std::lock_guard lock(dataMutex_);
        if (!accepting_ || !alive_ || sampleSequence_ == UINT64_MAX) { return false; }
        if (accelDataList_.size() >= MAX_PENDING_ACCEL_SAMPLES) {
            FI_HILOGE("Accelerometer input queue is full");
            return false;
        }
        accelDataList_.push_back({++sampleSequence_, *acclData});
        FI_HILOGD("ACCEL pushData:x:%{public}f, y:%{public}f, z:%{public}f, PushData sensorTypeId:%{public}d",
            acclData->x, acclData->y, acclData->z, sensorTypeId);
    }
    dataReady_.notify_one();
    return true;
}

bool SensorDataCallback::PopData(int32_t sensorTypeId, AccelData& data, uint64_t& sequence)
{
    CALL_DEBUG_ENTER;
    if (sensorTypeId != SENSOR_TYPE_ID_ACCELEROMETER) {
        FI_HILOGE("Invalid sensorTypeId:%{public}d", sensorTypeId);
        return false;
    }
    std::lock_guard lock(dataMutex_);
    if (accelDataList_.empty()) {
        FI_HILOGE("No accel data");
        return false;
    }
    sequence = accelDataList_.front().sequence;
    data = accelDataList_.front().data;
    accelDataList_.pop_front();
    FI_HILOGD("ACCEL popData:x:%{public}f, y:%{public}f, z:%{public}f, PopData sensorTypeId:%{public}d",
        data.x, data.y, data.z, sensorTypeId);
    return true;
}

static void SensorDataCallbackImpl(SensorEvent *event)
{
    CALL_DEBUG_ENTER;
    CHKPV(event);
    if (event->sensorTypeId != SENSOR_TYPE_ID_ACCELEROMETER || event->data == nullptr ||
        event->dataLen != static_cast<decltype(event->dataLen)>(sizeof(AccelData))) {
        FI_HILOGE("Invalid accelerometer event envelope");
        return;
    }
    FI_HILOGI("SensorDataCallbackImpl sensorTypeId:%{public}d", event->sensorTypeId);
    SENSOR_DATA_CB.PushData(event->sensorTypeId, event->data);
}

bool SensorDataCallback::RegisterCallbackSensor(int32_t sensorTypeId)
{
    if (sensorTypeId != SENSOR_TYPE_ID_ACCELEROMETER) { return false; }
    std::lock_guard lock(sensorMutex_);
    if (physicalUsers_ != 0) { ++physicalUsers_; return true; }
    if (!alive_ || !StopPhysicalLocked()) { return false; }
    user_.callback = SensorDataCallbackImpl;
    if (SubscribeSensor(sensorTypeId, &user_) != 0) { return false; }
    subscribed_ = true;
    if (SetBatch(sensorTypeId, &user_, RATE_MILLISEC, RATE_MILLISEC) != 0) {
        StopPhysicalLocked(); return false;
    }
    if (ActivateSensor(sensorTypeId, &user_) != 0) {
        // Activate can partially register even on error; attempt exact rollback.
        activated_ = true;
        StopPhysicalLocked(); return false;
    }
    activated_ = true;
    physicalUsers_ = 1;
    { std::lock_guard dataLock(dataMutex_); accepting_ = true; }
    return true;
}

bool SensorDataCallback::UnregisterCallbackSensor(int32_t sensorTypeId)
{
    if (sensorTypeId != SENSOR_TYPE_ID_ACCELEROMETER) { return false; }
    std::lock_guard lock(sensorMutex_);
    if (physicalUsers_ > 1) { --physicalUsers_; return true; }
    physicalUsers_ = 0;
    {
        std::lock_guard dataLock(dataMutex_);
        accepting_ = false;
        alive_ = false;
        accelDataList_.clear();
    }
    dataReady_.notify_all();
    return StopPhysicalLocked();
}

bool SensorDataCallback::StopPhysicalLocked()
{
    if (activated_ && DeactivateSensor(SENSOR_TYPE_ID_ACCELEROMETER, &user_) == 0) { activated_ = false; }
    if (subscribed_ && UnsubscribeSensor(SENSOR_TYPE_ID_ACCELEROMETER, &user_) == 0) { subscribed_ = false; }
    return !activated_ && !subscribed_;
}

void SensorDataCallback::AlgorithmLoop()
{
    SetThreadName(std::string("os_loop_sensor"));
    for (;;) {
        {
            std::unique_lock lock(dataMutex_);
            dataReady_.wait(lock, [this] { return !alive_ || !accelDataList_.empty(); });
            if (!alive_) { return; }
        }
        HandleSensorEvent();
    }
}

void SensorDataCallback::HandleSensorEvent()
{
    AccelData data;
    uint64_t sequence = 0;
    if (PopData(SENSOR_TYPE_ID_ACCELEROMETER, data, sequence)) {
        NotifyCallback(SENSOR_TYPE_ID_ACCELEROMETER, &data, sequence);
    }
}
} // namespace DeviceStatus
} // namespace Msdp
} // namespace OHOS
#endif // DEVICE_STATUS_SENSOR_ENABLE
