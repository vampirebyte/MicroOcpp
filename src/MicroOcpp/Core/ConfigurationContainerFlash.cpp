// matth-x/MicroOcpp
// Copyright Matthias Akstaller 2019 - 2024
// MIT License

#include <MicroOcpp/Core/ConfigurationContainerFlash.h>

#include <algorithm>
#include <MicroOcpp/Core/FilesystemUtils.h>
#include <MicroOcpp/Core/Memory.h>
#include <MicroOcpp/Debug.h>

#define MAX_CONFIGURATIONS 500

namespace MicroOcpp {

class ConfigurationContainerFlash : public ConfigurationContainer, public MemoryManaged {
private:
    Vector<std::shared_ptr<Configuration>> configurations;
    std::shared_ptr<FilesystemAdapter> filesystem;
    uint16_t revisionSum = 0;

    bool loaded = false;

    Vector<char*> keyPool;
    
    void clearKeyPool(const char *key) {
        auto it = keyPool.begin();
        while (it != keyPool.end()) {
            if (!strcmp(*it, key)) {
                MO_DBG_VERBOSE("clear key %s", key);
                MO_FREE(*it);
                it = keyPool.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool configurationsUpdated() {
        auto revisionSum_old = revisionSum;

        revisionSum = 0;
        for (auto& config : configurations) {
            revisionSum += config->getValueRevision();
        }

        return revisionSum != revisionSum_old;
    }
public:
    ConfigurationContainerFlash(std::shared_ptr<FilesystemAdapter> filesystem, const char *filename, bool accessible) :
            ConfigurationContainer(filename, accessible), MemoryManaged("v16.Configuration.ContainerFlash.", filename), configurations(makeVector<std::shared_ptr<Configuration>>(getMemoryTag())), filesystem(filesystem), keyPool(makeVector<char*>(getMemoryTag())) { }

    ~ConfigurationContainerFlash() {
        auto it = keyPool.begin();
        while (it != keyPool.end()) {
            MO_FREE(*it);
            it = keyPool.erase(it);
        }
    }
    
    bool load() override {

        /* ---------------------------------------------------------------
        * 0.  Fast‑exit if this container was already initialised
        * ------------------------------------------------------------ */
        if (loaded) {
            MO_DBG_VERBOSE("[CFG] load(): container already loaded – skip");
            return true;
        }

        if (!filesystem) {
            MO_DBG_ERR("[CFG] load(): filesystem adapter is nullptr");
            return false;
        }

        MO_DBG_VERBOSE("[CFG] load(): start, file=\"%s\"", getFilename());

        /* ---------------------------------------------------------------
        * 1.  Check if the file exists – create an empty skeleton on the
        *     very first boot so that save() has something to overwrite.
        * ------------------------------------------------------------ */
        size_t file_size = 0;
        if (filesystem->stat(getFilename(), &file_size) != 0 || file_size == 0) {
            MO_DBG_VERBOSE("[CFG] load(): file missing or empty (size=%zu) – "
                        "calling save() to create skeleton", file_size);
            return save();
        }

        MO_DBG_VERBOSE("[CFG] load(): file found, size=%zu – reading JSON", file_size);

        /* ---------------------------------------------------------------
        * 2.  Parse JSON document
        * ------------------------------------------------------------ */
        auto doc = FilesystemUtils::loadJson(filesystem, getFilename(), getMemoryTag());
        if (!doc) {
            MO_DBG_ERR("[CFG] load(): JSON parsing failed");
            return false;
        }
        MO_DBG_VERBOSE("[CFG] load(): JSON parsed");

        JsonObject root         = doc->as<JsonObject>();
        JsonObject configHeader = root["head"];

        /* ---------------------------------------------------------------
        * 3.  Header validation
        * ------------------------------------------------------------ */
        const char *ctype = configHeader["content-type"] | "Invalid";
        const char *vers  = configHeader["version"]      | "Invalid";
        MO_DBG_VERBOSE("[CFG] load(): header content‑type=\"%s\", version=\"%s\"",
                    ctype, vers);

        if (strcmp(ctype, "ocpp_config_file") &&
            strcmp(ctype, "ao_configuration_file")) {
            MO_DBG_ERR("[CFG] load(): unknown file format");
            return false;
        }
        if (strcmp(vers, "2.0") && strcmp(vers, "1.1")) {
            MO_DBG_ERR("[CFG] load(): unsupported version \"%s\"", vers);
            return false;
        }

        /* ---------------------------------------------------------------
        * 4.  Iterate through stored configurations
        * ------------------------------------------------------------ */
        JsonArray configurationsArray = root["configurations"];
        MO_DBG_VERBOSE("[CFG] load(): %zu configuration entries",
                    configurationsArray.size());

        if (configurationsArray.size() > MAX_CONFIGURATIONS) {
            MO_DBG_ERR("[CFG] load(): entry count exceeds limit (%u)",
                    MAX_CONFIGURATIONS);
            return false;
        }

        for (JsonObject stored : configurationsArray) {

            /* ---------- Extract key ---------------------------------- */
            const char *key = stored["key"] | "";
            if (!*key) {
                MO_DBG_ERR("[CFG] load(): entry without key – skipped");
                continue;
            }
            MO_DBG_VERBOSE("[CFG] load(): processing key \"%s\"", key);

            /* ---------- Determine type (explicit field or inference) -- */
            TConfig type{};
            bool    has_explicit = stored.containsKey("type");
            bool    ok_type      = has_explicit &&
                                deserializeTConfig(stored["type"], type);

            if (!ok_type) {
                /* Fallback to inference */
                if (stored["value"].is<int>())         type = TConfig::Int;
                else if (stored["value"].is<bool>())   type = TConfig::Bool;
                else                                   type = TConfig::String;
                MO_DBG_VERBOSE("[CFG] load(): inferred type for \"%s\" → %s",
                            key,
                            type == TConfig::Int    ? "Int"    :
                            type == TConfig::Bool   ? "Bool"   :
                                                        "String");
            } else {
                MO_DBG_VERBOSE("[CFG] load(): explicit type for \"%s\" → %s",
                            key, stored["type"].as<const char*>());
            }

            /* ---------- Look for existing Configuration -------------- */
            auto  cfg = getConfiguration(key).get();
            if (cfg && cfg->getType() != type) {
                MO_DBG_ERR("[CFG] load(): type mismatch for \"%s\" – recreating",
                        key);
                remove(cfg);
                cfg = nullptr;
            }

            /* ---------- Create one on‑the‑fly if necessary ----------- */
            char *key_pooled = nullptr;
            if (!cfg) {
            #if MO_ENABLE_HEAP_PROFILER
                char memoryTag[64];
                snprintf(memoryTag, sizeof(memoryTag), "%s%s",
                        "v16.Configuration.", key);
            #else
                const char *memoryTag = nullptr;
            #endif
                key_pooled = static_cast<char *>(MO_MALLOC(memoryTag,
                                                            strlen(key) + 1));
                if (!key_pooled) {
                    MO_DBG_ERR("[CFG] load(): OOM while duplicating key \"%s\"", key);
                    continue;
                }
                strcpy(key_pooled, key);
                cfg = createConfiguration(type, key_pooled).get();
                if (!cfg) {
                    MO_DBG_ERR("[CFG] load(): OOM while creating Configuration "
                            "object for \"%s\"", key);
                    MO_FREE(key_pooled);
                    continue;
                }
                MO_DBG_VERBOSE("[CFG] load(): created new Configuration for \"%s\"",
                            key);
            } else {
                MO_DBG_VERBOSE("[CFG] load(): found existing Configuration for \"%s\"",
                            key);
            }

            /* ---------- Apply the stored value ----------------------- */
            if (!stored.containsKey("value")) {
                MO_DBG_ERR("[CFG] load(): \"%s\" has no value – skipped", key);
                continue;
            }

            bool value_ok = true;
            switch (type) {
                case TConfig::Int:
                    if (stored["value"].is<int>()) cfg->setInt(stored["value"]);
                    else value_ok = false;
                    break;
                case TConfig::Bool:
                    if (stored["value"].is<bool>()) cfg->setBool(stored["value"]);
                    else value_ok = false;
                    break;
                case TConfig::String:
                    if (stored["value"].is<const char*>())
                        cfg->setString(stored["value"]);
                    else value_ok = false;
                    break;
            }
            MO_DBG_VERBOSE("[CFG] load(): value %s for \"%s\"",
                        value_ok ? "applied" : "rejected (type mismatch)", key);

            if (key_pooled) {
                keyPool.push_back(std::move(key_pooled));
            }
        } // for each entry

        /* ---------------------------------------------------------------
        * 5.  Finalise
        * ------------------------------------------------------------ */
        configurationsUpdated();
        loaded = true;

        MO_DBG_DEBUG("[CFG] load(): finished OK – %zu configs in RAM",
                    configurations.size());
        return true;
    }



    bool save() override {

        if (!filesystem) {
            return false;
        }

        if (!configurationsUpdated()) {
            return true; //nothing to be done
        }

        //during mocpp_deinitialize(), key owners are destructed. Don't store if this container is affected
        for (auto& config : configurations) {
            if (!config->getKey()) {
                MO_DBG_DEBUG("don't write back container with destructed key(s)");
                return false;
            }
        }

        size_t jsonCapacity = 2 * JSON_OBJECT_SIZE(2); //head + configurations + head payload
        jsonCapacity += JSON_ARRAY_SIZE(configurations.size()); //configurations array
        jsonCapacity += configurations.size() * JSON_OBJECT_SIZE(3); //config entries in array

        if (jsonCapacity > MO_MAX_JSON_CAPACITY) {
            MO_DBG_ERR("configs JSON exceeds maximum capacity (%s, %zu entries). Crop configs file (by FCFS)", getFilename(), configurations.size());
            jsonCapacity = MO_MAX_JSON_CAPACITY;
        }

        auto doc = initJsonDoc(getMemoryTag(), jsonCapacity);
        JsonObject head = doc.createNestedObject("head");
        head["content-type"] = "ocpp_config_file";
        head["version"] = "2.0";

        JsonArray configurationsArray = doc.createNestedArray("configurations");

        size_t trackCapacity = 0;

        for (size_t i = 0; i < configurations.size(); i++) {
            auto& config = *configurations[i];

            size_t entryCapacity = JSON_OBJECT_SIZE(3) + (JSON_ARRAY_SIZE(2) - JSON_ARRAY_SIZE(1));
            if (trackCapacity + entryCapacity > MO_MAX_JSON_CAPACITY) {
                break;
            }

            trackCapacity += entryCapacity;

            auto stored = configurationsArray.createNestedObject();

            stored["type"] = serializeTConfig(config.getType());
            stored["key"] = config.getKey();
            
            switch (config.getType()) {
                case TConfig::Int:
                    stored["value"] = config.getInt();
                    break;
                case TConfig::Bool:
                    stored["value"] = config.getBool();
                    break;
                case TConfig::String:
                    stored["value"] = config.getString();
                    break;
            }
        }

        bool success = FilesystemUtils::storeJson(filesystem, getFilename(), doc);

        if (success) {
            MO_DBG_DEBUG("Saving configurations finished");
        } else {
            MO_DBG_ERR("could not save configs file: %s", getFilename());
        }

        return success;
    }

    std::shared_ptr<Configuration> createConfiguration(TConfig type, const char *key) override {
        auto res = std::shared_ptr<Configuration>(makeConfiguration(type, key).release(), std::default_delete<Configuration>(), makeAllocator<Configuration>("v16.Configuration.", key));
        if (!res) {
            //allocation failure - OOM
            MO_DBG_ERR("OOM");
            return nullptr;
        }
        configurations.push_back(res);
        return res;
    }

    void remove(Configuration *config) override {
        const char *key = config->getKey();
        configurations.erase(std::remove_if(configurations.begin(), configurations.end(),
            [config] (std::shared_ptr<Configuration>& entry) {
                return entry.get() == config;
            }), configurations.end());
        if (key) {
            clearKeyPool(key);
        }
    }

    size_t size() override {
        return configurations.size();
    }

    Configuration *getConfiguration(size_t i) override {
        return configurations[i].get();
    }

    std::shared_ptr<Configuration> getConfiguration(const char *key) override {
        for (auto& entry : configurations) {
            if (entry->getKey() && !strcmp(entry->getKey(), key)) {
                return entry;
            }
        }
        return nullptr;
    }

    void loadStaticKey(Configuration& config, const char *key) override {
        config.setKey(key);
        clearKeyPool(key);
    }

    void removeUnused() override {
        //if a config's key is still in the keyPool, we know it's unused because it has never been declared in FW (originates from an older FW version)

        auto key = keyPool.begin();
        while (key != keyPool.end()) {

            for (auto config = configurations.begin(); config != configurations.end(); ++config) {
                if ((*config)->getKey() == *key) {
                    MO_DBG_DEBUG("remove unused config %s", (*config)->getKey());
                    configurations.erase(config);
                    break;
                }
            }

            MO_FREE(*key);
            key = keyPool.erase(key);
        }
    }
};

std::unique_ptr<ConfigurationContainer> makeConfigurationContainerFlash(std::shared_ptr<FilesystemAdapter> filesystem, const char *filename, bool accessible) {
    return std::unique_ptr<ConfigurationContainer>(new ConfigurationContainerFlash(filesystem, filename, accessible));
}

} //end namespace MicroOcpp
