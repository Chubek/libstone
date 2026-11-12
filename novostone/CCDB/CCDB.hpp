#pragma once
#include "novostone/Datalog/Datalog.hpp"

#include <functional>
#include <memory>
#include <stdexcept>

namespace novostone::ccdb {
using Revision = std::uint64_t;
using Entity = std::int64_t;
class Conflict : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct State;
struct Control;
struct QueryResult : datalog::Result { Revision revision = 0; bool cached = false; };
class Snapshot {
public:
  Revision revision() const;
  const datalog::Facts& facts() const;
  const datalog::Relation& relation(const std::string& name) const;
  QueryResult query(const std::string& name, datalog::Limits limits = {}) const;
  std::string dump() const;
  bool same_revision(const Snapshot& other) const { return state_ == other.state_; }
private:
  explicit Snapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
  std::shared_ptr<const State> state_;
  friend class Database;
  friend class Transaction;
};
using Validator = std::function<void(const Snapshot&)>;
struct Event {
  Revision revision;
  std::string reason;
  std::set<std::string> changed;
  Revision source = 0;
  Revision parent = 0;
  std::set<std::string> reads;
};
struct RestoreLimits { std::size_t bytes = 16 * 1024 * 1024; std::size_t facts = 1000000; };
class Transaction {
public:
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  Transaction(Transaction&&) noexcept = default;
  Transaction& operator=(Transaction&&) noexcept = default;
  void insert(const std::string& relation, datalog::Tuple tuple);
  void erase(const std::string& relation, const datalog::Tuple& tuple);
  const datalog::Relation& relation(const std::string& name);
  QueryResult query(const std::string& name, datalog::Limits limits = {});
  // Register facts consumed outside the transaction, from its exact base only.
  void depend(const Snapshot& source, const std::set<std::string>& relations);
  // Reserve imported identities. Reservations survive aborts; IDs are never recycled.
  void reserve(Entity through);
  Snapshot preview() const;
  Snapshot commit(std::string reason = {}, Validator validator = {});
  // Opt-in relation-granularity serializable rebase. All external reads must be
  // registered with depend(); transaction reads register themselves automatically.
  Snapshot commit_disjoint(std::string reason = {}, Validator validator = {}, std::size_t retries = 8);
  void abort();
private:
  Transaction(std::shared_ptr<Control> control, Snapshot base);
  void active() const;
  Snapshot tentative() const;
  std::shared_ptr<Control> control_;
  Snapshot base_;
  datalog::Facts facts_;
  mutable std::set<std::string> reads_;
  std::set<std::string> writes_;
  friend class Database;
};
class Database {
public:
  explicit Database(datalog::Program program, Validator validator = {});
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Snapshot snapshot() const;
  Transaction begin();
  Transaction begin(const Snapshot& expected);
  Entity allocate();
  std::vector<Event> events() const;
  std::string checkpoint() const;
  // Restores primary facts as a new revision; never restores cached analyses.
  Snapshot restore(const Snapshot& expected, const std::string& checkpoint, RestoreLimits limits = {});
private:
  std::shared_ptr<Control> control_;
};
} // namespace novostone::ccdb
