/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <algorithm>
#include <chrono>
#include <queue>
#include <map>
#include <sstream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cJSON.h>
#include <fstream>
#include <memcached/isotime.h>
#include <iostream>

#include "auditd.h"
#include "audit.h"
#include "event.h"
#include "configureevent.h"
#include "auditd_audit_events.h"
#include "eventdescriptor.h"

EXTENSION_LOGGER_DESCRIPTOR* Audit::logger = NULL;
std::string Audit::hostname;
void (*Audit::notify_io_complete)(gsl::not_null<const void*> cookie,
                                  ENGINE_ERROR_CODE status);

void Audit::log_error(const AuditErrorCode return_code,
                      const std::string& string) {
    switch (return_code) {
    case AuditErrorCode::AUDIT_EXTENSION_DATA_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: audit extension data error");
        break;
    case AuditErrorCode::FILE_OPEN_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: open error on file %s: %s", string.c_str(),
                    strerror(errno));
        break;
    case AuditErrorCode::FILE_RENAME_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: rename error on file %s: %s", string.c_str(),
                    strerror(errno));
        break;
    case AuditErrorCode::FILE_REMOVE_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: remove error on file %s: %s", string.c_str(),
                    strerror(errno));
        break;
    case AuditErrorCode::MEMORY_ALLOCATION_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: memory allocation error: %s", string.c_str());
        break;
    case AuditErrorCode::JSON_PARSING_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON parsing error on string \"%s\"",
                    string.c_str());
        break;
    case AuditErrorCode::JSON_MISSING_DATA_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON missing data error");
        break;
    case AuditErrorCode::JSON_MISSING_OBJECT_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON missing object error");
        break;
    case AuditErrorCode::JSON_KEY_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON key \"%s\" error", string.c_str());
        break;
    case AuditErrorCode::JSON_ID_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON eventid error");
        break;
    case AuditErrorCode::JSON_UNKNOWN_FIELD_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: JSON unknown field error");
        break;
    case AuditErrorCode::CB_CREATE_THREAD_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: cb create thread error");
        break;
    case AuditErrorCode::EVENT_PROCESSING_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: event processing error");
        break;
    case AuditErrorCode::PROCESSING_EVENT_FIELDS_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: processing events field error");
        break;
    case AuditErrorCode::TIMESTAMP_MISSING_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: timestamp missing error");
        break;
    case AuditErrorCode::TIMESTAMP_FORMAT_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: timestamp format error on string \"%s\"",
                    string.c_str());
        break;
    case AuditErrorCode::EVENT_ID_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL, "Audit: eventid error");
        break;
    case AuditErrorCode::VERSION_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL, "Audit: audit version error");
        break;
    case AuditErrorCode::VALIDATE_PATH_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: validate path \"%s\" error", string.c_str());
        break;
    case AuditErrorCode::ROTATE_INTERVAL_BELOW_MIN_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: rotate_interval below minimum error");
        break;
    case AuditErrorCode::ROTATE_INTERVAL_EXCEEDS_MAX_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: rotate_interval exceeds maximum error");
        break;
    case AuditErrorCode::OPEN_AUDITFILE_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error opening audit file");
        break;
    case AuditErrorCode::SETTING_AUDITFILE_OPEN_TIME_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error: setting auditfile open time = %s",
                    string.c_str());
        break;
    case AuditErrorCode::WRITING_TO_DISK_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: writing to disk error: %s", string.c_str());
        break;
    case AuditErrorCode::WRITE_EVENT_TO_DISK_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error writing event to disk");
        break;
    case AuditErrorCode::UNKNOWN_EVENT_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error: unknown event %s", string.c_str());
        break;
    case AuditErrorCode::CONFIG_INPUT_ERROR:
        if (!string.empty()) {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Audit: error reading config: %s", string.c_str());
        } else {
            logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Audit: error reading config");
        }
        break;
    case AuditErrorCode::CONFIGURATION_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error performing configuration");
        break;
    case AuditErrorCode::MISSING_AUDIT_EVENTS_FILE_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error: missing audit_event.json from \"%s\"",
                    string.c_str());
        break;
    case AuditErrorCode::ROTATE_INTERVAL_SIZE_TOO_BIG:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error: rotation_size too big: %s", string.c_str());
        break;
    case AuditErrorCode::AUDIT_DIRECTORY_DONT_EXIST:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error: %s does not exists", string.c_str());
        break;
    case AuditErrorCode::INITIALIZATION_ERROR:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: error during initialization: %s", string.c_str());
        break;
    default:
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: unknown error code:%d with string:%s",
                    return_code, string.c_str());
        break;
    }
}


