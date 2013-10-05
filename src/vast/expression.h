#ifndef VAST_EXPRESSION_H
#define VAST_EXPRESSION_H

#include "vast/event.h"
#include "vast/offset.h"
#include "vast/operator.h"
#include "vast/schema.h"
#include "vast/util/operators.h"
#include "vast/util/visitor.h"

namespace vast {
namespace expr {

// Forward declarations
class node;
class timestamp_extractor;
class name_extractor;
class id_extractor;
class offset_extractor;
class type_extractor;
class conjunction;
class disjunction;
class relation;
class constant;

using const_visitor = util::const_visitor<
  timestamp_extractor,
  name_extractor,
  id_extractor,
  offset_extractor,
  type_extractor,
  conjunction,
  disjunction,
  relation,
  constant
>;

using visitor = util::visitor<
  timestamp_extractor,
  name_extractor,
  id_extractor,
  offset_extractor,
  type_extractor,
  conjunction,
  disjunction,
  relation,
  constant
>;

/// The base class for nodes in the expression tree.
class node : public util::visitable_with<const_visitor>
{
public:
  node(node const&) = delete;
  node& operator=(node const&) = delete;
  node& operator=(node&&) = delete;
  virtual ~node() = default;

  /// Gets the result of the sub-tree induced by this node.
  /// @returns The value of this node.
  value const& result() const;

  /// Determines whether the result is available without evaluation.
  ///
  /// @returns `true` if the result can be obtained without a call to
  /// node::eval.
  bool ready() const;

  /// Resets the sub-tree induced by this node.
  virtual void reset();

  /// Evaluates the sub-tree induced by this node.
  virtual void eval() = 0;

protected:
  node() = default;

  value result_ = invalid;
  bool ready_ = false;
};

/// The base class for extractor nodes.
class extractor : public util::abstract_visitable<node, const_visitor>
{
public:
  virtual void feed(event const* event);

protected:
  virtual void eval() = 0;

  event const* event_;
};

/// Extracts the event timestamp.
class timestamp_extractor
  : public util::visitable<extractor, timestamp_extractor, const_visitor>
{
  virtual void eval();
};

/// Extracts the event name.
class name_extractor
  : public util::visitable<extractor, name_extractor, const_visitor>
{
  virtual void eval();
};

/// Extracts the event ID.
class id_extractor
  : public util::visitable<extractor, id_extractor, const_visitor>
{
  virtual void eval();
};

/// Extracts an argument at a given offset.
class offset_extractor
  : public util::visitable<extractor, offset_extractor, const_visitor>
{
public:
  offset_extractor(offset o);

  offset const& off() const;

private:
  virtual void eval();
  offset offset_;
};

/// Extracts arguments of a given type.
class type_extractor
  : public util::visitable<extractor, type_extractor, const_visitor>
{
public:
  type_extractor(value_type type);

  virtual void feed(event const* e);
  virtual void reset();

  value_type type() const;

private:
  virtual void eval();

  value_type type_;
  std::vector<std::pair<record const*, size_t>> pos_;
};

/// An n-ary operator.
class n_ary_operator : public util::abstract_visitable<node, const_visitor>
{
public:
  void add(std::unique_ptr<node> operand);
  virtual void reset();

  std::vector<std::unique_ptr<node>>& operands();
  std::vector<std::unique_ptr<node>> const& operands() const;

protected:
  virtual void eval() = 0;
  std::vector<std::unique_ptr<node>> operands_;
};

/// A conjunction.
class conjunction
  : public util::visitable<n_ary_operator, conjunction, const_visitor>
{
  virtual void eval();
};

/// A disjunction.
class disjunction
  : public util::visitable<n_ary_operator, disjunction, const_visitor>
{
  virtual void eval();
};

/// A relational operator.
class relation
  : public util::visitable<n_ary_operator, relation, const_visitor>
{
public:
  using binary_predicate = std::function<bool(value const&, value const&)>;

  relation(relational_operator op);

  bool test(value const& lhs, value const& rhs) const;
  relational_operator type() const;

private:
  virtual void eval();

  binary_predicate pred_;
  relational_operator op_type_;
};

/// A constant value.
class constant : public util::visitable<node, constant, const_visitor>
{
public:
  constant(value val);
  virtual void reset();

private:
  virtual void eval();
};

} // namespace expr

/// A query expression.
class expression : util::equality_comparable<expression>
{
public:
  /// Parses a given expression.
  /// @param str The query expression to transform into an AST.
  /// @param sch The schema to use to resolve event clauses.
  static expression parse(std::string const& str, schema sch = {});

  expression() = default;
  expression(expression const& other);
  expression(expression&& other);
  expression& operator=(expression const& other) = default;
  expression& operator=(expression&& other) = default;

  /// Evaluates an event with respect to the root node.
  /// @param e The event to evaluate against the expression.
  /// @returns `true` if @a event matches the expression.
  bool eval(event const& e);

  /// Allow a visitor to process the expression.
  /// @param v The visitor
  void accept(expr::const_visitor& v) const;

  /// Allow a visitor to process the expression.
  /// @param v The visitor
  void accept(expr::visitor& v);

private:
  std::string str_;
  schema schema_;
  std::unique_ptr<expr::node> root_;
  std::vector<expr::extractor*> extractors_;

private:
  friend access;
  void serialize(serializer& sink) const;
  void deserialize(deserializer& source);
  bool convert(std::string& str) const;

  friend bool operator==(expression const& x, expression const& y);
};

} // namespace vast

#endif
