//
// Created by qiyu on 10/20/17.
//

#ifndef ORM_MYSQL_HPP
#define ORM_MYSQL_HPP
#include <climits>
#include <list>
#include <map>
#include <string_view>
#include <utility>

#include "entity.hpp"
#include "type_mapping.hpp"
#include "utility.hpp"

namespace ormpp {

using blob = ormpp_mysql::blob;

class mysql {
 public:
  ~mysql() { disconnect(); }

  template <typename... Args>
  bool connect(Args &&...args) {
    if (con_ != nullptr) {
      mysql_close(con_);
    }

    con_ = mysql_init(nullptr);
    if (con_ == nullptr) {
      set_last_error("mysql init failed");
      return false;
    }

    int timeout = -1;
    auto tp = get_tp(timeout, std::forward<Args>(args)...);

    if (timeout > 0) {
      if (mysql_options(con_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0) {
        set_last_error(mysql_error(con_));
        return false;
      }
    }

    char value = 1;
    mysql_options(con_, MYSQL_OPT_RECONNECT, &value);
    mysql_options(con_, MYSQL_SET_CHARSET_NAME, "utf8");

    if (std::apply(&mysql_real_connect, tp) == nullptr) {
      set_last_error(mysql_error(con_));
      return false;
    }

    reset_error();

    return true;
  }

  void set_last_error(std::string last_error) {
    last_error_ = std::move(last_error);
    std::cout << last_error_ << std::endl;  // todo, write to log file
  }

  std::string get_last_error() const { return last_error_; }

  bool ping() { return mysql_ping(con_) == 0; }

  template <typename... Args>
  bool disconnect(Args &&...args) {
    if (con_ != nullptr) {
      mysql_close(con_);
      con_ = nullptr;
    }

    return true;
  }

  template <typename T, typename... Args>
  bool create_datatable(Args &&...args) {
    reset_error();
    std::string sql = generate_createtb_sql<T>(std::forward<Args>(args)...);
    sql += " DEFAULT CHARSET=utf8";
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    if (mysql_query(con_, sql.data())) {
      fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

  template <typename T, typename... Args>
  int insert(const std::vector<T> &t, Args &&...args) {
    reset_error();
    auto name = get_name<T>();
    std::string sql = auto_key_map_[name].empty()
                          ? generate_insert_sql<T>(false)
                          : generate_auto_insert_sql<T>(auto_key_map_, false);

    return insert_impl(sql, t, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  int update(const std::vector<T> &t, Args &&...args) {
    reset_error();
    std::string sql = generate_insert_sql<T>(true);

    return insert_impl(sql, t, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  int insert(const T &t, Args &&...args) {
    reset_error();
    // insert into person values(?, ?, ?);
    auto name = get_name<T>();
    std::string sql = auto_key_map_[name].empty()
                          ? generate_insert_sql<T>(false)
                          : generate_auto_insert_sql<T>(auto_key_map_, false);

    return insert_impl(sql, t, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  int update(const T &t, Args &&...args) {
    reset_error();
    std::string sql = generate_insert_sql<T>(true);
    return insert_impl(sql, t, std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  bool delete_records(Args &&...where_conditon) {
    reset_error();
    auto sql = generate_delete_sql<T>(std::forward<Args>(where_conditon)...);
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    if (mysql_query(con_, sql.data())) {
      fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

  int get_last_affect_rows() { return (int)mysql_affected_rows(con_); }

  // for tuple and string with args...
  template <typename T, typename Arg, typename... Args>
  std::enable_if_t<!iguana::is_reflection_v<T>, std::vector<T>> query(
      const Arg &s, Args &&...args) {
    reset_error();
    static_assert(iguana::is_tuple<T>::value);
    constexpr auto SIZE = std::tuple_size_v<T>;

    std::string sql = s;
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    constexpr auto Args_Size = sizeof...(Args);
    if constexpr (Args_Size != 0) {
      if (Args_Size != std::count(sql.begin(), sql.end(), '?')) {
        has_error_ = true;
        return {};
      }

      sql = get_sql(sql, std::forward<Args>(args)...);
    }

    stmt_ = mysql_stmt_init(con_);
    if (!stmt_) {
      has_error_ = true;
      return {};
    }

    auto guard = guard_statment(stmt_);

    if (mysql_stmt_prepare(stmt_, sql.c_str(), (int)sql.size())) {
      fprintf(stderr, "%s\n", mysql_error(con_));
      has_error_ = true;
      return {};
    }

    std::array<MYSQL_BIND, result_size<T>::value> param_binds = {};
    std::list<std::vector<char>> mp;

    std::vector<T> v;
    T tp{};

    size_t index = 0;
    iguana::for_each(
        tp,
        [&param_binds, &mp, &index](auto &item, auto /*I*/) {
          using U = std::remove_reference_t<decltype(item)>;
          if constexpr (std::is_arithmetic_v<U>) {
            param_binds[index].buffer_type =
                (enum_field_types)ormpp_mysql::type_to_id(identity<U>{});
            param_binds[index].buffer = &item;
            index++;
          }
          else if constexpr (std::is_same_v<std::string, U>) {
            std::vector<char> tmp(65536, 0);
            mp.emplace_back(std::move(tmp));
            param_binds[index].buffer_type = MYSQL_TYPE_STRING;
            param_binds[index].buffer = &(mp.back()[0]);
            param_binds[index].buffer_length = 65536;
            index++;
          }
          else if constexpr (iguana::is_reflection_v<U>) {
            iguana::for_each(item, [&param_binds, &mp, &item, &index](
                                       auto &ele, auto /*i*/) {
              using V =
                  std::remove_reference_t<decltype(std::declval<U>().*ele)>;
              if constexpr (std::is_arithmetic_v<V>) {
                param_binds[index].buffer_type =
                    (enum_field_types)ormpp_mysql::type_to_id(identity<V>{});
                param_binds[index].buffer = &(item.*ele);
              }
              else if constexpr (std::is_same_v<std::string, V>) {
                std::vector<char> tmp(65536, 0);
                mp.emplace_back(std::move(tmp));
                param_binds[index].buffer_type = MYSQL_TYPE_STRING;
                param_binds[index].buffer = &(mp.back()[0]);
                param_binds[index].buffer_length = 65536;
              }
              else if constexpr (is_char_array_v<V>) {
                std::vector<char> tmp(sizeof(V), 0);
                mp.emplace_back(std::move(tmp));
                param_binds[index].buffer_type = MYSQL_TYPE_VAR_STRING;
                param_binds[index].buffer = &(mp.back()[0]);
                param_binds[index].buffer_length = (unsigned long)sizeof(V);
              }
              else if constexpr (std::is_same_v<blob, V>) {
                std::vector<char> tmp(65536, 0);
                mp.emplace_back(std::move(tmp));
                param_binds[index].buffer_type = MYSQL_TYPE_BLOB;
                param_binds[index].buffer = &(mp.back()[0]);
                param_binds[index].buffer_length = 65536;
              }
              else {
                static_assert(!sizeof(V), "this type has not supported yet");
              }
              index++;
            });
          }
          else if constexpr (is_char_array_v<U>) {
            param_binds[index].buffer_type = MYSQL_TYPE_VAR_STRING;
            std::vector<char> tmp(sizeof(U), 0);
            mp.emplace_back(std::move(tmp));
            param_binds[index].buffer = &(mp.back()[0]);
            param_binds[index].buffer_length = (unsigned long)sizeof(U);
            index++;
          }
          else if constexpr (std::is_same_v<blob, U>) {
            std::vector<char> tmp(65536, 0);
            mp.emplace_back(std::move(tmp));
            param_binds[index].buffer_type = MYSQL_TYPE_BLOB;
            param_binds[index].buffer = &(mp.back()[0]);
            param_binds[index].buffer_length = 65536;
            index++;
          }
          else {
            static_assert(!sizeof(U), "this type has not supported yet");
          }
        },
        std::make_index_sequence<SIZE>{});

    if (mysql_stmt_bind_result(stmt_, &param_binds[0])) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      has_error_ = true;
      return {};
    }

    if (mysql_stmt_execute(stmt_)) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      has_error_ = true;
      return {};
    }

    while (mysql_stmt_fetch(stmt_) == 0) {
      auto column = 0;
      auto it = mp.begin();
      iguana::for_each(
          tp,
          [&mp, &it, &column, this](auto &item, auto /*i*/) {
            using W = std::remove_reference_t<decltype(item)>;
            if constexpr (std::is_arithmetic_v<W>) {
              // return; // don't return, need ++column at end.
            }
            else if constexpr (std::is_same_v<std::string, W>) {
              item = std::string(&(*it)[0], strlen((*it).data()));
              it++;
            }
            else if constexpr (is_char_array_v<W>) {
              memcpy(item, &(*it)[0], sizeof(W));
              it++;
            }
            else if constexpr (iguana::is_reflection_v<W>) {
              iguana::for_each(item, [&it, &item, &column, this](auto ele,
                                                                 auto /*i*/) {
                using V =
                    std::remove_reference_t<decltype(std::declval<W>().*ele)>;
                if constexpr (std::is_arithmetic_v<V>) {
                  // item.*ele = *(V *)(&(*it)[0]);
                }
                else if constexpr (std::is_same_v<std::string, V>) {
                  item.*ele = std::string(&(*it)[0], strlen((*it).data()));
                  it++;
                }
                else if constexpr (is_char_array_v<V>) {
                  memcpy(item.*ele, &(*it)[0], sizeof(V));
                }
                else if constexpr (std::is_same_v<blob, V>) {
                  (item.*ele).assign((*it).data(),
                                     (*it).data() + get_blob_len(column));
                  it++;
                }
                else {
                  static_assert(!sizeof(V), "this type has not supported yet");
                }
                ++column;
              });
              return;
            }
            else if constexpr (std::is_same_v<blob, W>) {
              item.assign((*it).data(), (*it).data() + get_blob_len(column));
              it++;
            }
            else {
              static_assert(!sizeof(W), "this type has not supported yet");
            }
            ++column;
          },
          std::make_index_sequence<SIZE>{});

      if (index > 0)
        v.push_back(std::move(tp));
    }

    return v;
  }

  // if there is a sql error, how to tell the user? throw exception?
  template <typename T, typename... Args>
  std::enable_if_t<iguana::is_reflection_v<T>, std::vector<T>> query(
      Args &&...args) {
    reset_error();
    std::string sql = generate_query_sql<T>(args...);
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    constexpr auto SIZE = iguana::get_value<T>();

    stmt_ = mysql_stmt_init(con_);
    if (!stmt_) {
      has_error_ = true;
      return {};
    }

    auto guard = guard_statment(stmt_);

    if (mysql_stmt_prepare(stmt_, sql.c_str(), (unsigned long)sql.size())) {
      has_error_ = true;
      return {};
    }

    std::array<MYSQL_BIND, SIZE> param_binds = {};
    std::map<size_t, std::vector<char>> mp;

    std::vector<T> v;
    T t{};
    int index = 0;
    iguana::for_each(t, [&](auto item, auto i) {
      constexpr auto Idx = decltype(i)::value;
      using U = std::remove_reference_t<decltype(std::declval<T>().*item)>;
      if constexpr (std::is_arithmetic_v<U>) {
        param_binds[Idx].buffer_type =
            (enum_field_types)ormpp_mysql::type_to_id(identity<U>{});
        param_binds[Idx].buffer = &(t.*item);
        index++;
      }
      else if constexpr (std::is_same_v<std::string, U>) {
        param_binds[Idx].buffer_type = MYSQL_TYPE_STRING;
        std::vector<char> tmp(65536, 0);
        mp.emplace(decltype(i)::value, tmp);
        param_binds[Idx].buffer = &(mp.rbegin()->second[0]);
        param_binds[Idx].buffer_length = (unsigned long)tmp.size();
        index++;
      }
      else if constexpr (is_char_array_v<U>) {
        param_binds[Idx].buffer_type = MYSQL_TYPE_VAR_STRING;
        std::vector<char> tmp(sizeof(U), 0);
        mp.emplace(decltype(i)::value, tmp);
        param_binds[Idx].buffer = &(mp.rbegin()->second[0]);
        param_binds[Idx].buffer_length = (unsigned long)sizeof(U);
        index++;
      }
      else if constexpr (std::is_same_v<blob, U>) {
        std::vector<char> tmp(65536, 0);
        mp.emplace(decltype(i)::value, std::move(tmp));
        param_binds[index].buffer_type = MYSQL_TYPE_BLOB;
        param_binds[index].buffer = &(mp.rbegin()->second[0]);
        param_binds[index].buffer_length = 65536;
        index++;
      }
    });

    if (index == 0) {
      return {};
    }

    if (mysql_stmt_bind_result(stmt_, &param_binds[0])) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      has_error_ = true;
      return {};
    }

    if (mysql_stmt_execute(stmt_)) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      has_error_ = true;
      return {};
    }

    while (mysql_stmt_fetch(stmt_) == 0) {
      auto column = 0;
      iguana::for_each(t, [&mp, &t, &column, this](auto item, auto i) {
        using U = std::remove_reference_t<decltype(std::declval<T>().*item)>;
        if constexpr (std::is_same_v<std::string, U>) {
          auto &vec = mp[decltype(i)::value];
          t.*item = std::string(&vec[0], strlen(vec.data()));
        }
        else if constexpr (is_char_array_v<U>) {
          auto &vec = mp[decltype(i)::value];
          memcpy(t.*item, vec.data(), vec.size());
        }
        else if constexpr (std::is_same_v<blob, U>) {
          auto &vec = mp[decltype(i)::value];
          t.*item = blob(vec.data(), vec.data() + get_blob_len(column));
        }
        ++column;
      });

      for (auto &p : mp) {
        p.second.assign(p.second.size(), 0);
      }

      v.push_back(std::move(t));
      iguana::for_each(t, [&mp, &t](auto item, auto /*i*/) {
        using U = std::remove_reference_t<decltype(std::declval<T>().*item)>;
        if constexpr (std::is_arithmetic_v<U>) {
          memset(&(t.*item), 0, sizeof(U));
        }
      });
    }

    return v;
  }

  int get_blob_len(int column) {
    unsigned long data_len = 0;

    MYSQL_BIND param;
    memset(&param, 0, sizeof(MYSQL_BIND));
    param.length = &data_len;
    param.buffer_type = MYSQL_TYPE_BLOB;

    auto retcode = mysql_stmt_fetch_column(stmt_, &param, column, 0);
    if (retcode != 0) {
      set_last_error(mysql_stmt_error(stmt_));
      return 0;
    }

    return static_cast<int>(data_len);
  }

  bool has_error() { return has_error_; }
  void reset_error() {
    has_error_ = false;
    last_error_ = {};
  }

  // just support execute string sql without placeholders
  bool execute(const std::string &sql) {
    if (mysql_query(con_, sql.data()) != 0) {
      fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

  // transaction
  bool begin() {
    if (mysql_query(con_, "BEGIN")) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

  bool commit() {
    if (mysql_query(con_, "COMMIT")) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

  bool rollback() {
    if (mysql_query(con_, "ROLLBACK")) {
      //                fprintf(stderr, "%s\n", mysql_error(con_));
      return false;
    }

    return true;
  }

 private:
  template <typename T, typename... Args>
  std::string generate_createtb_sql(Args &&...args) {
    const auto type_name_arr = get_type_names<T>(DBType::mysql);
    auto name = get_name<T>();
    std::string sql =
        std::string("CREATE TABLE IF NOT EXISTS ") + name.data() + "(";
    auto arr = iguana::get_array<T>();
    constexpr auto SIZE = sizeof...(Args);
    auto_key_map_[name.data()] = "";

    // auto_increment_key and key can't exist at the same time
    using U = std::tuple<std::decay_t<Args>...>;
    if constexpr (SIZE > 0) {
      // using U = std::tuple<std::decay_t <Args>...>;//the code can't compile
      // in vs
      static_assert(!(iguana::has_type<ormpp_key, U>::value &&
                      iguana::has_type<ormpp_auto_key, U>::value),
                    "should only one key");
    }
    auto tp = sort_tuple(std::make_tuple(std::forward<Args>(args)...));
    const size_t arr_size = arr.size();
    for (size_t i = 0; i < arr_size; ++i) {
      auto field_name = arr[i];
      bool has_add_field = false;
      for_each0(
          tp,
          [&sql, &i, &has_add_field, field_name, type_name_arr, name,
           this](auto item) {
            if constexpr (std::is_same_v<decltype(item), ormpp_not_null>) {
              if (item.fields.find(field_name.data()) == item.fields.end())
                return;
            }
            else {
              if (item.fields != field_name.data())
                return;
            }

            if constexpr (std::is_same_v<decltype(item), ormpp_not_null>) {
              if (!has_add_field) {
                append(sql, field_name.data(), " ", type_name_arr[i]);
              }
              append(sql, " NOT NULL");
              has_add_field = true;
            }
            else if constexpr (std::is_same_v<decltype(item), ormpp_key>) {
              if (!has_add_field) {
                append(sql, field_name.data(), " ", type_name_arr[i]);
              }
              append(sql, " PRIMARY KEY");
              has_add_field = true;
            }
            else if constexpr (std::is_same_v<decltype(item), ormpp_auto_key>) {
              if (!has_add_field) {
                append(sql, field_name.data(), " ", type_name_arr[i]);
              }
              append(sql, " AUTO_INCREMENT");
              append(sql, " PRIMARY KEY");
              auto_key_map_[name.data()] = item.fields;
              has_add_field = true;
            }
            else if constexpr (std::is_same_v<decltype(item), ormpp_unique>) {
              if (!has_add_field) {
                append(sql, field_name.data(), " ", type_name_arr[i]);
              }

              append(sql, ", UNIQUE(", item.fields, ")");
              has_add_field = true;
            }
            else {
              append(sql, field_name.data(), " ", type_name_arr[i]);
            }
          },
          std::make_index_sequence<SIZE>{});

      if (!has_add_field) {
        append(sql, field_name.data(), " ", type_name_arr[i]);
      }

      if (i < arr_size - 1)
        sql += ", ";
    }

    sql += ")";

    return sql;
  }

  template <typename T>
  constexpr void set_param_bind(std::vector<MYSQL_BIND> &param_binds,
                                T &&value) {
    MYSQL_BIND param = {};
    using U = std::remove_const_t<std::remove_reference_t<T>>;
    if constexpr (is_optional_v<U>::value) {
      if (value.has_value()) {
        return set_param_bind(param_binds, std::move(value.value()));
      }
      else {
        param.buffer_type = MYSQL_TYPE_NULL;
      }
    }
    else if constexpr (std::is_arithmetic_v<U>) {
      param.buffer_type =
          (enum_field_types)ormpp_mysql::type_to_id(identity<U>{});
      param.buffer = const_cast<void *>(static_cast<const void *>(&value));
    }
    else if constexpr (std::is_same_v<std::string, U>) {
      param.buffer_type = MYSQL_TYPE_STRING;
      param.buffer = (void *)(value.c_str());
      param.buffer_length = (unsigned long)value.size();
    }
    else if constexpr (std::is_same_v<const char *, U> || is_char_array_v<U>) {
      param.buffer_type = MYSQL_TYPE_STRING;
      param.buffer = (void *)(value);
      param.buffer_length = (unsigned long)strlen(value);
    }
    else if constexpr (std::is_same_v<blob, U>) {
      param.buffer_type = MYSQL_TYPE_BLOB;
      param.buffer = (void *)(value.data());
      param.buffer_length = (unsigned long)value.size();
    }
    else {
      static_assert(!sizeof(U), "this type has not supported yet");
    }
    param_binds.push_back(param);
  }

  template <typename T>
  int stmt_execute(const T &t) {
    std::vector<MYSQL_BIND> param_binds;
    auto it = auto_key_map_.find(get_name<T>());
    std::string auto_key = (it == auto_key_map_.end()) ? "" : it->second;

    iguana::for_each(
        t, [&t, &param_binds, &auto_key, this](const auto &v, auto /*i*/) {
          /*if (!auto_key.empty() && auto_key ==
             iguana::get_name<T>(decltype(i)::value).data()) return;*/

          set_param_bind(param_binds, t.*v);
        });

    if (mysql_stmt_bind_param(stmt_, &param_binds[0])) {
      set_last_error(mysql_error(con_));
      return INT_MIN;
    }

    if (mysql_stmt_execute(stmt_)) {
      set_last_error(mysql_error(con_));
      return INT_MIN;
    }

    int count = (int)mysql_stmt_affected_rows(stmt_);
    if (count == 0) {
      return INT_MIN;
    }

    return count;
  }

  struct guard_statment {
    guard_statment(MYSQL_STMT *stmt) : stmt_(stmt) {}
    MYSQL_STMT *stmt_ = nullptr;
    int status_ = 0;
    ~guard_statment() {
      if (stmt_ != nullptr)
        status_ = mysql_stmt_close(stmt_);

      if (status_)
        fprintf(stderr, "close statment error code %d\n", status_);
    }
  };

  template <typename T, typename... Args>
  int insert_impl(const std::string &sql, const T &t, Args &&...args) {
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    stmt_ = mysql_stmt_init(con_);
    if (!stmt_)
      return INT_MIN;

    if (mysql_stmt_prepare(stmt_, sql.c_str(), (int)sql.size())) {
      return INT_MIN;
    }

    auto guard = guard_statment(stmt_);

    if (stmt_execute(t) < 0)
      return INT_MIN;

    return 1;
  }

  template <typename T, typename... Args>
  int insert_impl(const std::string &sql, const std::vector<T> &t,
                  Args &&...args) {
#if ORMPP_ENABLE_LOG
    std::cout << sql << std::endl;
#endif
    stmt_ = mysql_stmt_init(con_);
    if (!stmt_)
      return INT_MIN;

    if (mysql_stmt_prepare(stmt_, sql.c_str(), (int)sql.size())) {
      return INT_MIN;
    }

    auto guard = guard_statment(stmt_);

    // transaction
    bool b = begin();
    if (!b)
      return INT_MIN;

    for (auto &item : t) {
      int r = stmt_execute(item);
      if (r == INT_MIN) {
        rollback();
        return INT_MIN;
      }
    }
    b = commit();

    return b ? (int)t.size() : INT_MIN;
  }

  template <typename... Args>
  auto get_tp(int &timeout, Args &&...args) {
    auto tp = std::make_tuple(con_, std::forward<Args>(args)...);
    if constexpr (sizeof...(Args) == 5) {
      auto [c, s1, s2, s3, s4, i] = tp;
      timeout = i;
      return std::make_tuple(c, s1, s2, s3, s4, 0, nullptr, 0);
    }
    else if constexpr (sizeof...(Args) == 6) {
      auto [c, s1, s2, s3, s4, i, port] = tp;
      timeout = i;
      return std::make_tuple(c, s1, s2, s3, s4, port, nullptr, 0);
    }
    else {
      return std::tuple_cat(tp, std::make_tuple(0, nullptr, 0));
    }
  }

 private:
  MYSQL *con_ = nullptr;
  MYSQL_STMT *stmt_ = nullptr;
  bool has_error_ = false;
  std::string last_error_;
  inline static std::map<std::string, std::string> auto_key_map_;
};
}  // namespace ormpp

#endif  // ORM_MYSQL_HPP
