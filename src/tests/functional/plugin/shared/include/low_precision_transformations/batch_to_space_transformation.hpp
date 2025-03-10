// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <string>
#include <memory>

#include "shared_test_classes/base/low_precision_transformations/layer_transformation.hpp"
#include "lpt_ngraph_functions/common/fake_quantize_on_data.hpp"


namespace LayerTestsDefinitions {
class BatchToSpaceTransformationParam {
public:
    ngraph::PartialShape input_shape;
    std::vector<size_t> block_shape;
    std::vector<size_t> crops_begin;
    std::vector<size_t> crops_end;
    ngraph::builder::subgraph::FakeQuantizeOnData fake_quantize;
    std::string layer_type;
    std::string expected_kernel_type;
};

typedef std::tuple<
    ngraph::element::Type,
    std::string,
    BatchToSpaceTransformationParam
> BatchToSpaceTransformationParams;

class BatchToSpaceTransformation :
    public testing::WithParamInterface<BatchToSpaceTransformationParams>,
    public LayerTestsUtils::LayerTransformation {
public:
    static std::string getTestCaseName(const testing::TestParamInfo<BatchToSpaceTransformationParams>& obj);

protected:
    void SetUp() override;
    void Run() override;
};

}  // namespace LayerTestsDefinitions
