// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/simplify_shape_of_sub_graph.hpp"

#include <memory>
#include <numeric>
#include <vector>

#include "itt.hpp"
#include "openvino/core/rt_info.hpp"
#include "openvino/core/validation_util.hpp"
#include "openvino/op/concat.hpp"
#include "openvino/op/constant.hpp"
#include "openvino/op/fake_quantize.hpp"
#include "openvino/op/gather.hpp"
#include "openvino/op/reshape.hpp"
#include "openvino/op/shape_of.hpp"
#include "openvino/op/util/op_types.hpp"
#include "openvino/pass/manager.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"
#include "transformations/common_optimizations/eliminate_unsqueeze_gather.hpp"
#include "transformations/common_optimizations/nop_elimination.hpp"
#include "transformations/common_optimizations/shared_ops_optimization.hpp"
#include "transformations/utils/utils.hpp"

using namespace ov;
using namespace ov::op;
using namespace ov::pass::pattern;

pass::GroupedGatherElimination::GroupedGatherElimination() {
    MATCHER_SCOPE(GroupedGatherElimination);
    auto concat_label = wrap_type<v0::Concat>(rank_equals(1));

    matcher_pass_callback callback = [=](Matcher& m) {
        auto concat = m.get_match_root();
        OutputVector inputs = concat->input_values();
        NodeVector new_ops;
        size_t i = 0, original_inputs_size = inputs.size();
        while (inputs.size() > i + 1) {
            auto curr = inputs[i].get_node_shared_ptr(), next = inputs[i + 1].get_node_shared_ptr();
            if (curr->get_type_info() != next->get_type_info() ||
                (!is_type<v1::Gather>(curr) && !is_type<v7::Gather>(curr) && !is_type<v8::Gather>(curr)) ||
                (curr->input_value(0) != next->input_value(0))) {
                ++i;
                continue;
            }

            // Scalar inputs are not supported by Concat and we don't want to throw an exception here.
            // The transformation should not be applied instead.
            if (curr->input_value(1).get_partial_shape().same_scheme(Shape{}) ||
                next->input_value(1).get_partial_shape().same_scheme(Shape{})) {
                return false;
            }

            // curr and next are the same type of gather which takes data from the same source
            auto joint_indices =
                op::util::make_try_fold<v0::Concat>(OutputVector{curr->input_value(1), next->input_value(1)}, 0);
            std::shared_ptr<Node> new_gather;
            if (is_type<v1::Gather>(curr)) {
                new_gather = register_new_node<v1::Gather>(curr->input_value(0),
                                                           joint_indices->output(0),
                                                           v0::Constant::create(element::i64, {}, {0})->output(0));
            } else if (is_type<v7::Gather>(curr)) {
                new_gather = register_new_node<v7::Gather>(curr->input_value(0),
                                                           joint_indices->output(0),
                                                           v0::Constant::create(element::i64, {}, {0})->output(0));
            } else if (is_type<v8::Gather>(curr)) {
                new_gather = register_new_node<v8::Gather>(curr->input_value(0),
                                                           joint_indices->output(0),
                                                           v0::Constant::create(element::i64, {}, {0})->output(0));
            } else {
                OPENVINO_THROW("Unexpected Gather version");
            }
            new_ops.push_back(joint_indices);
            new_ops.push_back(new_gather);
            inputs.erase(inputs.begin() + i);
            inputs[i] = new_gather->output(0);
        }
        ov::copy_runtime_info(concat, new_ops);
        if (inputs.size() == 1)  // we can optimize out concat
            return replace_output_update_name(concat->output(0), inputs[0]);
        if (original_inputs_size > inputs.size()) {
            auto new_concat = std::make_shared<v0::Concat>(inputs, 0);
            new_concat->set_friendly_name(concat->get_friendly_name());
            ov::copy_runtime_info(concat, new_concat);
            ov::replace_node(concat, new_concat);
            return true;
        }
        return false;
    };

    auto m = std::make_shared<Matcher>(concat_label, matcher_name);
    this->register_matcher(m, callback);
}

pass::GatherNopElimination::GatherNopElimination() {
    MATCHER_SCOPE(GatherNopElimination);
    const auto gather_label = wrap_type<op::util::GatherBase>(
        {any_input(has_static_shape()), wrap_type<v0::Constant>(), wrap_type<v0::Constant>()});

    matcher_pass_callback callback = [](Matcher& m) {
        auto gather = m.get_match_root();
        const auto& number_of_indices = shape_size(gather->get_input_shape(1));
        if (gather->get_input_shape(0) != gather->get_output_shape(0) || shape_size(gather->get_input_shape(2)) != 1 ||
            number_of_indices > 10)
            return false;
        std::vector<int64_t> expected_vector(number_of_indices);
        std::iota(expected_vector.begin(), expected_vector.end(), 0);
        OPENVINO_SUPPRESS_DEPRECATED_START
        if (const auto& indices = get_constant_from_source(gather->input_value(1))) {
            OPENVINO_SUPPRESS_DEPRECATED_END
            const auto& indices_values = indices->cast_vector<int64_t>();
            if (indices_values != expected_vector)
                return false;
        }
        return replace_output_update_name(gather->output(0), gather->input_value(0));
    };
    auto m = std::make_shared<Matcher>(gather_label, matcher_name);
    this->register_matcher(m, callback);
}

