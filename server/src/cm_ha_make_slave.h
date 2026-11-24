/*
 * HA slave POC: make this server an HA slave of a running master.
 * Tasks are described in docs/ha-make-slave-poc-api.md. Linux only.
 */

#ifndef _CM_HA_MAKE_SLAVE_H_
#define _CM_HA_MAKE_SLAVE_H_

#include <json/json.h>

int ext_ha_check_ssh (Json::Value &request, Json::Value &response);
int ext_ha_make_slave (Json::Value &request, Json::Value &response);
int ext_ha_make_slave_status (Json::Value &request, Json::Value &response);
int ext_ha_make_slave_cancel (Json::Value &request, Json::Value &response);
int ext_ha_make_slave_cleanup (Json::Value &request, Json::Value &response);

#endif /* _CM_HA_MAKE_SLAVE_H_ */