std::string Audit::load_file(const char *file) {
    std::ifstream myfile(file, std::ios::in | std::ios::binary);
    if (myfile.is_open()) {
        std::string str((std::istreambuf_iterator<char>(myfile)),
                        std::istreambuf_iterator<char>());
        myfile.close();
        return str;
    } else {
        Audit::log_error(AuditErrorCode::FILE_OPEN_ERROR, file);
        std::string str;
        return str;
    }
}


bool Audit::create_audit_event(uint32_t event_id, cJSON *payload) {
    // Add common fields to the audit event
    cJSON_AddStringToObject(payload, "timestamp",
                            ISOTime::generatetimestamp().c_str());
    cJSON *real_userid = cJSON_CreateObject();
    cJSON_AddStringToObject(real_userid, "source", "internal");
    cJSON_AddStringToObject(real_userid, "user", "couchbase");
    cJSON_AddItemToObject(payload, "real_userid", real_userid);

    switch (event_id) {
        case AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON:
            cJSON_AddBoolToObject(payload, "auditd_enabled",
                                  config.is_auditd_enabled());
            cJSON_AddStringToObject(payload, "descriptors_path",
                                    config.get_descriptors_path().c_str());
            cJSON_AddStringToObject(payload, "hostname", hostname.c_str());
            cJSON_AddStringToObject(payload, "log_path",
                                    config.get_log_directory().c_str());
            cJSON_AddNumberToObject(payload, "rotate_interval",
                                    config.get_rotate_interval());
            cJSON_AddNumberToObject(payload, "version", 1.0);
            break;

        case AUDITD_AUDIT_SHUTTING_DOWN_AUDIT_DAEMON:
            break;

        default:
            log_error(AuditErrorCode::EVENT_ID_ERROR);
            return false;
    }
    return true;
}


bool Audit::initialize_event_data_structures(cJSON *event_ptr) {
    if (event_ptr == nullptr) {
        log_error(AuditErrorCode::JSON_MISSING_DATA_ERROR);
        return false;
    }

    try {
        auto *entry = new EventDescriptor(event_ptr);
        if (config.is_event_disabled(entry->getId())) {
            entry->setEnabled(false);
        }
        events.insert(std::pair<uint32_t, EventDescriptor*>(entry->getId(), entry));
    } catch (std::bad_alloc& ba) {
        log_error(AuditErrorCode::MEMORY_ALLOCATION_ERROR, ba.what());
        return false;
    } catch (std::logic_error& le) {
        log_error(AuditErrorCode::JSON_KEY_ERROR, le.what());
    }

    return true;
}


bool Audit::process_module_data_structures(cJSON *module) {
    if (module == NULL) {
        log_error(AuditErrorCode::JSON_MISSING_OBJECT_ERROR);
        return false;
    }
    while (module != NULL) {
        cJSON *mod_ptr = module->child;
        if (mod_ptr == NULL) {
            log_error(AuditErrorCode::JSON_MISSING_DATA_ERROR);
            return false;
        }
        while (mod_ptr != NULL) {
            cJSON *event_ptr;
            switch (mod_ptr->type) {
                case cJSON_Number:
                case cJSON_String:
                    break;
                case cJSON_Array:
                    event_ptr = mod_ptr->child;
                    while (event_ptr != NULL) {
                        if (!initialize_event_data_structures(event_ptr)) {
                            return false;
                        }
                        event_ptr = event_ptr->next;
                    }
                    break;
                default:
                    log_error(AuditErrorCode::JSON_UNKNOWN_FIELD_ERROR);
                    return false;
            }
            mod_ptr = mod_ptr->next;
        }
        module = module->next;
    }
    return true;
}