pass::SimplifyGatherShapeOf::SimplifyGatherShapeOf() {
    MATCHER_SCOPE(SimplifyGatherShapeOf);
    const auto gather_pattern = wrap_type<op::util::GatherBase>();
    const auto shape_of_pattern = wrap_type<v0::ShapeOf, v3::ShapeOf>({gather_pattern});

    matcher_pass_callback callback = [](Matcher& m) {
        auto node = m.get_match_root();
        auto gather = as_type_ptr<v1::Gather>(node->input_value(0).get_node_shared_ptr());
        if (!gather) {
            return false;
        }
        auto gather_in_rank = gather->get_input_partial_shape(0).rank();
        auto indices_rank = gather->get_input_partial_shape(1).rank();
        auto axis = gather->get_axis();
        if (gather_in_rank.is_dynamic() || indices_rank.is_dynamic() || axis == v1::Gather::AXIS_NOT_SET_VALUE) {
            return false;
        }

        auto zero_axis = v0::Constant::create<int64_t>(element::i64, Shape{}, {0});
        NodeVector new_ops;
        auto new_shapeof = std::make_shared<v3::ShapeOf>(gather->input_value(0), node->get_output_element_type(0));
        new_ops.push_back(new_shapeof);
        std::shared_ptr<Node> replace_op;
        if (indices_rank.get_length() == 0) {
            std::vector<int64_t> vi(gather_in_rank.get_length());
            std::iota(vi.begin(), vi.end(), 0);
            vi.erase(vi.begin() + axis);
            auto new_indices = v0::Constant::create<int64_t>(element::i64, Shape{vi.size()}, vi);
            replace_op = std::make_shared<v1::Gather>(new_shapeof, new_indices, zero_axis);
            new_ops.push_back(replace_op);
        } else {
            NodeVector concat_inputs;
            if (axis > 0) {
                std::vector<int64_t> vi(axis);
                std::iota(vi.begin(), vi.end(), 0);
                auto indices = v0::Constant::create<int64_t>(element::i64, Shape{vi.size()}, vi);
                auto new_gather = std::make_shared<v1::Gather>(new_shapeof, indices, zero_axis);
                new_ops.push_back(new_gather);
                concat_inputs.push_back(new_gather);
            }
            auto shapeof_indices =
                std::make_shared<v3::ShapeOf>(gather->input_value(1), node->get_output_element_type(0));
            new_ops.push_back(shapeof_indices);

            concat_inputs.push_back(shapeof_indices);

            if (gather_in_rank.get_length() - 1 > axis) {
                std::vector<int64_t> vi(gather_in_rank.get_length() - (axis + 1));
                std::iota(vi.begin(), vi.end(), axis + 1);
                auto indices = v0::Constant::create<int64_t>(element::i64, Shape{vi.size()}, vi);
                auto new_gather = std::make_shared<v1::Gather>(new_shapeof, indices, zero_axis);
                new_ops.push_back(new_gather);
                concat_inputs.push_back(new_gather);
            }
            replace_op = std::make_shared<v0::Concat>(concat_inputs, 0);
            new_ops.push_back(replace_op);
        }
        replace_op->set_friendly_name(node->get_friendly_name());
        copy_runtime_info(node, new_ops);
        replace_node(node, replace_op);
        return true;
    };

    auto m = std::make_shared<Matcher>(shape_of_pattern, matcher_name);
    this->register_matcher(m, callback);
}

