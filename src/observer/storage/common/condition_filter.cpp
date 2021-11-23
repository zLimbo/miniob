/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its
affiliates. All rights reserved. miniob is licensed under Mulan PSL v2. You can
use this software according to the terms and conditions of the Mulan PSL v2. You
may obtain a copy of Mulan PSL v2 at: http://license.coscl.org.cn/MulanPSL2 THIS
SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2021/5/7.
//

#include "condition_filter.h"

#include <stddef.h>

#include "common/log/log.h"
#include "record_manager.h"
#include "session/session.h"
#include "sql/executor/execution_node.h"
#include "sql/executor/tuple.h"
#include "sql/parser/parse.h"
#include "storage/common/field_meta.h"
#include "storage/common/table.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"
using namespace common;

static bool judge_cmp_result(CompOp comp_op, int cmp_result) {
    switch (comp_op) {
        case EQUAL_TO:
            return 0 == cmp_result;
        case LESS_EQUAL:
            return cmp_result <= 0;
        case NOT_EQUAL:
            return cmp_result != 0;
        case LESS_THAN:
            return cmp_result < 0;
        case GREAT_EQUAL:
            return cmp_result >= 0;
        case GREAT_THAN:
            return cmp_result > 0;
    }
    LOG_PANIC("Never should print this.");
    return cmp_result;  // should not go here
}

ConditionFilter::~ConditionFilter() {}

DefaultConditionFilter::DefaultConditionFilter() {
    left_.is_attr = false;
    left_.data.field_meta = nullptr;
    left_.data.value = nullptr;

    right_.is_attr = false;
    right_.data.field_meta = nullptr;
    right_.data.value = nullptr;
}

DefaultConditionFilter::~DefaultConditionFilter() {}

RC DefaultConditionFilter::init(const ConDesc& left, const ConDesc& right,
                                AttrType left_type, AttrType right_type,
                                CompOp comp_op) {
    if (left_type <= AttrType::UNDEFINED || left_type > AttrType::DATES) {
        LOG_ERROR("Invalid condition with unsupported attribute type: %d",
                  left_type);
        return RC::INVALID_ARGUMENT;
    }
    if (right_type <= AttrType::UNDEFINED || right_type > AttrType::DATES) {
        LOG_ERROR("Invalid condition with unsupported attribute type: %d",
                  right_type);
        return RC::INVALID_ARGUMENT;
    }

    if (comp_op < CompOp::EQUAL_TO || comp_op >= CompOp::NO_OP) {
        LOG_ERROR("Invalid condition with unsupported compare operation: %d",
                  comp_op);
        return RC::INVALID_ARGUMENT;
    }

    left_ = left;
    right_ = right;
    left_type_ = left_type;
    right_type_ = right_type;
    comp_op_ = comp_op;
    return RC::SUCCESS;
}

RC DefaultConditionFilter::init(Table& table, const Condition& condition) {
    const TableMeta& table_meta = table.table_meta();
    ConDesc left;
    ConDesc right;

    AttrType type_left = UNDEFINED;
    AttrType type_right = UNDEFINED;

    if (1 == condition.left_is_attr) {
        left.is_attr = true;
        const FieldMeta* field_meta =
            table_meta.field(condition.left_attr.attribute_name);
        if (nullptr == field_meta) {
            LOG_WARN("No such field in condition. %s.%s", table.name(),
                     condition.left_attr.attribute_name);
            return RC::SCHEMA_FIELD_MISSING;
        }
        left.data.field_meta = field_meta;
        type_left = field_meta->type();
    } else {
        left.is_attr = false;
        left.data.value = &condition.left_value;
        type_left = condition.left_value.type;
    }

    if (1 == condition.right_is_attr) {
        right.is_attr = true;
        const FieldMeta* field_meta =
            table_meta.field(condition.right_attr.attribute_name);
        if (nullptr == field_meta) {
            LOG_WARN("No such field in condition. %s.%s", table.name(),
                     condition.right_attr.attribute_name);
            return RC::SCHEMA_FIELD_MISSING;
        }
        right.data.field_meta = field_meta;
        type_right = field_meta->type();
    } else {
        right.is_attr = false;
        right.data.value = &condition.right_value;
        type_right = condition.right_value.type;
    }

    // 校验和转换
    //  if (!field_type_compare_compatible_table[type_left][type_right]) {
    //    // 不能比较的两个字段， 要把信息传给客户端
    //    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    //  }
    // NOTE：这里没有实现不同类型的数据比较，比如整数跟浮点数之间的对比
    // 但是选手们还是要实现。这个功能在预选赛中会出现

    if (type_left == type_right || type_left == NULLS || type_right == NULLS ||
        (AttrType::INTS == type_left || AttrType::FLOATS == type_left) &&
            (AttrType::INTS == type_right || AttrType::FLOATS == type_right)) {
        return init(left, right, type_left, type_right, condition.comp);
    }

    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
}

