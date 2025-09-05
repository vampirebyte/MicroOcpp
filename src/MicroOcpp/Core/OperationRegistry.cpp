// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Core/OperationRegistry.h>
#include <MicroOcpp/Core/Operation.h>
#include <MicroOcpp/Core/Request.h>
#include <MicroOcpp/Core/OcppError.h>
#include <MicroOcpp/Debug.h>
#include <algorithm>

using namespace MicroOcpp;

OperationRegistry::OperationRegistry() : registry(makeVector<OperationCreator>("OperationRegistry")) {

}

OperationCreator *OperationRegistry::findCreator(const char *operationType) {
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (!strcmp(it->operationType, operationType)) {
            return &*it;
        }
    }
    return nullptr;
}

void OperationRegistry::registerOperation(const char *operationType, std::function<Operation*()> creator) {
    registry.erase(std::remove_if(registry.begin(), registry.end(),
                [operationType] (const OperationCreator& el) {
                    return !strcmp(operationType, el.operationType);
                }),
            registry.end());
    
    OperationCreator entry;
    entry.operationType = operationType;
    entry.creator = creator;

    registry.push_back(entry);

    MO_DBG_DEBUG("registered operation %s", operationType);
}

void OperationRegistry::setOnRequest(const char *operationType, OnReceiveReqListener onRequest) {
    if (auto entry = findCreator(operationType)) {
        entry->onRequest = onRequest;
    } else {
        MO_DBG_ERR("%s not registered", operationType);
    }
}

void OperationRegistry::setOnResponse(const char *operationType, OnSendConfListener onResponse) {
    if (auto entry = findCreator(operationType)) {
        entry->onResponse = onResponse;
    } else {
        MO_DBG_ERR("%s not registered", operationType);
    }
}

void OperationRegistry::setOnReceiveConf(const char *operationType, OnReceiveConfListener onReceiveConf) {
    if (auto entry = findCreator(operationType)) {
        entry->onReceiveConf = onReceiveConf;
    MO_DBG_DEBUG("registered onReceiveConf handler for %s", operationType);
    } else {
        MO_DBG_ERR("%s not registered", operationType);
    }
}

std::unique_ptr<Request> OperationRegistry::deserializeOperation(const char *operationType) {
    
    if (auto entry = findCreator(operationType)) {
        auto payload = entry->creator();
        if (payload) {
            auto result = std::unique_ptr<Request>(new Request(
                                std::unique_ptr<Operation>(payload)));
            result->setOnReceiveReqListener(entry->onRequest);
            result->setOnSendConfListener(entry->onResponse);
            // Attach generic onReceiveConf listener for outgoing request confirmations
            result->setOnReceiveConfListener(entry->onReceiveConf);
            if (entry->onReceiveConf) {
                MO_DBG_DEBUG("attached onReceiveConf handler to new %s request", entry->operationType);
            }
            return result;
        }
    }

    return std::unique_ptr<Request>(new Request(
                std::unique_ptr<Operation>(new NotImplemented())));
}

void OperationRegistry::debugPrint() {
    for (auto& creator : registry) {
        MO_CONSOLE_PRINTF("[OCPP]     > %s\n", creator.operationType);
    }
}