pass::SimplifySecondInputOfReshape::SimplifySecondInputOfReshape() {
    MATCHER_SCOPE(SimplifySecondInputOfReshape);
    const auto input = any_input();
    auto has_static_1d_shape = [](const Output<Node>& output) {
        return has_static_shape()(output) && rank_equals(1)(output);
    };
    const auto concat = wrap_type<v0::Concat>(has_static_1d_shape);
    const auto reshape_pattern = wrap_type<v1::Reshape>({input, concat});

    matcher_pass_callback callback = [=](Matcher& m) {
        auto node = m.get_match_root();
        const auto reshape = as_type_ptr<v1::Reshape>(node);
        if (!reshape || reshape->get_special_zero() == false) {
            return false;
        }

        const auto concat = as_type_ptr<v0::Concat>(reshape->get_input_node_shared_ptr(1));
        if (!concat)
            return false;

        const auto concat_axis = concat->get_axis();
        OPENVINO_ASSERT(concat_axis == 0 || concat_axis == -1, "axis is not valid for matched Concat with 1D output");

        auto data = m.get_pattern_value_map().at(input);
        if (is_type<v0::FakeQuantize>(data.get_node_shared_ptr()) ||
            op::util::is_unary_elementwise_arithmetic(data.get_node_shared_ptr())) {
            data = data.get_node_shared_ptr()->input_value(0);
        }

        auto check_shape_of_gather = [&](const std::shared_ptr<Node>& gather) {
            auto shape_of = gather->get_input_node_shared_ptr(0);
            if (!is_type<v3::ShapeOf>(shape_of) && !is_type<v0::ShapeOf>(shape_of)) {
                return false;
            }
            return shape_of->input_value(0) == data;
        };

        const auto concat_inputs = concat->input_values();
        OutputVector new_concat_inputs = concat_inputs;
        std::int64_t gather_dims_expected_location = 0;
        bool gather_folded = false;

        auto update_expected_gather_location = [&](const Output<Node>& concat_input) {
            const auto concat_input_shape = concat_input.get_shape();
            OPENVINO_ASSERT(concat_input_shape.size() == 1,
                            "concat input rank is not valid for matched Concat with 1D output");
            gather_dims_expected_location += concat_input_shape[0];
        };

        // We need this check to avoid sequences shapeOf -> gather -> concat
        // that change the arrangement of dimensions in the reshape pattern
        for (auto& concat_input : new_concat_inputs) {
            if (const auto gather = as_type_ptr<op::util::GatherBase>(concat_input.get_node_shared_ptr())) {
                auto indices_constant = as_type_ptr<v0::Constant>(gather->get_input_node_shared_ptr(1));
                if (!indices_constant || !check_shape_of_gather(gather)) {
                    update_expected_gather_location(gather);
                    continue;
                }

                bool gather_can_be_fused = true;
                const auto indices = indices_constant->cast_vector<std::int64_t>();
                for (size_t i = 0; i < indices.size(); ++i) {
                    if (indices[i] != gather_dims_expected_location) {
                        gather_can_be_fused = false;
                    }
                    gather_dims_expected_location++;
                }

                if (gather_can_be_fused) {
                    const size_t num_of_unchanged_dimensions = indices.size();
                    const auto subgraph_et = gather->get_input_element_type(0);
                    concat_input = v0::Constant::create(subgraph_et, Shape{num_of_unchanged_dimensions}, {0});
                    gather_folded = true;
                }
            } else {
                update_expected_gather_location(concat_input);
            }
        }

        if (!gather_folded) {
            return false;
        }

        const auto new_concat = op::util::make_try_fold<v0::Concat>(new_concat_inputs, concat_axis);
        new_concat->set_friendly_name(concat->get_friendly_name());
        copy_runtime_info(concat, new_concat);

        const auto new_reshape = reshape->clone_with_new_inputs({reshape->input_value(0), new_concat});
        new_reshape->set_friendly_name(reshape->get_friendly_name());

        copy_runtime_info(reshape, new_reshape);
        replace_node(reshape, new_reshape);
        return true;
    };

    auto m = std::make_shared<Matcher>(reshape_pattern, matcher_name);
    this->register_matcher(m, callback);
}

bool pass::SimplifyShapeOfSubGraph::run_on_model(const std::shared_ptr<Model>& f) {
    RUN_ON_FUNCTION_SCOPE(SimplifyShapeOfSubGraph);
    Manager manager;
    manager.set_per_pass_validation(false);

    REGISTER_PASS(manager, PrepareShapeOpsForEliminationAroundBE)
    REGISTER_PASS(manager, SharedOpOptimization)
    REGISTER_PASS(manager, EliminateGatherUnsqueeze)  // should run after SharedOpOptimization
    REGISTER_PASS(manager, NopElimination, m_use_shapes)
    REGISTER_PASS(manager, GroupedGatherElimination)
    // GatherNopElimination depends on shape, so it requires shape propagation
    // if previous transformations has resolved some dynamic shapes.
    REGISTER_PASS(manager, Validate)
    REGISTER_PASS(manager, GatherNopElimination)
    REGISTER_PASS(manager, SimplifyGatherShapeOf)
    REGISTER_PASS(manager, SimplifySecondInputOfReshape)

    // TODO: potentially this Validate is not needed but it requires additional validation
    REGISTER_PASS(manager, Validate)

    manager.run_passes(f);
    return false;
}
