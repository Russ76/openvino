// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "int_executable.hpp"

#include <cstring>
#include <limits>

#include "evaluates_map.hpp"
#include "openvino/core/except.hpp"
#include "openvino/op/parameter.hpp"
#include "openvino/op/result.hpp"
#include "openvino/op/util/op_types.hpp"
#include "openvino/op/util/variable_context.hpp"
#include "perf_counter.hpp"
#include "shape_util.hpp"

class TemporaryOverrideOutputs {
    std::shared_ptr<ov::Model> model;
    std::unordered_map<std::shared_ptr<ov::descriptor::Tensor>, ov::PartialShape> orig_paramter_shapes_map;

public:
    TemporaryOverrideOutputs(std::shared_ptr<ov::Model>& model,
                             const std::unordered_map<std::shared_ptr<ov::descriptor::Tensor>, ov::Tensor>& tensor_map)
        : model(model) {
        for (const auto& param : model->get_parameters()) {
            auto output_tensor = param->output(0).get_tensor_ptr();
            orig_paramter_shapes_map.insert({output_tensor, param->get_partial_shape()});
            param->set_partial_shape(tensor_map.at(output_tensor).get_shape());
        }
        model->validate_nodes_and_infer_types();
    }

    ~TemporaryOverrideOutputs() {
        for (const auto& param : model->get_parameters()) {
            auto output_tensor = param->output(0).get_tensor_ptr();
            param->set_partial_shape(orig_paramter_shapes_map.at(output_tensor));
        }
        model->validate_nodes_and_infer_types();
    }
};

ov::runtime::interpreter::INTExecutable::INTExecutable(const std::shared_ptr<ov::Model>& model) : m_is_compiled{true} {
    m_model = model->clone();
    for (auto node : m_model->get_ordered_ops()) {
        m_nodes.push_back(node);
    }
    set_parameters_and_results(*m_model);
}

void ov::runtime::interpreter::INTExecutable::cancel() {
    m_cancel_execution = true;
}

bool ov::runtime::interpreter::INTExecutable::call(std::vector<ov::Tensor>& outputs,
                                                   const std::vector<ov::Tensor>& inputs,
                                                   bool collect_performance) {
    EvaluationContext eval_context;
    ov::op::util::VariableContext variable_context;
    eval_context.emplace("VariableContext", variable_context);

    // for each ordered op in the graph
    for (const auto& op : m_nodes) {
        if (auto var_extension = std::dynamic_pointer_cast<ov::op::util::VariableExtension>(op)) {
            auto variable = var_extension->get_variable();
            if (!variable_context.get_variable_value(variable)) {
                auto h_tensor = ov::Tensor(op->get_input_element_type(0), op->get_input_shape(0));
                variable_context.set_variable_value(variable, std::make_shared<ov::op::util::VariableValue>(h_tensor));
            }
        }
    }

    return call(outputs, inputs, eval_context, collect_performance);
}

bool ov::runtime::interpreter::INTExecutable::call(std::vector<ov::Tensor>& outputs,
                                                   const std::vector<ov::Tensor>& inputs,
                                                   const ov::EvaluationContext& context,
                                                   bool collect_performance) {
#define CHECK_TERMINATE()                          \
    if (m_cancel_execution) {                      \
        std::lock_guard<std::mutex> lock(m_mutex); \
        m_cancel_execution = false;                \
        return false;                              \
    }

    CHECK_TERMINATE()
    // map function params -> ov::Tensor
    std::unordered_map<std::shared_ptr<ov::descriptor::Tensor>, ov::Tensor> tensor_map;
    size_t input_count = 0;
    for (const auto& param : get_parameters()) {
        for (size_t i = 0; i < param->get_output_size(); ++i) {
            auto tensor = param->output(i).get_tensor_ptr();
            tensor_map.insert({tensor, inputs[input_count++]});
        }
    }

    std::unordered_map<std::shared_ptr<ov::descriptor::Tensor>, size_t> results_map;
    // map function outputs -> ov::Tensor
    for (size_t output_count = 0; output_count < get_results().size(); ++output_count) {
        auto output = get_results()[output_count]->output(0).get_tensor_ptr();
        if (!results_map.count(output))
            results_map.emplace(output, output_count);
    }

    auto overrider = TemporaryOverrideOutputs(m_model, tensor_map);

    // for each ordered op in the graph
    for (const auto& op : m_nodes) {
        CHECK_TERMINATE()
        if (std::dynamic_pointer_cast<ov::op::v0::Parameter>(op)) {
            continue;
        }
        // get op inputs from map
        std::vector<ov::Tensor> op_inputs;
        for (auto input : op->inputs()) {
            auto tensor = input.get_tensor_ptr();
            op_inputs.push_back(tensor_map.at(tensor));
        }

        // get op outputs from map or create
        std::vector<ov::Tensor> op_outputs;
        for (size_t i = 0; i < op->get_output_size(); ++i) {
            auto tensor = op->output(i).get_tensor_ptr();
            ov::Tensor host_tensor;
            auto it = tensor_map.find(tensor);
            auto output = op->output(i);
            if (op::util::is_output(op) || it == tensor_map.end() || !it->second) {
                OPENVINO_SUPPRESS_DEPRECATED_START
                host_tensor = ov::Tensor(
                    output.get_element_type(),
                    output.get_partial_shape().is_dynamic() ? ov::util::make_dynamic_shape() : output.get_shape());
                OPENVINO_SUPPRESS_DEPRECATED_END
            } else {
                host_tensor = it->second;
            }
            op_outputs.push_back(host_tensor);
        }

        {
            PERF(op, collect_performance);
            // Call evaluate for cloned_node with static shapes
            if (!op->evaluate(op_outputs, op_inputs, context)) {
                // TODO: extend evaluate map for the context
                evaluate_node(op, op_outputs, op_inputs);
            }
        }
        // Update tensors in tensor map
        for (size_t i = 0; i < op->get_output_size(); ++i) {
            auto tensor = op->output(i).get_tensor_ptr();
            tensor_map.insert({tensor, op_outputs[i]});
            if (op::util::is_output(op)) {
                auto& output = outputs[results_map[tensor]];
                if (!output || output.get_shape() != op_outputs[i].get_shape()) {
                    outputs[results_map[tensor]] = op_outputs[i];
                } else {
                    op_outputs[i].copy_to(output);
                }
            }
        }
    }

    return true;
}

