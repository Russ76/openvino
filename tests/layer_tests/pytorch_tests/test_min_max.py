# Copyright (C) 2018-2023 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import pytest

from pytorch_layer_test_class import PytorchLayerTest


class TestMinMax(PytorchLayerTest):
    def _prepare_input(self, input_dtype="float32", second_input=False, second_input_dtype="float32"):
        import numpy as np
        if not second_input:
            return (np.random.randn(1, 3, 10, 10).astype(input_dtype),)
        return (np.random.randn(1, 3, 10, 10).astype(input_dtype), np.random.randn(1, 3, 10, 10).astype(second_input_dtype))

    def create_model(self, op_type, axes, keep_dims, single_input=True, dtypes=("float32", "float32")):
        import torch
        op_types = {
            'max': torch.max,
            'min': torch.min
        }

        dtypes_map = {
            "float32": torch.float32,
            "float64": torch.float64,
            "int32": torch.int32,
            "int64": torch.int64,
            "uint8": torch.uint8
        }

        op = op_types[op_type]

        class aten_min_max(torch.nn.Module):
            def __init__(self, op):
                super(aten_min_max, self).__init__()
                self.op = op

            def forward(self, x):
                return self.op(x)

        class aten_min_max_3args(torch.nn.Module):
            def __init__(self, op, axes=None, keep_dims=None):
                super(aten_min_max_3args, self).__init__()
                self.op = op
                self.axes = axes
                self.keep_dims = keep_dims

            def forward(self, x):
                return self.op(x, self.axes, self.keep_dims)

        class aten_min_max_2args(torch.nn.Module):
            def __init__(self, op, l_dtype, r_dtype):
                super(aten_min_max_2args, self).__init__()
                self.op = op
                self.l_dtype = l_dtype
                self.r_dtype = r_dtype

            def forward(self, x, y):
                return self.op(x.to(self.l_dtype), y.to(self.r_dtype))

        ref_net = None
        if axes is None and keep_dims is None:
            if single_input:
                model_cls = aten_min_max(op)
            else:
                l_dtype = dtypes_map[dtypes[0]]
                r_dtype = dtypes_map[dtypes[1]]
                model_cls = aten_min_max_2args(op, l_dtype, r_dtype)
        else:
            model_cls = aten_min_max_3args(op, axes, keep_dims)

        return model_cls, ref_net, f"aten::{op_type}"

    @pytest.mark.parametrize("axes,keep_dims", [(None, None), (1, False), (1, True), (-1, False), (-1, True)])
    @pytest.mark.parametrize("op_type", ['min', 'max'])
    @pytest.mark.nightly
    @pytest.mark.precommit
    def test_reduce_min_max(self, axes, keep_dims, op_type, ie_device, precision, ir_version):
        self._test(*self.create_model(op_type, axes, keep_dims,
                                      single_input=True), ie_device, precision, ir_version)

    @pytest.mark.parametrize("op_type", ['min', 'max'])
    @pytest.mark.parametrize("second_input_dtype", ["float32", "int32", "float64", "int64", "uint8"])
    @pytest.mark.parametrize("first_input_dtype", ["float32", "int32", "float64", "int64", "uint8"])
    @pytest.mark.nightly
    @pytest.mark.precommit
    def test_min_max(self, op_type, first_input_dtype, second_input_dtype, ie_device, precision, ir_version):
        self._test(*self.create_model(op_type, None, None, single_input=False, dtypes=(first_input_dtype, second_input_dtype)),
                   ie_device, precision, ir_version, kwargs_to_prepare_input=
                   {"second_input": True, "input_dtype": first_input_dtype, "second_input_dtype": second_input_dtype}
                   )


