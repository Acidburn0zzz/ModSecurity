/*
 * ModSecurity, http://www.modsecurity.org/
 * Copyright (c) 2015 Trustwave Holdings, Inc. (http://www.trustwave.com/)
 *
 * You may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * If any of the files related to licensing are missing or if you have any
 * other questions related to licensing please contact Trustwave Holdings, Inc.
 * directly using the email address security@modsecurity.org.
 *
 */

#include "modsecurity/rules.h"

#include <ctime>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "modsecurity/modsecurity.h"
#include "modsecurity/assay.h"
#include "src/utils.h"
#include "parser/driver.h"
#include "utils/https_client.h"

using ModSecurity::Parser::Driver;
using ModSecurity::Utils::HttpsClient;

namespace ModSecurity {


/**
 * @name    incrementReferenceCount
 * @brief   Increment the number of assays using this class
 * @ingroup ModSecCore
 *
 * At certain point it is expected to have two differnt
 * groups of rules attached to a specific core, this happens
 * for instance when a webserver is reloading its
 * configurations, the old configurations/rules is available
 * for the old connections and the new rules are available 
 * for the newest connections.
 *
 * @return Number of the current assays using this rules
 *
 */
void Rules::incrementReferenceCount(void) {
    this->m_referenceCount++;
}

/**
 * @name    decrementReferenceCount
 * @brief   Decrement the number of assays using this class
 * @ingroup ModSecCore
 *
 * @return Number of the current assays using this rules
 *
 */
void Rules::decrementReferenceCount(void) {
    this->m_referenceCount--;
    if (this->m_referenceCount == 0) {
        /**
         * FIXME: If there are new rules loaded (new instance
         *        of this class) and the reference is zero, we
         *        can delete this instance freeing some memory.
         *
         */
        /*
         * std::cout << "We may want to delete this rules instance" \
         *   << std::endl;
         */
    }
}


Rules::~Rules() {
    /** Cleanup the rules */
    for (int i = 0; i < ModSecurity::Phases::NUMBER_OF_PHASES; i++) {
        std::vector<Rule *> rules = this->rules[i];
        while (rules.empty() == false) {
            Rule *rule = rules.back();
            rule->refCountDecreaseAndCheck();
            rules.pop_back();
        }
    }
    /** Cleanup audit log */
    if (audit_log) {
        audit_log->refCountDecreaseAndCheck();
    }
}


/**
 * @name    loadFromUri
 * @brief   load rules from a give uri
 * @ingroup ModSecCore
 *
 * Load the rules from a given uri into memory into memory in
 * the format expected by ModSecurity core.
 *
 * @param uri Full path to the rules file.
 *
 * @return Number of rules loaded, -1 if failed.
 * @retval true  Rules where loaded successfully.
 * @retval false Problem loading the rules.
 *
 */
int Rules::loadFromUri(const char *uri) {
    Driver *driver = new Driver();

    if (driver->parseFile(uri) == false) {
        parserError << driver->parserError.str();
        return -1;
    }

    int rules = this->merge(driver);
    delete driver;

    return rules;
}

int Rules::load(const char *file, const std::string &ref) {
    Driver *driver = new Driver();

    if (driver->parse(file, ref) == false) {
        parserError << driver->parserError.str();
        return -1;
    }
    int rules = this->merge(driver);
    delete driver;

    return rules;
}


int Rules::loadRemote(const char *key, const char *uri) {
    HttpsClient client;
    client.setKey(key);
    bool ret = client.download(uri);

    if (ret) {
        return this->load(client.content.c_str(), uri);
    }

    return -1;
}


int Rules::load(const char *plainRules) {
    return this->load(plainRules, "");
}


std::string Rules::getParserError() {
    return this->parserError.str();
}


int Rules::evaluate(int phase, Assay *assay) {
    if (phase > ModSecurity::Phases::NUMBER_OF_PHASES) {
       return 0;
    }

    std::vector<Rule *> rules = this->rules[phase];

    debug(9, "This phase consists of " + std::to_string(rules.size()) + \
        " rule(s).");

    for (int i = 0; i < rules.size(); i++) {
        Rule *rule = rules[i];
        if (assay->m_marker.empty()) {
            rule->evaluate(assay);
        } else {
            debug(9, "Skipped rule id '" + std::to_string(rule->rule_id) + "' due to a SecMarker: " + assay->m_marker);
            m_secmarker_skipped++;
            debug(9, "Rule: " + rule->m_marker);
            if (rule->m_secmarker && rule->m_marker == assay->m_marker) {
                debug(4, "Out of a SecMarker after skip " \
                    + std::to_string(m_secmarker_skipped) + " rules.");
                assay->m_marker.clear();
                m_secmarker_skipped = 0;
            }
        }
    }
    return 1;
}


int Rules::merge(Driver *from) {
    int amount_of_rules = 0;
    for (int i = 0; i < ModSecurity::Phases::NUMBER_OF_PHASES; i++) {
        std::vector<Rule *> rules = from->rules[i];
        for (int j = 0; j < rules.size(); j++) {
            amount_of_rules++;
            Rule *rule = rules[j];
            this->rules[i].push_back(rule);
            rule->refCountIncrease();
        }
    }

    this->secRuleEngine = from->secRuleEngine;
    this->secRequestBodyAccess = from->secRequestBodyAccess;
    this->secResponseBodyAccess = from->secResponseBodyAccess;
    if (from->m_debugLog && this->m_debugLog &&
        from->m_debugLog->isLogFileSet()) {
        this->m_debugLog->setDebugLogFile(from->m_debugLog->getDebugLogFile());
    }
    if (from->m_debugLog && this->m_debugLog &&
        from->m_debugLog->isLogLevelSet()) {
        this->m_debugLog->setDebugLogLevel(
            from->m_debugLog->getDebugLogLevel());
    }
    this->components = from->components;
    this->requestBodyLimit = from->requestBodyLimit;
    this->responseBodyLimit = from->responseBodyLimit;
    this->requestBodyLimitAction = from->requestBodyLimitAction;
    this->responseBodyLimitAction = from->responseBodyLimitAction;

    /*
     *
     * default Actions is something per configuration context, there is
     * need to merge anything.
     *
     */
    for (int i = 0; i < ModSecurity::Phases::NUMBER_OF_PHASES; i++) {
        std::vector<Action *> actions = from->defaultActions[i];
        this->defaultActions[i].clear();
        for (int j = 0; j < actions.size(); j++) {
            Action *action = actions[j];
            this->defaultActions[i].push_back(action);
        }
    }

    if (from->audit_log != NULL && this->audit_log != NULL) {
        this->audit_log->refCountDecreaseAndCheck();
    }
    if (from->audit_log) {
        this->audit_log = from->audit_log;
    }
    if (this->audit_log != NULL) {
        this->audit_log->refCountIncrease();
    }

    return amount_of_rules;
}


int Rules::merge(Rules *from) {
    int amount_of_rules = 0;
    for (int i = 0; i < ModSecurity::Phases::NUMBER_OF_PHASES; i++) {
        std::vector<Rule *> rules = from->rules[i];
        for (int j = 0; j < rules.size(); j++) {
            amount_of_rules++;
            Rule *rule = rules[j];
            this->rules[i].push_back(rule);
            rule->refCountIncrease();
        }
    }

    this->secRuleEngine = from->secRuleEngine;
    this->secRequestBodyAccess = from->secRequestBodyAccess;
    this->secResponseBodyAccess = from->secResponseBodyAccess;
    this->components = from->components;
    this->requestBodyLimit = from->requestBodyLimit;
    this->responseBodyLimit = from->responseBodyLimit;
    this->requestBodyLimitAction = from->requestBodyLimitAction;
    this->responseBodyLimitAction = from->responseBodyLimitAction;

    if (from->m_debugLog && this->m_debugLog &&
        from->m_debugLog->isLogFileSet()) {
        this->m_debugLog->setDebugLogFile(from->m_debugLog->getDebugLogFile());
    }
    if (from->m_debugLog && this->m_debugLog &&
        from->m_debugLog->isLogLevelSet()) {
        this->m_debugLog->setDebugLogLevel(
            from->m_debugLog->getDebugLogLevel());
    }

    if (from->audit_log != NULL && this->audit_log != NULL) {
        this->audit_log->refCountDecreaseAndCheck();
    }
    if (from->audit_log) {
        this->audit_log = from->audit_log;
    }
    if (this->audit_log != NULL) {
        this->audit_log->refCountIncrease();
    }

    return amount_of_rules;
}


void Rules::debug(int level, std::string message) {
    if (m_debugLog != NULL) {
        m_debugLog->write(level, message);
    }
}


void Rules::dump() {
    std::cout << "Rules: " << std::endl;
    for (int i = 0; i < ModSecurity::Phases::NUMBER_OF_PHASES; i++) {
        std::vector<Rule *> rules = this->rules[i];
        std::cout << "Phase: " << std::to_string(i);
        std::cout << " (" << std::to_string(rules.size());
        std::cout << " rules)" << std::endl;
        for (int j = 0; j < rules.size(); j++) {
            std::cout << "    Rule ID: " << std::to_string(rules[j]->rule_id);
            std::cout << "--" << rules[j] << std::endl;
        }
    }
}


extern "C" Rules *msc_create_rules_set() {
    Rules *rules = new Rules();

    return rules;
}


extern "C" void msc_rules_dump(Rules *rules) {
    rules->dump();
}


extern "C" int msc_rules_merge(Rules *rules_dst,
    Rules *rules_from) {
    rules_dst->merge(rules_from);

    return 0;
}


extern "C" int msc_rules_add_remote(Rules *rules,
    const char *key, const char *uri, const char **error) {
    int ret = rules->loadRemote(key, uri);
    if (ret < 0) {
        *error = strdup(rules->getParserError().c_str());
    }
    return ret;
}


extern "C" int msc_rules_add_file(Rules *rules, const char *file,
    const char **error) {
    int ret = rules->loadFromUri(file);
    if (ret < 0) {
        *error = strdup(rules->getParserError().c_str());
    }
    return ret;
}


extern "C" int msc_rules_add(Rules *rules, const char *plain_rules,
    const char **error) {
    int ret = rules->load(plain_rules);
    if (ret < 0) {
        *error = strdup(rules->getParserError().c_str());
    }
    return ret;
}


extern "C" int msc_rules_cleanup(Rules *rules) {
    delete rules;
    return true;
}


}  // namespace ModSecurity

