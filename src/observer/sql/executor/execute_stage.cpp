/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its
affiliates. All rights reserved. miniob is licensed under Mulan PSL v2. You can
use this software according to the terms and conditions of the Mulan PSL v2. You
may obtain a copy of Mulan PSL v2 at: http://license.coscl.org.cn/MulanPSL2 THIS
SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/4/13.
//

#include "execute_stage.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/seda/timer_stage.h"
#include "event/execution_plan_event.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "event/storage_event.h"
#include "session/session.h"
#include "sql/executor/execution_node.h"
#include "sql/executor/select_executor.h"
#include "sql/executor/tuple.h"
#include "storage/common/condition_filter.h"
#include "storage/common/table.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"

using namespace common;

//! Constructor
ExecuteStage::ExecuteStage(const char *tag) : Stage(tag) {}

//! Destructor
ExecuteStage::~ExecuteStage() {}

//! Parse properties, instantiate a stage object
Stage *ExecuteStage::make_stage(const std::string &tag) {
    ExecuteStage *stage = new (std::nothrow) ExecuteStage(tag.c_str());
    if (stage == nullptr) {
        LOG_ERROR("new ExecuteStage failed");
        return nullptr;
    }
    stage->set_properties();
    return stage;
}

//! Set properties for this object set in stage specific properties
bool ExecuteStage::set_properties() {
    //  std::string stageNameStr(stageName);
    //  std::map<std::string, std::string> section = theGlobalProperties()->get(
    //    stageNameStr);
    //
    //  std::map<std::string, std::string>::iterator it;
    //
    //  std::string key;

    return true;
}

//! Initialize stage params and validate outputs
bool ExecuteStage::initialize() {
    LOG_TRACE("Enter");

    std::list<Stage *>::iterator stgp = next_stage_list_.begin();
    default_storage_stage_ = *(stgp++);
    mem_storage_stage_ = *(stgp++);

    LOG_TRACE("Exit");
    return true;
}

//! Cleanup after disconnection
void ExecuteStage::cleanup() {
    LOG_TRACE("Enter");

    LOG_TRACE("Exit");
}

void ExecuteStage::handle_event(StageEvent *event) {
    LOG_TRACE("Enter\n");

    handle_request(event);

    LOG_TRACE("Exit\n");
    return;
}

void ExecuteStage::callback_event(StageEvent *event, CallbackContext *context) {
    LOG_TRACE("Enter\n");

    // here finish read all data from disk or network, but do nothing here.
    ExecutionPlanEvent *exe_event = static_cast<ExecutionPlanEvent *>(event);
    SQLStageEvent *sql_event = exe_event->sql_event();
    sql_event->done_immediate();

    LOG_TRACE("Exit\n");
    return;
}

void ExecuteStage::handle_request(common::StageEvent *event) {
    ExecutionPlanEvent *exe_event = static_cast<ExecutionPlanEvent *>(event);
    SessionEvent *session_event = exe_event->sql_event()->session_event();
    Query *sql = exe_event->sqls();
    const char *current_db =
        session_event->get_client()->session->get_current_db().c_str();

    CompletionCallback *cb =
        new (std::nothrow) CompletionCallback(this, nullptr);
    if (cb == nullptr) {
        LOG_ERROR("Failed to new callback for ExecutionPlanEvent");
        exe_event->done_immediate();
        return;
    }
    exe_event->push_callback(cb);

    switch (sql->flag) {
        case SCF_SELECT: {  // select
            RC rc = do_select(current_db, sql,
                              exe_event->sql_event()->session_event());
            if (rc != SUCCESS) {
                session_event->set_response("FAILURE\n");
            }
            exe_event->done_immediate();
        } break;

        case SCF_INSERT:
        case SCF_UPDATE:
        case SCF_DELETE:
        case SCF_CREATE_TABLE:
        case SCF_SHOW_TABLES:
        case SCF_DESC_TABLE:
        case SCF_DROP_TABLE:
        case SCF_CREATE_INDEX:
        case SCF_DROP_INDEX:
        case SCF_LOAD_DATA: {
            StorageEvent *storage_event =
                new (std::nothrow) StorageEvent(exe_event);
            if (storage_event == nullptr) {
                LOG_ERROR("Failed to new StorageEvent");
                event->done_immediate();
                return;
            }
            default_storage_stage_->handle_event(storage_event);
        } break;
        case SCF_SYNC: {
            RC rc = DefaultHandler::get_default().sync();
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        } break;
        case SCF_BEGIN: {
            session_event->get_client()->session->set_trx_multi_operation_mode(
                true);
            session_event->set_response(strrc(RC::SUCCESS));
            exe_event->done_immediate();
        } break;
        case SCF_COMMIT: {
            Trx *trx = session_event->get_client()->session->current_trx();
            RC rc = trx->commit();
            session_event->get_client()->session->set_trx_multi_operation_mode(
                false);
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        } break;
        case SCF_ROLLBACK: {
            Trx *trx = session_event->get_client()->session->current_trx();
            RC rc = trx->rollback();
            session_event->get_client()->session->set_trx_multi_operation_mode(
                false);
            session_event->set_response(strrc(rc));
            exe_event->done_immediate();
        } break;
        case SCF_HELP: {
            const char *response =
                "show tables;\n"
                "desc `table name`;\n"
                "create table `table name` (`column name` `column type`, "
                "...);\n"
                "create index `index name` on `table` (`column`);\n"
                "insert into `table` values(`value1`,`value2`);\n"
                "update `table` set column=value [where `column`=`value`];\n"
                "delete from `table` [where `column`=`value`];\n"
                "select [ * | `columns` ] from `table`;\n";
            session_event->set_response(response);
            exe_event->done_immediate();
        } break;
        case SCF_EXIT: {
            // do nothing
            const char *response = "Unsupported\n";
            session_event->set_response(response);
            exe_event->done_immediate();
        } break;
        default: {
            exe_event->done_immediate();
            LOG_ERROR("Unsupported command=%d\n", sql->flag);
        }
    }
}

// 这里没有对输入的某些信息做合法性校验，比如查询的列名、where条件中的列名等，没有做必要的合法性校验
// 需要补充上这一部分.
// 校验部分也可以放在resolve，不过跟execution放一起也没有关系
RC ExecuteStage::do_select(const char *db, Query *sql,
                           SessionEvent *session_event) {
    Session *session = session_event->get_client()->session;
    Trx *trx = session->current_trx();
    const Selects &selects = sql->sstr.selection;
    // 把所有的表和只跟这张表关联的condition都拿出来，生成最底层的select
    // 执行节点

    SelectExecutor select_executor(session, trx, db, &selects);
    RC rc = select_executor.execute(session_event);
    if (rc != RC::SUCCESS) {
        return rc;
    }
    return RC::SUCCESS;
}