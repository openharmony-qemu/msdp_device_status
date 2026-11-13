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

#ifndef SENSOR_DATA_CALLBACK_H
#define SENSOR_DATA_CALLBACK_H

#ifdef DEVICE_STATUS_SENSOR_ENABLE
#include <atomic>
#include <condition_variable>
#include <memory>
#include <vector>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include "singleton.h"
#include "sensor_agent.h"
#include "sensor_agent_type.h"

#include "devicestatus_data_define.h"

namespace OHOS {
namespace Msdp {
namespace DeviceStatus {
class SensorDataCallback final {
    DECLARE_SINGLETON(SensorDataCallback);
public:
    bool RegisterCallbackSensor(int32_t sensorTypeId);
    bool UnregisterCallbackSensor(int32_t sensorTypeId);
    void Init();
    bool Unregister();
    bool SubscribeSensorEvent(int32_t sensorTypeId, SensorCallback callback, std::shared_ptr<void> owner = nullptr);
    bool UnsubscribeSensorEvent(int32_t sensorTypeId, SensorCallback callback);
    bool PushData(int32_t sensorTypeId, uint8_t* data);

private:
    struct Sample { uint64_t sequence; AccelData data; };
    struct Slot {
        SensorCallback callback;
        std::shared_ptr<void> owner;
        uint64_t firstSequence { 0 };
        bool active { true };
        size_t inFlight { 0 };
        std::condition_variable drained;
    };
    bool PopData(int32_t sensorTypeId, AccelData& data, uint64_t& sequence);
    void AlgorithmLoop();
    void HandleSensorEvent();
    bool NotifyCallback(int32_t sensorTypeId, AccelData* data, uint64_t sequence);
    bool StopPhysicalLocked();

    struct SensorUser user_ {};
    std::list<Sample> accelDataList_;
    std::unique_ptr<std::thread> algorithmThread_ { nullptr };
    std::condition_variable dataReady_;
    std::mutex callbackMutex_;
    std::mutex dataMutex_;
    std::mutex initMutex_;
    std::mutex sensorMutex_;
    std::atomic<bool> alive_ { false };
    bool workerStopping_ { false };
    bool accepting_ { false };
    bool subscribed_ { false };
    bool activated_ { false };
    size_t physicalUsers_ { 0 };
    uint64_t sampleSequence_ { 0 };
    std::map<int32_t, std::shared_ptr<Slot>> algoMap_;
};
#define SENSOR_DATA_CB OHOS::Singleton<SensorDataCallback>::GetInstance()
} // namespace DeviceStatus
} // namespace Msdp
} // namespace OHOS
#endif // DEVICE_STATUS_SENSOR_ENABLE
#endif // SENSOR_DATA_CALLBACK_H