bool Audit::process_module_descriptor(cJSON *module_descriptor) {
    clear_events_map();
    while(module_descriptor != NULL) {
        switch (module_descriptor->type) {
            case cJSON_Number:
                break;
            case cJSON_Array:
                if (!process_module_data_structures(module_descriptor->child)) {
                    return false;
                }
                break;
            default:
                log_error(AuditErrorCode::JSON_UNKNOWN_FIELD_ERROR);
                return false;
        }
        module_descriptor = module_descriptor->next;
    }
    return true;
}


bool Audit::configure(void) {
    bool is_enabled_before_reconfig = config.is_auditd_enabled();
    std::string configuration = load_file(configfile.c_str());
    if (configuration.empty()) {
        return false;
    }

    cJSON *config_json = cJSON_Parse(configuration.c_str());
    if (config_json == NULL) {
        log_error(AuditErrorCode::JSON_PARSING_ERROR, configuration);
        return false;
    }

    bool failure = false;
    try {
        config.initialize_config(config_json);
    } catch (std::pair<AuditErrorCode, char*>& exc) {
        log_error(exc.first, exc.second);
        failure = true;
    } catch (std::pair<AuditErrorCode, const char *>& exc) {
        log_error(exc.first, exc.second);
        failure = true;
    } catch (std::string &msg) {
        log_error(AuditErrorCode::CONFIG_INPUT_ERROR, msg);
        failure = true;
    } catch (...) {
        log_error(AuditErrorCode::CONFIG_INPUT_ERROR);
        failure = true;
    }
    cJSON_Delete(config_json);
    if (failure) {
        return false;
    }

    if (!auditfile.is_open()) {
        try {
            auditfile.cleanup_old_logfile(config.get_log_directory());
        } catch (std::string &str) {
            logger->log(EXTENSION_LOG_WARNING, NULL, "%s", str.c_str());
            return false;
        }
    }
    std::stringstream audit_events_file;
    audit_events_file << config.get_descriptors_path();
    audit_events_file << DIRECTORY_SEPARATOR_CHARACTER << "audit_events.json";
    std::string str = load_file(audit_events_file.str().c_str());
    if (str.empty()) {
        return false;
    }
    cJSON *json_ptr = cJSON_Parse(str.c_str());
    if (json_ptr == NULL) {
        Audit::log_error(AuditErrorCode::JSON_PARSING_ERROR, str);
        return false;
    }
    if (!process_module_descriptor(json_ptr->child)) {
        cJSON_Delete(json_ptr);
        return false;
    }
    cJSON_Delete(json_ptr);

    auditfile.reconfigure(config);

    // iterate through the events map and update the sync and enabled flags
    typedef std::map<uint32_t, EventDescriptor*>::iterator it_type;
    for(it_type iterator = events.begin(); iterator != events.end(); iterator++) {
        iterator->second->setSync(config.is_event_sync(iterator->first));
        if (config.is_event_disabled(iterator->first)) {
            iterator->second->setEnabled(false);
        }
    }

    if (is_enabled_before_reconfig != config.is_auditd_enabled()) {
        notify_event_state_changed(0, config.is_auditd_enabled());
    }

    // create event to say done reconfiguration
    if (is_enabled_before_reconfig || config.is_auditd_enabled()) {
        auto evt = events.find(AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON);
        if (evt == events.end()) {
            std::ostringstream convert;
            convert << AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON;
            Audit::log_error(AuditErrorCode::UNKNOWN_EVENT_ERROR,
                             convert.str().c_str());
        } else {
            if (evt->second->isEnabled()) {
                cJSON *payload = cJSON_CreateObject();
                if (payload == nullptr) {
                    return false;
                }
                if (create_audit_event(AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON,
                                       payload)) {
                    cJSON_AddNumberToObject(payload, "id",
                                            AUDITD_AUDIT_CONFIGURED_AUDIT_DAEMON);
                    cJSON_AddStringToObject(payload, "name",
                                            evt->second->getName().c_str());
                    cJSON_AddStringToObject(payload, "description",
                                            evt->second->getDescription().c_str());

                    if (!(auditfile.ensure_open() && auditfile.write_event_to_disk(payload))) {
                        dropped_events++;
                    }
                } else {
                    dropped_events++;
                }
                cJSON_Delete(payload);
            }
        }
    }

    if (!config.is_auditd_enabled()) {
        // Audit is disabled, ensure that the audit file is closed
        auditfile.close();
    }

    return true;
}


