/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * cm_ha_add_nodes.h - tasks that run the HA node-adding script ($CUBRID/share/cms/ha_add_nodes.sh)
 */

#ifndef _CM_HA_ADD_NODES_H_
#define _CM_HA_ADD_NODES_H_

#include "json/json.h"

#if !defined(WINDOWS)
int ext_ha_check_ssh (Json::Value &request, Json::Value &response);
int ext_ha_add_nodes (Json::Value &request, Json::Value &response);
int ext_ha_add_nodes_status (Json::Value &request, Json::Value &response);
int ext_ha_add_nodes_cancel (Json::Value &request, Json::Value &response);
#endif

#endif /* _CM_HA_ADD_NODES_H_ */
