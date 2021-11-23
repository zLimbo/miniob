
// ! zlimbo

#ifndef __SELECT_EXECUTOR_H__
#define __SELECT_EXECUTOR_H__

#include <sstream>
#include <vector>

#include "rc.h"
#include "sql/parser/parse.h"

struct TupleCons {
    const Condition *condition;
    AttrType type;
    int left_value_pos;
    int right_value_pos;
};

class Tuple;
class TupleSchema;
class TupleSet;

class TupleFilter {
public:
    RC init(const TupleSchema &tuple_schema, const Selects &selects,
            const char *db);
    RC init(const TupleSchema &left_schema, const TupleSchema &right_schema,
            const Selects &selects, const char *db);
    bool filter(const Tuple &tuple);
    bool filter(const Tuple &left_tuple, const Tuple &right_tuple);

private:
    std::vector<TupleCons> tuple_cons_vector_;
};

class TupleProjcet {
public:
    RC init(TupleSchema &tuple_schema, TupleSchema &select_tuple_schema);
    RC project(Tuple &tuple, Tuple &new_tuple);

private:
    std::vector<int> select_tuple_pos_vector_;
};

class Trx;
class Session;
class SelectExeNode;
class SessionEvent;

class SelectExecutor {
public:
    SelectExecutor(Session *session, Trx *trx, const char *db,
                   const Selects *selects);

    bool match_table(const char *table_name_in_condition,
                     const char *table_name_to_match);
    const char *get_unique_table_name(const TupleSchema &tuple_schema,
                                      const char *field_name);
    RC create_select_exe_node(const char *table_name,
                              SelectExeNode &select_node);
    RC add_single_table_tuple_schema(const char *table_name,
                                     TupleSchema &tuple_schema);
    RC add_all_table_tuple_schema(TupleSchema &tuple_schema);
    RC get_select_tuple_schema(TupleSchema &tuple_schema,
                               TupleSchema &select_tuple_schema);
    RC schema_add_field(const char *table_name, const char *field_name,
                        TupleSchema &schema);
    RC select_all_table_tuple_sets(std::vector<TupleSet> &tuple_sets);
    RC select_filter_tuples(TupleSchema &tuple_schema,
                            std::vector<Tuple> &filter_tuple_vector);
    RC order_tuples(TupleSchema &tuple_schema,
                    std::vector<Tuple> &filter_tuple_vector);
    RC project(TupleSchema &tuple_schema,
               std::vector<Tuple> &filter_tuple_vector, TupleSet &result);
    RC aggregate(TupleSchema &tuple_schema,
                 std::vector<Tuple> &filter_tuple_vector, TupleSet &result);
    RC aggregate_numeric(std::vector<Tuple> &filter_tuple_vector, int pos,
                         const char *aggregate_name, Tuple &aggregate_tuple);
    RC aggregate_string(std::vector<Tuple> &filter_tuple_vector, int pos,
                        const char *aggregate_name, Tuple &aggregate_tuple);
    bool check_value_condition();
    RC execute(TupleSet &result);
    RC execute(SessionEvent *session_event);
    void end_trx_if_need(bool all_right);

private:
    Session *session_ = nullptr;
    Trx *trx_ = nullptr;
    const char *db_ = nullptr;
    const Selects *selects_ = nullptr;
};

#endif