bool Audit::add_to_filleventqueue(const uint32_t event_id,
                                  const char *payload,
                                  const size_t length) {
    // @todo I think we should do full validation of the content
    //       in debug mode to ensure that developers actually fill
    //       in the correct fields.. if not we should add an
    //       event to the audit trail saying it is one in an illegal
    //       format (or missing fields)
    bool res;
    Event* new_event = new Event(event_id, payload, length);
    cb_mutex_enter(&producer_consumer_lock);
    if (filleventqueue->size() < max_audit_queue) {
        filleventqueue->push(new_event);
        cb_cond_broadcast(&events_arrived);
        res = true;
    } else {
        logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Audit: Dropping audit event %u: %s",
                    new_event->id, new_event->payload.c_str());
        dropped_events++;
        delete new_event;
        res = false;
    }
    cb_mutex_exit(&producer_consumer_lock);
    return res;
}


bool Audit::add_reconfigure_event(const char* configfile, const void *cookie) {
    ConfigureEvent* new_event = new ConfigureEvent(configfile, cookie);
    cb_mutex_enter(&producer_consumer_lock);
    filleventqueue->push(new_event);
    cb_cond_broadcast(&events_arrived);
    cb_mutex_exit(&producer_consumer_lock);
    return true;
}


void Audit::clear_events_map(void) {
    typedef std::map<uint32_t, EventDescriptor*>::iterator it_type;
    for(it_type iterator = events.begin(); iterator != events.end(); iterator++) {
        delete iterator->second;
    }
    events.clear();
}


void Audit::clear_events_queues(void) {
    while(!processeventqueue->empty()) {
        Event *event = processeventqueue->front();
        processeventqueue->pop();
        delete event;
    }
    while(!filleventqueue->empty()) {
        Event *event = filleventqueue->front();
        filleventqueue->pop();
        delete event;
    }
}

bool Audit::terminate_consumer_thread(void)
{
    cb_mutex_enter(&producer_consumer_lock);
    terminate_audit_daemon = true;
    cb_mutex_exit(&producer_consumer_lock);

    /* The consumer thread maybe waiting for an event
     * to arrive so we need to send it a broadcast so
     * it can exit cleanly.
     */
    cb_mutex_enter(&producer_consumer_lock);
    cb_cond_broadcast(&events_arrived);
    cb_mutex_exit(&producer_consumer_lock);
    if (consumer_thread_running.load()) {
        if (cb_join_thread(consumer_tid) == 0) {
            consumer_thread_running.store(false);
            return true;
        }
        return false;
    } else {
        return false;
    }
}

bool Audit::clean_up(void) {
    /* clean_up is called from shutdown_auditdaemon
     * which in turn is called from memcached when
     * performing a graceful shutdown.
     *
     * However clean_up is also called from ~Audit().
     * This is required because it possible for the
     * destructor to be invoked without going through
     * the memcached graceful shutdown.
     *
     * We therefore first check to see if the
     * terminate_audit_daemon flag has not been set
     * before trying to terminate the consumer thread.
     */
    if (!terminate_audit_daemon) {
        if (terminate_consumer_thread()) {
            consumer_tid = cb_thread_self();
        } else {
            return false;
        }
        clear_events_map();
        clear_events_queues();
    }
    return true;
}

void Audit::notify_all_event_states() {
    notify_event_state_changed(0, config.is_auditd_enabled());
    for (const auto& event : events) {
        notify_event_state_changed(event.first, event.second->isEnabled());
    }
}

void Audit::add_event_state_listener(cb::audit::EventStateListener listener) {
    std::lock_guard<std::mutex> guard(event_state_listener.mutex);
    event_state_listener.clients.push_back(listener);
}

void Audit::notify_event_state_changed(uint32_t id, bool enabled) const {
    std::lock_guard<std::mutex> guard(event_state_listener.mutex);
    for (const auto& func : event_state_listener.clients) {
        func(id, enabled);
    }
}
