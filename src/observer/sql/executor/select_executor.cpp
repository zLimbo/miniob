// ! zlimbo

#include "select_executor.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

#include "common/log/log.h"
#include "event/session_event.h"
#include "session/session.h"
#include "sql/executor/execution_node.h"
#include "storage/common/table.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"

static inline bool judge_cmp_result(CompOp comp_op, int cmp_result) {
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

RC TupleFilter::init(const TupleSchema &left_schema,
                     const TupleSchema &right_schema, const Selects &selects,
                     const char *db) {
    const std::vector<TupleField> &left_tuple_fields = left_schema.fields();
    const std::vector<TupleField> &right_tuple_fields = right_schema.fields();
    for (size_t i = 0; i < selects.condition_num; ++i) {
        const Condition &condition = selects.conditions[i];
        if (condition.left_is_attr == 1 && condition.right_is_attr == 1) {
            std::string right_table_name(condition.right_attr.relation_name);
            std::string right_field_name(condition.right_attr.attribute_name);
            int right_pos = -1;
            AttrType right_type = AttrType::UNDEFINED;
            for (int i = 0; i < right_tuple_fields.size(); ++i) {
                const TupleField &tuple_field = right_tuple_fields[i];
                if (tuple_field.table_name() == right_table_name &&
                    tuple_field.field_name() == right_field_name) {
                    right_pos = i;
                    right_type = tuple_field.type();
                    break;
                }
            }
            if (-1 == right_pos) continue;

            std::string left_table_name(condition.left_attr.relation_name);
            std::string left_field_name(condition.left_attr.attribute_name);
            int left_pos = -1;
            AttrType left_type = AttrType::UNDEFINED;
            for (int i = 0; i < left_tuple_fields.size(); ++i) {
                const TupleField &tuple_field = left_tuple_fields[i];
                if (tuple_field.table_name() == left_table_name &&
                    tuple_field.field_name() == left_field_name) {
                    left_pos = i;
                    left_type = tuple_field.type();
                    break;
                }
            }
            if (-1 == left_pos) continue;

            // todo: 暂时进行相同类型的比较，后续调整
            if (left_type != right_type) {
                return RC::SCHEMA_FIELD_TYPE_MISMATCH;
            }

            TupleCons tuple_cons;
            tuple_cons.left_value_pos = left_pos;
            tuple_cons.right_value_pos = right_pos;
            tuple_cons.type = left_type;
            tuple_cons.condition = &condition;
            tuple_cons_vector_.push_back(tuple_cons);
        }
    }
    return RC::SUCCESS;
}

bool TupleFilter::filter(const Tuple &left_tuple, const Tuple &right_tuple) {
    for (TupleCons &tuple_cons : tuple_cons_vector_) {
        const TupleValue &left_value =
            left_tuple.get(tuple_cons.left_value_pos);
        const TupleValue &right_value =
            right_tuple.get(tuple_cons.right_value_pos);
        if (left_value.get_type() != right_value.get_type() ||
            AttrType::NULLS == left_value.get_type() ||
            AttrType::NULLS == right_value.get_type()) {
            // 程序正确的话，这里异样的类型只能是 null，null 无法比较
            return false;
        }
        int cmp_result = left_value.compare(right_value);
        if (!judge_cmp_result(tuple_cons.condition->comp, cmp_result)) {
            return false;
        }
    }
    return true;
}

RC TupleProjcet::init(TupleSchema &tuple_schema,
                      TupleSchema &select_tuple_schema) {
    for (const TupleField &select_tuple_field : select_tuple_schema.fields()) {
        int pos = tuple_schema.index_of_field(select_tuple_field.table_name(),
                                              select_tuple_field.field_name());
        if (-1 == pos) {
            return RC::SCHEMA_FIELD_MISSING;
        }
        select_tuple_pos_vector_.push_back(pos);
    }

    return RC::SUCCESS;
}

RC TupleProjcet::project(Tuple &tuple, Tuple &new_tuple) {
    LOG_DEBUG("ZD: select_tuple_pos_vector_.size=%d",
              select_tuple_pos_vector_.size());
    for (int pos : select_tuple_pos_vector_) {
        new_tuple.add(tuple.get_pointer(pos));
    }
    return RC::SUCCESS;
}

bool SelectExecutor::match_table(const char *table_name_in_condition,
                                 const char *table_name_to_match) {
    if (table_name_in_condition != nullptr) {
        return 0 == strcmp(table_name_in_condition, table_name_to_match);
    }

    return selects_->relation_num == 1;
}

void SelectExecutor::end_trx_if_need(bool all_right) {
    if (!session_->is_trx_multi_operation_mode()) {
        if (all_right) {
            trx_->commit();
        } else {
            trx_->rollback();
        }
    }
}

RC SelectExecutor::add_single_table_tuple_schema(const char *table_name,
                                                 TupleSchema &tuple_schema) {
    Table *table = DefaultHandler::get_default().find_table(db_, table_name);
    if (nullptr == table) {
        LOG_WARN("No such table [%s] in db [%s]", table_name, db_);
        return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    TupleSchema::from_table(table, tuple_schema);
    return RC::SUCCESS;
}

RC SelectExecutor::add_all_table_tuple_schema(TupleSchema &tuple_schema) {
    for (int i = selects_->relation_num - 1; i >= 0; --i) {
        RC rc =
            add_single_table_tuple_schema(selects_->relations[i], tuple_schema);
        if (RC::SUCCESS != rc) {
            return rc;
        }
    }
    return RC::SUCCESS;
}

RC SelectExecutor::schema_add_field(const char *table_name,
                                    const char *field_name,
                                    TupleSchema &schema) {
    Table *table = DefaultHandler::get_default().find_table(db_, table_name);
    const FieldMeta *field_meta = table->table_meta().field(field_name);
    if (nullptr == field_meta) {
        LOG_WARN("No such field. %s.%s", table->name(), field_name);
        return RC::SCHEMA_FIELD_MISSING;
    }

    schema.add_if_not_exists(field_meta->type(), table->name(),
                             field_meta->name());
    return RC::SUCCESS;
}

RC SelectExecutor::get_select_tuple_schema(TupleSchema &tuple_schema,
                                           TupleSchema &select_tuple_schema) {
    for (int i = selects_->attr_num - 1; i >= 0; --i) {
        const RelAttr &attr = selects_->attributes[i];
        if (0 == strcmp("*", attr.attribute_name)) {
            if (nullptr == attr.relation_name) {
                if (i != selects_->attr_num - 1) {
                    return RC::SQL_SYNTAX;
                }
                select_tuple_schema = tuple_schema;
                return RC::SUCCESS;
            }
            RC rc = add_single_table_tuple_schema(attr.relation_name,
                                                  select_tuple_schema);
            if (RC::SUCCESS != rc) {
                return rc;
            }
        } else {
            const char *table_name = attr.relation_name;
            if (nullptr == table_name) {
                table_name =
                    get_unique_table_name(tuple_schema, attr.attribute_name);
                if (nullptr == table_name) {
                    return RC::SQL_SYNTAX;
                }
            }
            RC rc = schema_add_field(table_name, attr.attribute_name,
                                     select_tuple_schema);
            if (RC::SUCCESS != rc) {
                return rc;
            }
        }
    }
    return RC::SUCCESS;
}

SelectExecutor::SelectExecutor(Session *session, Trx *trx, const char *db,
                               const Selects *selects) {
    this->session_ = session;
    this->trx_ = trx;
    this->db_ = db;
    this->selects_ = selects;
}

// 把所有的表和只跟这张表关联的condition都拿出来，生成最底层的select
// 执行节点
RC SelectExecutor::create_select_exe_node(const char *table_name,
                                          SelectExeNode &select_node) {
    // 列出跟这张表关联的Attr
    TupleSchema schema;
    Table *table = DefaultHandler::get_default().find_table(db_, table_name);
    if (nullptr == table) {
        LOG_WARN("No such table [%s] in db [%s]", table_name, db_);
        return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    // ! zl 所有字段全部查到内存中再进行join和projection
    TupleSchema::from_table(table, schema);

    // 找出仅与此表相关的过滤条件, 或者都是值的过滤条件
    std::vector<DefaultConditionFilter *> condition_filters;
    for (size_t i = 0; i < selects_->condition_num; i++) {
        const Condition &condition = selects_->conditions[i];
        if ((condition.left_is_attr == 0 && condition.right_is_attr == 0) ||
            (condition.left_is_attr == 1 && condition.right_is_attr == 0 &&
             match_table(condition.left_attr.relation_name,
                         table_name)) ||  // 左边是属性右边是值
            (condition.left_is_attr == 0 && condition.right_is_attr == 1 &&
             match_table(condition.right_attr.relation_name,
                         table_name)) ||  // 左边是值，右边是属性名
            (condition.left_is_attr == 1 && condition.right_is_attr == 1 &&
             match_table(condition.left_attr.relation_name, table_name) &&
             match_table(condition.right_attr.relation_name,
                         table_name))  // 左右都是属性名，并且表名都符合
        ) {
            DefaultConditionFilter *condition_filter =
                new DefaultConditionFilter();
            RC rc = condition_filter->init(*table, condition);
            if (rc != RC::SUCCESS) {
                delete condition_filter;
                for (DefaultConditionFilter *&filter : condition_filters) {
                    delete filter;
                }
                return rc;
            }
            condition_filters.push_back(condition_filter);
        }
    }

    return select_node.init(trx_, table, std::move(schema),
                            std::move(condition_filters));
}

RC SelectExecutor::select_all_table_tuple_sets(
    std::vector<TupleSet> &tuple_sets) {
    RC rc = RC::SUCCESS;
    std::vector<SelectExeNode *> select_nodes;
    // ! zl: 因为语法解析结果是逆向的，所以这里修改为从后往前遍历
    for (int i = selects_->relation_num - 1; i >= 0; --i) {
        const char *table_name = selects_->relations[i];
        SelectExeNode *select_node = new SelectExeNode;
        rc = create_select_exe_node(table_name, *select_node);
        if (rc != RC::SUCCESS) {
            delete select_node;
            for (SelectExeNode *&tmp_node : select_nodes) {
                delete tmp_node;
            }
            return rc;
        }
        select_nodes.push_back(select_node);
    }

    if (select_nodes.empty()) {
        LOG_ERROR("No table given");
        return RC::SQL_SYNTAX;
    }

    for (SelectExeNode *&node : select_nodes) {
        TupleSet tuple_set;
        rc = node->execute(tuple_set);
        if (rc != RC::SUCCESS) {
            for (SelectExeNode *&tmp_node : select_nodes) {
                delete tmp_node;
            }
            return rc;
        } else {
            tuple_sets.push_back(std::move(tuple_set));
        }
    }

    for (SelectExeNode *&tmp_node : select_nodes) {
        delete tmp_node;
    }

    return rc;
}

RC SelectExecutor::select_filter_tuples(
    TupleSchema &tuple_schema, std::vector<Tuple> &filter_tuple_vector) {
    RC rc = RC::SUCCESS;

    std::vector<TupleSet> tuple_sets;
    rc = select_all_table_tuple_sets(tuple_sets);
    if (rc != RC::SUCCESS) {
        return rc;
    }

    for (TupleSet &tuple_set : tuple_sets) {
        tuple_schema.append(tuple_set.schema());
    }

    // 所有表全部字段的笛卡尔积，在过程中进行过滤
    std::vector<Tuple> tuple_vector(1);
    TupleSchema left_schema;
    for (TupleSet &tuple_set : tuple_sets) {
        std::vector<Tuple> new_tuple_vector;
        const TupleSchema &right_schema = tuple_set.get_schema();

        TupleFilter tuple_filter;
        rc = tuple_filter.init(left_schema, right_schema, *selects_, db_);
        if (rc != RC::SUCCESS) {
            return rc;
        }

        for (const Tuple &left_tuple : tuple_vector) {
            for (const Tuple &right_tuple : tuple_set.tuples()) {
                if (!tuple_filter.filter(left_tuple, right_tuple)) {
                    continue;
                }
                Tuple new_tuple(left_tuple);
                new_tuple.add(right_tuple.values());
                new_tuple_vector.emplace_back(std::move(new_tuple));
            }
        }
        left_schema.append(right_schema);
        tuple_vector = std::move(new_tuple_vector);
    }

    filter_tuple_vector = std::move(tuple_vector);

    return rc;
}

RC SelectExecutor::order_tuples(TupleSchema &tuple_schema,
                                std::vector<Tuple> &filter_tuple_vector) {
    LOG_DEBUG("ZD: order_tuples");
    std::vector<std::pair<int, int>> order_tuple_pos_vector;
    for (size_t i = 0; i < selects_->order_num; ++i) {
        const char *table_name = selects_->orders[i].attr.relation_name;
        const char *field_name = selects_->orders[i].attr.attribute_name;
        if (nullptr == field_name) {
            return RC::SQL_SYNTAX;
        }
        if (nullptr == table_name) {
            table_name = get_unique_table_name(tuple_schema, field_name);
            if (nullptr == table_name) {
                return RC::SQL_SYNTAX;
            }
        }
        // 检测列是否存在
        int pos = tuple_schema.index_of_field(table_name, field_name);
        if (-1 == pos) {  // 错误：不存在的列
            return RC::SQL_SYNTAX;
        }
        order_tuple_pos_vector.emplace_back(pos, selects_->orders[i].is_desc);
    }
    std::stable_sort(filter_tuple_vector.begin(), filter_tuple_vector.end(),
                     [&](const Tuple &lhs, const Tuple &rhs) {
                         for (auto [pos, is_desc] : order_tuple_pos_vector) {
                             const TupleValue &lv = lhs.get(pos);
                             const TupleValue &rv = rhs.get(pos);
                             int result = lv.compare(rv);
                             if (result != 0) {
                                 return bool((result < 0) ^ is_desc);
                             }
                         }
                         return false;
                     });
    return RC::SUCCESS;
}

RC SelectExecutor::project(TupleSchema &tuple_schema,
                           std::vector<Tuple> &filter_tuple_vector,
                           TupleSet &result_set) {
    LOG_DEBUG("project");
    RC rc = RC::SUCCESS;
    // 投影
    TupleSchema select_tuple_schema;
    rc = get_select_tuple_schema(tuple_schema, select_tuple_schema);
    if (rc != RC::SUCCESS) {
        return rc;
    }
    TupleProjcet tuple_project;
    rc = tuple_project.init(tuple_schema, select_tuple_schema);
    if (rc != RC::SUCCESS) {
        return rc;
    }
    result_set.set_schema(select_tuple_schema);
    for (Tuple &tuple : filter_tuple_vector) {
        Tuple join_tuple;
        tuple_project.project(tuple, join_tuple);
        result_set.add(std::move(join_tuple));
    }

    return rc;
}

static inline bool field_name_is_numeric(const char *field_name) {
    for (const char *p = field_name; *p != '\0'; ++p) {
        if (!isdigit(*p) || *p != '.') {
            return false;
        }
    }
    return true;
}

const char *SelectExecutor::get_unique_table_name(
    const TupleSchema &tuple_schema, const char *field_name) {
    int cnt = 0;
    const char *table_name = nullptr;
    for (int i = 0; i < selects_->relation_num; ++i) {
        if (-1 !=
            tuple_schema.index_of_field(selects_->relations[i], field_name)) {
            if (++cnt == 2) {
                // 相同字段歧义冲突（虽然原有普通的select没有判定这类语法错误）
                return nullptr;
            }
            table_name = selects_->relations[i];
        }
    }
    return table_name;
}

RC SelectExecutor::aggregate(TupleSchema &tuple_schema,
                             std::vector<Tuple> &filter_tuple_vector,
                             TupleSet &result_set) {
    RC rc = RC::SUCCESS;
    LOG_DEBUG("aggregate");
    TupleSchema aggregate_schema;
    Tuple aggregate_tuple;
    for (int i = 0; i < selects_->aggregate_num; ++i) {
        const char *aggregate_name = selects_->aggregates[i];
        const char *field_name = selects_->attributes[i].attribute_name;
        const char *table_name = selects_->attributes[i].relation_name;
        if (nullptr == aggregate_name || nullptr == field_name) {
            return RC::SQL_SYNTAX;
        }

        bool is_numeric = field_name_is_numeric(field_name);
        if (is_numeric || 0 == strcmp("*", field_name)) {
            // 数值和*不能拥有表名, sql解析成功便已排除此情况
            // 因为sql解析成功，如果有数字一定是NUMBER或FLOAT类型
            aggregate_schema.add(AttrType::FLOATS, "", field_name,
                                 aggregate_name);
            // 对于 avg, min, max来说，result就是field_name本身
            float result = 0.0;
            if (0 == strcmp(aggregate_name, "count")) {
                result = filter_tuple_vector.size();
            } else if (is_numeric && (0 == strcmp(aggregate_name, "max") ||
                                      0 == strcmp(aggregate_name, "min") ||
                                      0 == strcmp(aggregate_name, "avg"))) {
                result = atof(field_name);
            } else {
                return RC::SQL_SYNTAX;
            }
            aggregate_tuple.add(result);
            continue;
        }

        if (nullptr == table_name) {
            // 在from的tables中查找对应的表名,需唯一
            table_name = get_unique_table_name(tuple_schema, field_name);
            if (table_name == nullptr) {
                return RC::SQL_SYNTAX;
            }
        }
        // 检测列是否存在
        int pos = tuple_schema.index_of_field(table_name, field_name);
        if (-1 == pos) {  // 错误：不存在的列
            return RC::SQL_SYNTAX;
        }

        AttrType type = tuple_schema.field(pos).type();
        switch (type) {
            case AttrType::INTS:
            case AttrType::FLOATS: {
                rc = aggregate_numeric(filter_tuple_vector, pos, aggregate_name,
                                       aggregate_tuple);
                if (RC::SUCCESS != rc) {
                    return rc;
                }
                aggregate_schema.add(AttrType::FLOATS, table_name, field_name,
                                     aggregate_name);
                break;
            }
            case AttrType::DATES:
            case AttrType::CHARS: {
                rc = aggregate_string(filter_tuple_vector, pos, aggregate_name,
                                      aggregate_tuple);
                if (RC::SUCCESS != rc) {
                    return rc;
                }
                aggregate_schema.add(AttrType::CHARS, table_name, field_name,
                                     aggregate_name);
            }
        }
    }
    result_set.set_schema(aggregate_schema);
    result_set.add(std::move(aggregate_tuple));
    return rc;
}

RC SelectExecutor::aggregate_numeric(std::vector<Tuple> &filter_tuple_vector,
                                     int pos, const char *aggregate_name,
                                     Tuple &aggregate_tuple) {
    std::vector<double> num_vector;
    for (Tuple &tuple : filter_tuple_vector) {
        const TupleValue &tuple_value = tuple.get(pos);
        double value = 0.0;
        switch (tuple_value.get_type()) {
            case AttrType::NULLS:
                continue;
            case AttrType::INTS:
                value = dynamic_cast<const IntValue &>(tuple_value).get_value();
                break;
            case AttrType::FLOATS:
                value =
                    dynamic_cast<const FloatValue &>(tuple_value).get_value();
                break;
            default:
                return RC::SQL_SYNTAX;
        }
        num_vector.push_back(value);
    }

    float num_result = 0.0;
    if (0 == strcmp(aggregate_name, "count")) {
        num_result = num_vector.size();
    } else if (num_vector.empty()) {
        aggregate_tuple.add("NULL");
        return RC::SUCCESS;
    } else if (0 == strcmp(aggregate_name, "avg")) {
        num_result =
            (float)std::accumulate(num_vector.begin(), num_vector.end(), 0) /
            num_vector.size();
    } else if (0 == strcmp(aggregate_name, "max")) {
        num_result = *std::max_element(num_vector.begin(), num_vector.end());
    } else if (0 == strcmp(aggregate_name, "min")) {
        num_result = *std::min_element(num_vector.begin(), num_vector.end());
    } else {
        return RC::SQL_SYNTAX;
    }
    aggregate_tuple.add(num_result);
    return RC::SUCCESS;
}

RC SelectExecutor::aggregate_string(std::vector<Tuple> &filter_tuple_vector,
                                    int pos, const char *aggregate_name,
                                    Tuple &aggregate_tuple) {
    std::vector<std::string> str_vector;
    for (Tuple &tuple : filter_tuple_vector) {
        const TupleValue &tuple_value = tuple.get(pos);
        const char *value = nullptr;
        switch (tuple_value.get_type()) {
            case AttrType::NULLS:
                continue;
            case AttrType::DATES:
            case AttrType::CHARS:
                value =
                    dynamic_cast<const StringValue &>(tuple_value).get_value();
                break;
            default:
                return RC::SQL_SYNTAX;
        }
        str_vector.push_back(value);
    }
    std::string result;
    if (0 == strcmp(aggregate_name, "count")) {
        result = std::to_string(str_vector.size());
    } else if (str_vector.empty()) {
        result = "NULL";
    } else if (0 == strcmp(aggregate_name, "max")) {
        result =
            std::move(*std::max_element(str_vector.begin(), str_vector.end()));
    } else if (0 == strcmp(aggregate_name, "min")) {
        result =
            std::move(*std::min_element(str_vector.begin(), str_vector.end()));
    } else {
        return RC::SQL_SYNTAX;
    }
    aggregate_tuple.add(result.c_str());
    return RC::SUCCESS;
}

RC SelectExecutor::execute(TupleSet &result_set) {
    // !zl
    RC rc = RC::SUCCESS;

    // TupleSchema tuple_schema;

    // rc = add_all_table_tuple_schema(tuple_schema);
    // if (RC::SUCCESS != rc) {
    //     return rc;
    // }

    // // !zl 关于值的条件不匹配，返回空表
    // if (!check_value_condition()) {
    //     result_set.set_tuples({});
    //     return RC::SUCCESS;
    // }

    TupleSchema tuple_schema;
    std::vector<Tuple> filter_tuple_vector;
    rc = select_filter_tuples(tuple_schema, filter_tuple_vector);
    if (rc != RC::SUCCESS) {
        return rc;
    }

    if (selects_->order_num > 0) {
        rc = order_tuples(tuple_schema, filter_tuple_vector);
        if (rc != RC::SUCCESS) {
            return rc;
        }
    }

    if (selects_->aggregate_num == 0) {
        rc = project(tuple_schema, filter_tuple_vector, result_set);
    } else {
        rc = aggregate(tuple_schema, filter_tuple_vector, result_set);
    }
    if (rc != RC::SUCCESS) {
        return rc;
    }

    return RC::SUCCESS;
}

RC SelectExecutor::execute(SessionEvent *session_event) {
    TupleSet result_set;
    RC rc = execute(result_set);
    if (rc != SUCCESS) {
        end_trx_if_need(false);
        return rc;
    }
    std::stringstream ss;
    bool is_tables = selects_->relation_num > 1;
    result_set.print(ss, is_tables);
    session_event->set_response(ss.str());
    end_trx_if_need(true);
    return RC::SUCCESS;
}

// static bool value_compare(const Value &lhs, const Value &rhs, CompOp
// comp_op)
// {
//     if (AttrType::NULLS == lhs.type && AttrType::NULLS == rhs.type &&
//         CompOp::IS_NULL == comp_op) {
//         return true;
//     }

//     if ((AttrType::CHARS == lhs.type || AttrType::DATES == lhs.type) &&
//         (AttrType::CHARS == rhs.type || AttrType::DATES == rhs.type)) {
//         int cmp_result = strcmp((const char *)lhs.data, (const char
//         *)rhs.data); return judge_cmp_result(comp_op, cmp_result);
//     }

//     if ((AttrType::INTS == lhs.type || AttrType::FLOATS == lhs.type) &&
//         (AttrType::INTS == rhs.type || AttrType::FLOATS == rhs.type)) {
//         float left_value =
//             AttrType::INTS == lhs.type ? *(int *)lhs.data : *(float
//             *)lhs.data;
//         float right_value =
//             AttrType::INTS == rhs.type ? *(int *)rhs.data : *(float
//             *)rhs.data;
//         int cmp_result =
//             FloatValue(left_value).compare(FloatValue(right_value));
//         return judge_cmp_result(comp_op, cmp_result);
//     }

//     return false;
// }

// bool SelectExecutor::check_value_condition() {
//     for (size_t i = 0; i < selects_->condition_num; ++i) {
//         const Condition &condition = selects_->conditions[i];
//         if (condition.left_is_attr == 0 && condition.right_is_attr == 0)
//         {
//             if (!value_compare(condition.left_value,
//             condition.right_value,
//                                condition.comp)) {
//                 return false;
//             }
//         }
//     }
//     return true;
// }