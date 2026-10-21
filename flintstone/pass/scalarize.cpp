#include "../internal.hpp"

namespace flintstone {
namespace {

// Both aggregate eliminations share one shape: reject escaping values
// (parameters, results, calls, stores, returns), pre-map predicated phis to
// fresh scalar identities (so loop backedges resolve), fixpoint-expand
// producers whose dependencies are all mapped, then splice in the scalar
// phis. Anything left over fails explicitly instead of being silently
// dropped or miscompiled.

[[noreturn]] void aggregate_escape(const char* code, const std::string& what, const Location& location) {
    detail::fail(code, what + "; aggregates must be eliminated intra-function", location);
}

std::set<ValueId> aggregate_values(const Function& function, TypeKind aggregate) {
    std::set<ValueId> result;
    for (const auto& block : function.blocks)
        for (const auto& op : block.operations)
            for (const auto& value : op.results)
                if (value.type.kind == aggregate) result.insert(value.id);
    return result;
}

void check_signatures(const Function& function, TypeKind aggregate, const char* code) {
    for (const auto& parameter : function.parameters)
        if (parameter.type.kind == aggregate)
            aggregate_escape(code, "aggregate parameter %" + std::to_string(parameter.id), parameter.location);
    if (function.result.kind == aggregate) aggregate_escape(code, "aggregate function result", function.location);
}

} // namespace

Pass vector_scalarize_pass() {
    return {"vector-scalarize", {}, {"cfg", "dominators", "loops"},
        [](Module& module, const Registry&, AnalysisManager&) {
            bool changed = false;
            for (auto& function : module.functions) {
                if (function.external) continue;
                bool present = false;
                for (const auto& block : function.blocks)
                    for (const auto& op : block.operations)
                        if (op.name.rfind("vector.", 0) == 0) present = true;
                if (!present) continue;
                changed = true;
                check_signatures(function, TypeKind::Vector, "vector.escape");
                const auto vectors = aggregate_values(function, TypeKind::Vector);
                ValueId fresh = function.fresh_value();
                auto temp = [&] {
                    detail::check(fresh < invalid_id - 1, "id.exhausted", "scalarization exhausted value IDs");
                    return fresh++;
                };
                struct Expansion {
                    std::vector<Operation> replacement;
                    std::vector<ValueId> lanes;
                };
                std::map<std::pair<std::size_t, std::size_t>, Expansion> expansions;
                std::map<std::pair<std::size_t, std::size_t>, std::vector<ValueId>> phi_lanes;
                std::map<ValueId, std::vector<ValueId>> lanes;
                // Pre-map vector phis so loop backedge consumers resolve.
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        const auto& op = function.blocks[b].operations[n];
                        if (op.name == "cf.phi" && !op.results.empty() && op.results[0].type.kind == TypeKind::Vector) {
                            std::vector<ValueId> lane_ids;
                            for (std::uint32_t i = 0; i < op.results[0].type.count; ++i) lane_ids.push_back(temp());
                            phi_lanes[{b, n}] = lane_ids;
                            lanes[op.results[0].id] = lane_ids;
                        }
                    }
                auto scalar_arith = [](const std::string& name) {
                    if (name == "vector.add") return std::string("arith.add");
                    if (name == "vector.sub") return std::string("arith.sub");
                    if (name == "vector.mul") return std::string("arith.mul");
                    if (name == "vector.div") return std::string("arith.div");
                    if (name == "vector.mod") return std::string("arith.mod");
                    if (name == "vector.neg") return std::string("arith.neg");
                    detail::fail("vector.operation", "unsupported vector operation " + name);
                };
                std::size_t work = 0;
                bool progress = true;
                while (progress) {
                    progress = false;
                    for (std::size_t b = 0; b < function.blocks.size(); ++b)
                        for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                            if (++work > 500000000) detail::fail("vector.limit", "scalarization work budget exhausted");
                            if (expansions.count({b, n}) || phi_lanes.count({b, n})) continue;
                            const auto& op = function.blocks[b].operations[n];
                            if (op.name.rfind("vector.", 0) != 0) {
                                for (const auto& operand : op.operands)
                                    if (operand.kind == OperandKind::Value && vectors.count(operand.value))
                                        aggregate_escape("vector.escape", "vector value %" + std::to_string(operand.value) +
                                                                         " used outside vector operations",
                                                         op.location);
                                for (const auto& result : op.results)
                                    if (result.type.kind == TypeKind::Vector)
                                        aggregate_escape("vector.escape", "vector value produced outside vector operations", op.location);
                                continue;
                            }
                            Expansion expansion;
                            if (op.name == "vector.splat") {
                                const auto& element = op.results[0].type.elements[0];
                                const auto count = op.results[0].type.count;
                                const auto& input = op.operands[0];
                                if (input.kind == OperandKind::Value) {
                                    for (std::uint32_t i = 0; i < count; ++i) expansion.lanes.push_back(input.value);
                                } else {
                                    for (std::uint32_t i = 0; i < count; ++i) {
                                        const auto id = temp();
                                        Operation materialize;
                                        materialize.location = op.location;
                                        if (input.kind == OperandKind::Integer) {
                                            materialize.name = "arith.constant";
                                            materialize.results = {{id, element, op.location}};
                                            materialize.operands = {input};
                                        } else if (input.kind == OperandKind::Float) {
                                            materialize.name = "arith.constant";
                                            materialize.results = {{id, element, op.location}};
                                            materialize.operands = {input};
                                        } else {
                                            materialize.name = "arith.copy";
                                            materialize.results = {{id, element, op.location}};
                                            materialize.operands = {input};
                                        }
                                        expansion.replacement.push_back(std::move(materialize));
                                        expansion.lanes.push_back(id);
                                    }
                                }
                                lanes[op.results[0].id] = expansion.lanes;
                                expansions[{b, n}] = std::move(expansion);
                                progress = true;
                                continue;
                            }
                            if (op.name == "vector.extract") {
                                if (op.operands[0].kind != OperandKind::Value) continue;
                                auto source = lanes.find(op.operands[0].value);
                                if (source == lanes.end()) continue; // wait for definition
                                const auto lane = static_cast<std::uint32_t>(std::get<std::int64_t>(op.attributes.at("lane")));
                                Operation copy;
                                copy.name = "arith.copy";
                                copy.location = op.location;
                                copy.results = op.results;
                                copy.operands = {Operand::ref(source->second[lane])};
                                expansion.replacement.push_back(std::move(copy));
                                expansions[{b, n}] = std::move(expansion);
                                progress = true;
                                continue;
                            }
                            // Elementwise arithmetic: every operand is a mapped vector value.
                            const auto& element = op.results[0].type.elements[0];
                            const auto count = op.results[0].type.count;
                            bool ready = true;
                            std::vector<std::vector<Operand>> lanes_in;
                            for (const auto& input : op.operands) {
                                if (input.kind != OperandKind::Value) { ready = false; break; }
                                auto found = lanes.find(input.value);
                                if (found == lanes.end()) { ready = false; break; }
                                std::vector<Operand> one;
                                for (auto id : found->second) one.push_back(Operand::ref(id));
                                lanes_in.push_back(std::move(one));
                            }
                            if (!ready) continue; // wait for dependencies
                            const auto scalar = scalar_arith(op.name);
                            for (std::uint32_t i = 0; i < count; ++i) {
                                Operation lane_op;
                                lane_op.name = scalar;
                                lane_op.location = op.location;
                                const auto id = temp();
                                lane_op.results = {{id, element, op.location}};
                                for (auto& lane : lanes_in) lane_op.operands.push_back(lane[i]);
                                expansion.replacement.push_back(std::move(lane_op));
                                expansion.lanes.push_back(id);
                            }
                            lanes[op.results[0].id] = expansion.lanes;
                            expansions[{b, n}] = std::move(expansion);
                            progress = true;
                        }
                }
                // Splice scalar phis now that every incoming edge value is mapped.
                for (const auto& [position, lane_ids] : phi_lanes) {
                    const auto& op = function.blocks[position.first].operations[position.second];
                    const auto& element = op.results[0].type.elements[0];
                    Expansion expansion;
                    expansion.lanes = lane_ids;
                    for (std::uint32_t i = 0; i < lane_ids.size(); ++i) {
                        Operation phi;
                        phi.name = "cf.phi";
                        phi.location = op.location;
                        phi.results = {{lane_ids[i], element, op.location}};
                        phi.incoming = op.incoming;
                        for (const auto& operand : op.operands) {
                            if (operand.kind != OperandKind::Value) aggregate_escape("vector.escape", "vector phi has a non-value incoming", op.location);
                            auto found = lanes.find(operand.value);
                            if (found == lanes.end() || i >= found->second.size())
                                aggregate_escape("vector.escape", "vector phi incoming value is not scalarizable", op.location);
                            phi.operands.push_back(Operand::ref(found->second[i]));
                        }
                        expansion.replacement.push_back(std::move(phi));
                    }
                    expansions[position] = std::move(expansion);
                }
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        const auto& op = function.blocks[b].operations[n];
                        if (op.name.rfind("vector.", 0) == 0 && !expansions.count({b, n}))
                            aggregate_escape("vector.escape", "vector operation '" + op.name + "' could not be scalarized", op.location);
                    }
                for (std::size_t b = 0; b < function.blocks.size(); ++b) {
                    std::vector<Operation> rebuilt;
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        auto found = expansions.find({b, n});
                        if (found == expansions.end()) rebuilt.push_back(function.blocks[b].operations[n]);
                        else rebuilt.insert(rebuilt.end(), found->second.replacement.begin(), found->second.replacement.end());
                    }
                    function.blocks[b].operations = std::move(rebuilt);
                }
                for (const auto& block : function.blocks)
                    for (const auto& op : block.operations)
                        for (const auto& result : op.results)
                            if (result.type.kind == TypeKind::Vector)
                                aggregate_escape("vector.escape", "vector value survived scalarization", op.location);
            }
            return changed;
        }};
}