static bool value_compare(const Value& lhs, const Value& rhs, CompOp comp_op) {
    if (CompOp::IS_NULL == comp_op && AttrType::NULLS == lhs.type &&
            AttrType::NULLS == rhs.type ||
        CompOp::NOT_NULL == comp_op &&
            (AttrType::NULLS == lhs.type) ^ (AttrType::NULLS == rhs.type)) {
        return true;
    }

    if ((AttrType::CHARS == lhs.type || AttrType::DATES == lhs.type) &&
        (AttrType::CHARS == rhs.type || AttrType::DATES == rhs.type)) {
        int cmp_result = strcmp((const char*)lhs.data, (const char*)rhs.data);
        return judge_cmp_result(comp_op, cmp_result);
    }

    if ((AttrType::INTS == lhs.type || AttrType::FLOATS == lhs.type) &&
        (AttrType::INTS == rhs.type || AttrType::FLOATS == rhs.type)) {
        float left_value =
            AttrType::INTS == lhs.type ? *(int*)lhs.data : *(float*)lhs.data;
        float right_value =
            AttrType::INTS == rhs.type ? *(int*)rhs.data : *(float*)rhs.data;
        int cmp_result =
            FloatValue(left_value).compare(FloatValue(right_value));
        return judge_cmp_result(comp_op, cmp_result);
    }

    return false;
}

bool DefaultConditionFilter::filter(const Record& rec) const {
    Value left_value, right_value;

    if (left_.is_attr) {
        if (left_.data.field_meta->nullable()) {
            bool is_null = *(bool*)(rec.data + left_.data.field_meta->offset());
            if (is_null) {
                left_value.type = AttrType::NULLS;
                left_value.data = nullptr;
            } else {
                left_value.type = left_type_;
                left_value.data = rec.data + left_.data.field_meta->offset() + 1;
            }
        } else {
            left_value.type = left_type_;
            left_value.data = rec.data + left_.data.field_meta->offset();
        }
    } else {
        left_value = *left_.data.value;
    }

    if (right_.is_attr) {
        if (right_.data.field_meta->nullable()) {
            bool is_null =
                *(bool*)(rec.data + right_.data.field_meta->offset());
            if (is_null) {
                right_value.type = AttrType::NULLS;
                right_value.data = nullptr;
            } else {
                right_value.type = right_type_;
                right_value.data = rec.data + right_.data.field_meta->offset() + 1;
            }
        } else {
            right_value.type = right_type_;
            right_value.data = rec.data + right_.data.field_meta->offset();
        }
    } else {
        right_value = *right_.data.value;
    }

    return value_compare(left_value, right_value, comp_op_);
}

CompositeConditionFilter::~CompositeConditionFilter() {
    if (memory_owner_) {
        delete[] filters_;
        filters_ = nullptr;
    }
}

RC CompositeConditionFilter::init(const ConditionFilter* filters[],
                                  int filter_num, bool own_memory) {
    filters_ = filters;
    filter_num_ = filter_num;
    memory_owner_ = own_memory;
    return RC::SUCCESS;
}
RC CompositeConditionFilter::init(const ConditionFilter* filters[],
                                  int filter_num) {
    return init(filters, filter_num, false);
}

RC CompositeConditionFilter::init(Table& table, const Condition* conditions,
                                  int condition_num) {
    if (condition_num == 0) {
        return RC::SUCCESS;
    }
    if (conditions == nullptr) {
        return RC::INVALID_ARGUMENT;
    }

    RC rc = RC::SUCCESS;
    ConditionFilter** condition_filters = new ConditionFilter*[condition_num];
    for (int i = 0; i < condition_num; i++) {
        DefaultConditionFilter* default_condition_filter =
            new DefaultConditionFilter();
        rc = default_condition_filter->init(table, conditions[i]);
        if (rc != RC::SUCCESS) {
            delete default_condition_filter;
            for (int j = i - 1; j >= 0; j--) {
                delete condition_filters[j];
                condition_filters[j] = nullptr;
            }
            delete[] condition_filters;
            condition_filters = nullptr;
            return rc;
        }
        condition_filters[i] = default_condition_filter;
    }
    return init((const ConditionFilter**)condition_filters, condition_num,
                true);
}

bool CompositeConditionFilter::filter(const Record& rec) const {
    for (int i = 0; i < filter_num_; i++) {
        if (!filters_[i]->filter(rec)) {
            return false;
        }
    }
    return true;
}