class TestPrimMax(PytorchLayerTest):
    def _prepare_input(self, first_input, second_input, dtype="float"):
        import numpy as np
        first_array = np.array(first_input).astype(dtype)
        if not second_input:
            return (first_array,)
        second_array = np.array(second_input).astype(dtype)
        return (first_array, second_array)

    def create_model(self, case):
        import torch

        class prim_max_2_values(torch.nn.Module):

            def forward(self, x: float, y: float):
                return max(x, y)

        class prim_max_2_list_values(torch.nn.Module):
            def forward(self, x: float, y: float):
                return torch.tensor(max([x, x + y], [y, y - x]))

        class prim_max_1list_several_values(torch.nn.Module):

            def forward(self, x: float, y: float):
                return max([x, y, x + y])

        class prim_max_one_value(torch.nn.Module):
            def forward(self, x: float, y: float):
                return max(x)

        cases = {
            "2_values": prim_max_2_values,
            "2_list_values": prim_max_2_list_values,
            "list_several_values": prim_max_1list_several_values,
            "one_value": prim_max_one_value
        }
        model_cls = cases[case]()

        ref_net = None

        return model_cls, ref_net, "prim::max"

    @pytest.mark.parametrize("case", ["2_values", "2_list_values", "list_several_values", "one_value"])
    @pytest.mark.parametrize("kwargs_to_prepare_input", [
        {"first_input": 0, "second_input": 1, "dtype": "float"},
        {"first_input": 1, "second_input": 1, "dtype": "float"},
        {"first_input": 2, "second_input": 1, "dtype": "float"},
        {"first_input": 0, "second_input": 1, "dtype": "int"},
        {"first_input": 1, "second_input": 1, "dtype": "int"},
        {"first_input": 2, "second_input": 1, "dtype": "int"},
        {"first_input": 0, "second_input": 1, "dtype": "bool"},
        {"first_input": 1, "second_input": 1, "dtype": "bool"},
        {"first_input": 2, "second_input": 1, "dtype": "bool"},
    ])
    @pytest.mark.nightly
    @pytest.mark.precommit
    def test_min_max(self, case, kwargs_to_prepare_input, ie_device, precision, ir_version):
        self._test(*self.create_model(case),
                   ie_device, precision, ir_version, kwargs_to_prepare_input=kwargs_to_prepare_input, use_mo_convert=False)

class TestPrimMin(PytorchLayerTest):
    def _prepare_input(self, first_input, second_input, dtype="float"):
        import numpy as np
        first_array = np.array(first_input).astype(dtype)
        if not second_input:
            return (first_array,)
        second_array = np.array(second_input).astype(dtype)
        return (first_array, second_array)

    def create_model(self, case):
        import torch

        class prim_min_2_values(torch.nn.Module):

            def forward(self, x: float, y: float):
                return min(x, y)

        class prim_min_2_list_values(torch.nn.Module):
            def forward(self, x: float, y: float):
                return torch.tensor(min([x, x + y], [y, y - x]))

        class prim_min_1list_several_values(torch.nn.Module):

            def forward(self, x: float, y: float):
                return min([x, y, x + y])

        class prim_min_one_value(torch.nn.Module):
            def forward(self, x: float, y: float):
                return min(x)

        cases = {
            "2_values": prim_min_2_values,
            "2_list_values": prim_min_2_list_values,
            "list_several_values": prim_min_1list_several_values,
            "one_value": prim_min_one_value
        }
        model_cls = cases[case]()

        ref_net = None

        return model_cls, ref_net, "prim::min"

    @pytest.mark.parametrize("case", ["2_values", "2_list_values", "list_several_values", "one_value"])
    @pytest.mark.parametrize("kwargs_to_prepare_input", [
        {"first_input": 0, "second_input": 1, "dtype": "float"},
        {"first_input": 1, "second_input": 1, "dtype": "float"},
        {"first_input": 2, "second_input": 1, "dtype": "float"},
        {"first_input": 0, "second_input": 1, "dtype": "int"},
        {"first_input": 1, "second_input": 1, "dtype": "int"},
        {"first_input": 2, "second_input": 1, "dtype": "int"},
        {"first_input": 0, "second_input": 1, "dtype": "bool"},
        {"first_input": 1, "second_input": 1, "dtype": "bool"},
        {"first_input": 2, "second_input": 1, "dtype": "bool"},
    ])
    @pytest.mark.nightly
    @pytest.mark.precommit
    def test_min(self, case, kwargs_to_prepare_input, ie_device, precision, ir_version):
        self._test(*self.create_model(case),
                   ie_device, precision, ir_version, kwargs_to_prepare_input=kwargs_to_prepare_input, use_mo_convert=False)