Pass tuple_sroa_pass() {
    return {"tuple-sroa", {}, {"cfg", "dominators", "loops"},
        [](Module& module, const Registry&, AnalysisManager&) {
            bool changed = false;
            for (auto& function : module.functions) {
                if (function.external) continue;
                bool present = false;
                for (const auto& block : function.blocks)
                    for (const auto& op : block.operations)
                        if (op.name.rfind("tuple.", 0) == 0) present = true;
                if (!present) continue;
                changed = true;
                check_signatures(function, TypeKind::Tuple, "tuple.escape");
                std::map<ValueId, std::vector<Operand>> fields;
                std::map<std::pair<std::size_t, std::size_t>, std::vector<Operation>> replacements;
                std::map<std::pair<std::size_t, std::size_t>, std::vector<ValueId>> phi_fields;
                ValueId fresh = function.fresh_value();
                auto temp = [&] {
                    detail::check(fresh < invalid_id - 1, "id.exhausted", "tuple elimination exhausted value IDs");
                    return fresh++;
                };
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        const auto& op = function.blocks[b].operations[n];
                        if (op.name == "cf.phi" && !op.results.empty() && op.results[0].type.kind == TypeKind::Tuple) {
                            std::vector<ValueId> lanes;
                            for (std::size_t i = 0; i < op.results[0].type.elements.size(); ++i) lanes.push_back(temp());
                            phi_fields[{b, n}] = lanes;
                            std::vector<Operand> aliased;
                            for (auto id : lanes) aliased.push_back(Operand::ref(id));
                            fields[op.results[0].id] = std::move(aliased);
                        }
                    }
                std::size_t work = 0;
                bool progress = true;
                while (progress) {
                    progress = false;
                    for (std::size_t b = 0; b < function.blocks.size(); ++b)
                        for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                            if (++work > 500000000) detail::fail("tuple.limit", "tuple elimination work budget exhausted");
                            if (replacements.count({b, n}) || phi_fields.count({b, n})) continue;
                            const auto& op = function.blocks[b].operations[n];
                            if (op.name == "tuple.build") {
                                // Verified scalar fields alias directly and emit no operations.
                                std::vector<Operand> lanes;
                                for (const auto& operand : op.operands) lanes.push_back(operand);
                                fields[op.results[0].id] = std::move(lanes);
                                replacements[{b, n}] = {};
                                progress = true;
                            } else if (op.name == "tuple.extract") {
                                if (op.operands[0].kind != OperandKind::Value) continue;
                                auto found = fields.find(op.operands[0].value);
                                if (found == fields.end()) continue; // wait for definition
                                const auto index = static_cast<std::size_t>(std::get<std::int64_t>(op.attributes.at("index")));
                                Operation materialize;
                                materialize.location = op.location;
                                materialize.results = op.results;
                                const auto& field = found->second[index];
                                if (field.kind == OperandKind::Value || field.kind == OperandKind::Symbol) {
                                    materialize.name = "arith.copy";
                                    materialize.operands = {field};
                                } else {
                                    materialize.name = "arith.constant";
                                    materialize.operands = {field};
                                }
                                replacements[{b, n}] = {std::move(materialize)};
                                progress = true;
                            } else if (op.name.rfind("tuple.", 0) == 0) {
                                aggregate_escape("tuple.escape", "unknown tuple operation '" + op.name + "'", op.location);
                            } else {
                                for (const auto& operand : op.operands)
                                    if (operand.kind == OperandKind::Value) {
                                        auto found = fields.find(operand.value);
                                        if (found != fields.end())
                                            aggregate_escape("tuple.escape", "tuple value %" + std::to_string(operand.value) +
                                                                             " used outside tuple operations",
                                                             op.location);
                                    }
                                for (const auto& result : op.results)
                                    if (result.type.kind == TypeKind::Tuple && op.name != "cf.phi")
                                        aggregate_escape("tuple.escape", "tuple value produced outside tuple operations", op.location);
                            }
                        }
                }
                for (const auto& [position, lane_ids] : phi_fields) {
                    const auto& op = function.blocks[position.first].operations[position.second];
                    std::vector<Operation> phis;
                    for (std::size_t i = 0; i < lane_ids.size(); ++i) {
                        Operation phi;
                        phi.name = "cf.phi";
                        phi.location = op.location;
                        phi.results = {{lane_ids[i], op.results[0].type.elements[i], op.location}};
                        phi.incoming = op.incoming;
                        for (const auto& operand : op.operands) {
                            if (operand.kind != OperandKind::Value) aggregate_escape("tuple.escape", "tuple phi has a non-value incoming", op.location);
                            auto found = fields.find(operand.value);
                            if (found == fields.end() || i >= found->second.size())
                                aggregate_escape("tuple.escape", "tuple phi incoming value is not eliminable", op.location);
                            phi.operands.push_back(found->second[i]);
                        }
                        phis.push_back(std::move(phi));
                    }
                    replacements[position] = std::move(phis);
                }
                for (std::size_t b = 0; b < function.blocks.size(); ++b)
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        const auto& op = function.blocks[b].operations[n];
                        if ((op.name == "tuple.build" || op.name == "tuple.extract") && !replacements.count({b, n}))
                            aggregate_escape("tuple.escape", "tuple operation '" + op.name + "' could not be eliminated", op.location);
                    }
                for (std::size_t b = 0; b < function.blocks.size(); ++b) {
                    std::vector<Operation> rebuilt;
                    for (std::size_t n = 0; n < function.blocks[b].operations.size(); ++n) {
                        auto found = replacements.find({b, n});
                        if (found == replacements.end()) rebuilt.push_back(function.blocks[b].operations[n]);
                        else rebuilt.insert(rebuilt.end(), found->second.begin(), found->second.end());
                    }
                    function.blocks[b].operations = std::move(rebuilt);
                }
                for (const auto& block : function.blocks)
                    for (const auto& op : block.operations)
                        for (const auto& result : op.results)
                            if (result.type.kind == TypeKind::Tuple)
                                aggregate_escape("tuple.escape", "tuple value survived elimination", op.location);
            }
            return changed;
        }};
}

} // namespace flintstone