std::shared_ptr<ov::op::v0::Parameter> ov::runtime::interpreter::INTExecutable::get_parameter(size_t index) const {
    const ParameterVector& parameters = get_parameters();
    OPENVINO_ASSERT(index < parameters.size(), "create_tensor for input out of bounds");
    return parameters[index];
}

std::shared_ptr<ov::op::v0::Result> ov::runtime::interpreter::INTExecutable::get_result(size_t index) const {
    const ResultVector& results = get_results();
    OPENVINO_ASSERT(index < results.size(), "create_tensor for input out of bounds");
    return results[index];
}
ov::Tensor ov::runtime::interpreter::INTExecutable::create_input_tensor(size_t input_index) {
    std::shared_ptr<op::v0::Parameter> parameter = get_parameter(input_index);
    return ov::Tensor(parameter->get_element_type(), parameter->get_shape());
}

ov::Tensor ov::runtime::interpreter::INTExecutable::create_output_tensor(size_t output_index) {
    std::shared_ptr<op::v0::Result> result = get_result(output_index);
    return ov::Tensor(result->get_element_type(), result->get_shape());
}

std::vector<ov::Tensor> ov::runtime::interpreter::INTExecutable::create_input_tensor(size_t input_index,
                                                                                     size_t pipeline_depth) {
    std::vector<ov::Tensor> tensors;
    std::shared_ptr<op::v0::Parameter> parameter = get_parameter(input_index);
    for (size_t i = 0; i < pipeline_depth; i++) {
        ov::Tensor tensor;
        auto t = ov::Tensor(parameter->get_element_type(), parameter->get_shape());
        tensors.push_back(t);
    }
    return tensors;
}

std::vector<ov::Tensor> ov::runtime::interpreter::INTExecutable::create_output_tensor(size_t output_index,
                                                                                      size_t pipeline_depth) {
    std::vector<ov::Tensor> tensors;
    std::shared_ptr<op::v0::Result> result = get_result(output_index);
    for (size_t i = 0; i < pipeline_depth; i++) {
        ov::Tensor tensor;
        auto t = ov::Tensor(result->get_element_type(), result->get_shape());
        tensors.push_back(t);
    }
    return tensors;
}

bool ov::runtime::interpreter::INTExecutable::evaluate_node(const std::shared_ptr<Node>& node,
                                                            ov::TensorVector& outputs,
                                                            const ov::TensorVector& inputs) const {
    auto& map = ov::runtime::interpreter::get_evaluators_map();
    auto it = map.find(node->get_type_info());
    bool res = false;
    OPENVINO_ASSERT(it != map.end(),
                    "Interpreter backend doesn't implement evaluate method for OP ",
                    node->get_type_info().name);
    res = it->second(node, outputs, inputs);
    OPENVINO_ASSERT(res, "Running evaluate method for OP ", node->get_type_info().name, " failed!");
    return res;
}

std::shared_ptr<ov::Model> ov::runtime::interpreter::INTExecutable::get_model() const {
    return m_model;
}
