/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include "vast/system/meta_index.hpp"

#include "vast/data.hpp"
#include "vast/detail/overload.hpp"
#include "vast/detail/set_operations.hpp"
#include "vast/detail/string.hpp"
#include "vast/detail/tracepoint.hpp"
#include "vast/expression.hpp"
#include "vast/fbs/utils.hpp"
#include "vast/logger.hpp"
#include "vast/synopsis.hpp"
#include "vast/system/instrumentation.hpp"
#include "vast/table_slice.hpp"
#include "vast/time.hpp"

#include <caf/binary_deserializer.hpp>
#include <caf/binary_serializer.hpp>

#include <type_traits>

namespace vast::system {

size_t meta_index_state::memusage() const {
  size_t result = 0;
  for (auto& [id, partition_synopsis] : synopses)
    result += partition_synopsis.memusage();
  return result;
}

void meta_index_state::erase(const uuid& partition) {
  synopses.erase(partition);
}

void meta_index_state::merge(const uuid& partition, partition_synopsis&& ps) {
  synopses[partition] = std::move(ps);
}

partition_synopsis& meta_index_state::at(const uuid& partition) {
  return synopses.at(partition);
}

std::vector<uuid> meta_index_state::lookup(const expression& expr) const {
  VAST_ASSERT(!caf::holds_alternative<caf::none_t>(expr));
  auto start = system::stopwatch::now();
  // TODO: we could consider a flat_set<uuid> here, which would then have
  // overloads for inplace intersection/union and simplify the implementation
  // of this function a bit. This would also simplify the maintainance of a
  // critical invariant: partition UUIDs must be sorted. Otherwise the
  // invariants of the inplace union and intersection algorithms are violated,
  // leading to wrong results. This invariant is easily violated because we
  // currently just append results to the candidate vector, so all places where
  // we return an assembled set must ensure the post-condition of returning a
  // sorted list.
  using result_type = std::vector<uuid>;
  result_type memoized_partitions;
  auto all_partitions = [&] {
    if (!memoized_partitions.empty() || synopses.empty())
      return memoized_partitions;
    memoized_partitions.reserve(synopses.size());
    std::transform(synopses.begin(), synopses.end(),
                   std::back_inserter(memoized_partitions),
                   [](auto& x) { return x.first; });
    std::sort(memoized_partitions.begin(), memoized_partitions.end());
    return memoized_partitions;
  };
  auto f = detail::overload{
    [&](const conjunction& x) -> result_type {
      VAST_ASSERT(!x.empty());
      auto i = x.begin();
      auto result = lookup(*i);
      if (!result.empty())
        for (++i; i != x.end(); ++i) {
          auto xs = lookup(*i);
          if (xs.empty())
            return xs; // short-circuit
          detail::inplace_intersect(result, xs);
          VAST_ASSERT(std::is_sorted(result.begin(), result.end()));
        }
      return result;
    },
    [&](const disjunction& x) -> result_type {
      result_type result;
      for (auto& op : x) {
        auto xs = lookup(op);
        VAST_ASSERT(std::is_sorted(xs.begin(), xs.end()));
        if (xs.size() == synopses.size())
          return xs; // short-circuit
        detail::inplace_unify(result, xs);
        VAST_ASSERT(std::is_sorted(result.begin(), result.end()));
      }
      return result;
    },
    [&](const negation&) -> result_type {
      // We cannot handle negations, because a synopsis may return false
      // positives, and negating such a result may cause false
      // negatives.
      // TODO: The above statement seems to only apply to bloom filter
      // synopses, but it should be possible to handle time or bool synopses.
      return all_partitions();
    },
    [&](const predicate& x) -> result_type {
      // Performs a lookup on all *matching* synopses with operator and
      // data from the predicate of the expression. The match function
      // uses a qualified_record_field to determine whether the synopsis should
      // be queried.
      auto search = [&](auto match) {
        VAST_ASSERT(caf::holds_alternative<data>(x.rhs));
        auto& rhs = caf::get<data>(x.rhs);
        result_type result;
        for (auto& [part_id, part_syn] : synopses) {
          for (auto& [field, syn] : part_syn.field_synopses_) {
            if (match(field)) {
              auto cleaned_type = vast::type{field.type}.attributes({});
              // We rely on having a field -> nullptr mapping here for the
              // fields that don't have their own synopsis.
              if (syn) {
                auto opt = syn->lookup(x.op, make_view(rhs));
                if (!opt || *opt) {
                  VAST_TRACE("{} selects {} at predicate {}",
                             detail::pretty_type_name(this), part_id, x);
                  result.push_back(part_id);
                  break;
                }
                // The field has no dedicated synopsis. Check if there is one
                // for the type in general.
              } else if (auto it = part_syn.type_synopses_.find(cleaned_type);
                         it != part_syn.type_synopses_.end() && it->second) {
                auto opt = it->second->lookup(x.op, make_view(rhs));
                if (!opt || *opt) {
                  VAST_TRACE("{} selects {} at predicate {}",
                             detail::pretty_type_name(this), part_id, x);
                  result.push_back(part_id);
                  break;
                }
              } else {
                // The meta index couldn't rule out this partition, so we have
                // to include it in the result set.
                result.push_back(part_id);
                break;
              }
            }
          }
        }
        VAST_DEBUG(
          "{} checked {} partitions for predicate {} and got {} results",
          detail::pretty_type_name(this), synopses.size(), x, result.size());
        // Some calling paths require the result to be sorted.
        std::sort(result.begin(), result.end());
        return result;
      };
      auto extract_expr = detail::overload{
        [&](const meta_extractor& lhs, const data& d) -> result_type {
          if (lhs.kind == meta_extractor::type) {
            // We don't have to look into the synopses for type queries, just
            // at the layout names.
            result_type result;
            for (auto& [part_id, part_syn] : synopses) {
              for (auto& pair : part_syn.field_synopses_) {
                // TODO: provide an overload for view of evaluate() so that
                // we can use string_view here. Fortunately type names are
                // short, so we're probably not hitting the allocator due to
                // SSO.
                auto type_name = data{pair.first.layout_name};
                if (evaluate(type_name, x.op, d)) {
                  result.push_back(part_id);
                  break;
                }
              }
            }
            // Re-establish potentially violated invariant.
            std::sort(result.begin(), result.end());
            return result;
          } else if (lhs.kind == meta_extractor::field) {
            // We don't have to look into the synopses for type queries, just
            // at the layout names.
            result_type result;
            auto s = caf::get_if<std::string>(&d);
            if (!s) {
              VAST_WARN("#field meta queries only support string "
                        "comparisons");
            } else {
              for (const auto& synopsis : synopses) {
                // Compare the desired field name with each field in the
                // partition.
                auto matching = [&] {
                  for (const auto& pair : synopsis.second.field_synopses_) {
                    auto fqn = pair.first.fqn();
                    if (detail::ends_with(fqn, *s))
                      return true;
                  }
                  return false;
                }();
                // Only insert the partition if both sides are equal, i.e. the
                // operator is "positive" and matching is true, or both are
                // negative.
                if (!is_negated(x.op) == matching)
                  result.push_back(synopsis.first);
              }
            }
            // Re-establish potentially violated invariant.
            std::sort(result.begin(), result.end());
            return result;
          }
          VAST_WARN("{} cannot process attribute extractor: {}",
                    detail::pretty_type_name(this), lhs.kind);
          return all_partitions();
        },
        [&](const field_extractor& lhs, const data&) -> result_type {
          auto pred = [&](auto& field) {
            return detail::ends_with(field.fqn(), lhs.field);
          };
          return search(pred);
        },
        [&](const type_extractor& lhs, const data&) -> result_type {
          auto result = [&] {
            if (caf::holds_alternative<none_type>(lhs.type)) {
              VAST_ASSERT(!lhs.type.name().empty());
              auto pred = [&](auto& field) {
                return field.type.name() == lhs.type.name();
              };
              return search(pred);
            }
            auto pred = [&](auto& field) {
              return field.type == lhs.type && field.type.name().empty();
            };
            return search(pred);
          }();
          // Preserve compatibility with databases that were created beore
          // the #timestamp attribute was removed.
          if (lhs.type.name() == "timestamp") {
            auto pred = [](auto& field) {
              return has_attribute(field.type, "timestamp");
            };
            detail::inplace_unify(result, search(pred));
          }
          return result;
        },
        [&](const auto&, const auto&) -> result_type {
          VAST_WARN("{} cannot process predicate: {}",
                    detail::pretty_type_name(this), x);
          return all_partitions();
        },
      };
      return caf::visit(extract_expr, x.lhs, x.rhs);
    },
    [&](caf::none_t) -> result_type {
      VAST_ERROR("{} received an empty expression",
                 detail::pretty_type_name(this));
      VAST_ASSERT(!"invalid expression");
      return all_partitions();
    },
  };
  auto result = caf::visit(f, expr);
  auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
    system::stopwatch::now() - start);
  VAST_DEBUG("meta index lookup found {} candidates in {} microseconds",
             result.size(), delta.count());
  VAST_TRACEPOINT(meta_index_lookup, delta.count(), result.size());
  return result;
}

meta_index_actor::behavior_type
meta_index(meta_index_actor::stateful_pointer<meta_index_state> self) {
  self->state.self = self;
  return {
    [=](atom::merge,
        std::shared_ptr<std::map<uuid, partition_synopsis>>& ps) -> atom::ok {
      for (auto&& [id, synopsis] : std::move(*ps)) {
        self->state.merge(std::move(id), std::move(synopsis));
      }
      return atom::ok_v;
    },
    [=](atom::merge, uuid partition,
        std::shared_ptr<partition_synopsis>& synopsis) -> atom::ok {
      VAST_TRACE_SCOPE("{} {} {}", self, VAST_ARG(partition),
                       VAST_ARG(synopsis));
      self->state.merge(std::move(partition), std::move(*synopsis));
      return atom::ok_v;
    },
    [=](expression expr) -> std::vector<uuid> {
      VAST_TRACE_SCOPE("{} {}", self, VAST_ARG(expr));
      return self->state.lookup(expr);
    },
  };
}

} // namespace vast::system
