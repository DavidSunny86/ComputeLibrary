/*
 * Copyright (c) 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/CL/functions/CLConvolutionLayer.h"

#include "arm_compute/core/PixelValue.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/CL/CLScheduler.h"

#include <cmath>
#include <tuple>

using namespace arm_compute;

CLConvolutionLayerReshapeWeights::CLConvolutionLayerReshapeWeights()
    : _weights_reshape_kernel(), _weights_transposed_kernel(), _weights_reshaped(), _transpose1xW(false)
{
}

void CLConvolutionLayerReshapeWeights::configure(const ICLTensor *weights, const ICLTensor *biases, ICLTensor *output, bool transpose1xW)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(weights, 1, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(weights, 1, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(output, 1, DataType::F32);
    ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(weights, biases, output);
    ARM_COMPUTE_ERROR_ON_MISMATCHING_FIXED_POINT(weights, biases, output);
    ARM_COMPUTE_ERROR_ON(weights->info()->num_dimensions() > 4);

    if(biases != nullptr)
    {
        ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(biases, 1, DataType::F32);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(weights, biases);
        ARM_COMPUTE_ERROR_ON(biases->info()->dimension(0) != weights->info()->dimension(3));
        ARM_COMPUTE_ERROR_ON(biases->info()->num_dimensions() > 1);
    }

    const bool _has_bias = (biases != nullptr);

    _transpose1xW = transpose1xW;

    if(transpose1xW)
    {
        // Create tensor to store the reshaped weights
        const unsigned int mat_weights_cols = weights->info()->dimension(3);
        const unsigned int mat_weights_rows = weights->info()->dimension(0) * weights->info()->dimension(1) * weights->info()->dimension(2) + (_has_bias ? 1 : 0);
        TensorShape        shape_wr(mat_weights_cols, mat_weights_rows);
        const DataType     dt = weights->info()->data_type();
        TensorInfo         info_wr(shape_wr, 1, dt);

        _weights_reshaped.allocator()->init(info_wr);
        _weights_reshape_kernel.configure(weights, biases, &_weights_reshaped);
        _weights_transposed_kernel.configure(&_weights_reshaped, output);
        _weights_reshaped.allocator()->allocate();
    }
    else
    {
        _weights_reshape_kernel.configure(weights, biases, output);
    }
}

void CLConvolutionLayerReshapeWeights::run()
{
    cl::CommandQueue q = CLScheduler::get().queue();
    CLScheduler::get().enqueue(_weights_reshape_kernel);
    if(_transpose1xW)
    {
        CLScheduler::get().enqueue(_weights_transposed_kernel);
    }
}

CLConvolutionLayer::CLConvolutionLayer()
    : _reshape_weights(), _input_im2col_kernel(), _input_interleave_kernel(), _mm_kernel(), _output_col2im_kernel(), _input_im2col_reshaped(), _input_interleaved_reshaped(), _weights_reshaped(),
      _weights_transposed(), _gemm_output(), _has_bias(false), _is_fully_connected_convolution(false), _are_weights_reshaped(false)
{
}

void CLConvolutionLayer::configure(const ICLTensor *input, const ICLTensor *weights, const ICLTensor *biases, ICLTensor *output, const PadStrideInfo &conv_info, const WeightsInfo &weights_info)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(weights, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(output, 1, DataType::F16, DataType::F32);
    ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, weights, output);
    ARM_COMPUTE_ERROR_ON(!weights_info.are_reshaped() && weights->info()->dimension(2) != input->info()->dimension(2));
    ARM_COMPUTE_ERROR_ON(weights->info()->num_dimensions() > 4);

    if(biases != nullptr)
    {
        ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(biases, 1, DataType::F16, DataType::F32);
        ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(input, biases);
        ARM_COMPUTE_ERROR_ON(!weights_info.are_reshaped() && biases->info()->dimension(0) != weights->info()->dimension(3));
        ARM_COMPUTE_ERROR_ON(biases->info()->num_dimensions() > 1);
    }

    _has_bias             = (biases != nullptr);
    _are_weights_reshaped = weights_info.are_reshaped();

    // Get parameters for conv_info
    unsigned int stride_x = 0;
    unsigned int stride_y = 0;
    unsigned int pad_x    = 0;
    unsigned int pad_y    = 0;
    std::tie(stride_x, stride_y) = conv_info.stride();
    std::tie(pad_x, pad_y)       = conv_info.pad();

    // Get convolved dimensions
    unsigned int conv_w = 0;
    unsigned int conv_h = 0;

    const unsigned int kernel_width = _are_weights_reshaped ? weights_info.kernel_size() : weights->info()->dimension(0);
    std::tie(conv_w, conv_h) = scaled_dimensions(input->info()->dimension(0), input->info()->dimension(1), kernel_width,
                                                 stride_x, stride_y, pad_x, pad_y, conv_info.round());
    ARM_COMPUTE_ERROR_ON_MSG((output->info()->dimension(0) != conv_w) || (output->info()->dimension(1) != conv_h), "Output shape does not match the expected one");

    // Check if its a "fully connected" convolution
    _is_fully_connected_convolution = ((conv_w == 1) && (conv_h == 1));

    // Create tensor to store the reshaped weights
    size_t mat_weights_cols = weights->info()->dimension(3);
    size_t mat_weights_rows = weights->info()->dimension(0) * weights->info()->dimension(1) * weights->info()->dimension(2) + ((_has_bias) ? 1 : 0);
    if(_are_weights_reshaped)
    {
        mat_weights_cols                         = output->info()->dimension(2);
        const unsigned int quarter_reshaped_cols = weights->info()->dimension(0) / 4;
        mat_weights_rows                         = (_has_bias ? 1 + quarter_reshaped_cols : quarter_reshaped_cols);
    }
    else
    {
        if(_is_fully_connected_convolution)
        {
            // Create tensor to store the reshaped weights
            TensorShape shape_wr(mat_weights_cols, mat_weights_rows);
            TensorInfo  info_wr(shape_wr, 1, weights->info()->data_type());
            _weights_reshaped.allocator()->init(info_wr);
            _reshape_weights.configure(weights, biases, &_weights_reshaped, false);
            weights = &_weights_reshaped;
        }
        else
        {
            // Create tensor to store transposed weights
            TensorShape shape_wt(mat_weights_rows * 4, static_cast<size_t>(std::ceil(mat_weights_cols / 4.f)));
            TensorInfo  info_wt(shape_wt, 1, weights->info()->data_type());
            _weights_transposed.allocator()->init(info_wt);
            _reshape_weights.configure(weights, biases, &_weights_transposed, true);
            weights = &_weights_transposed;
        }
    }
    // Create tensor to store im2col reshaped inputs
    const size_t mat_input_cols = mat_weights_rows;
    const size_t mat_input_rows = conv_w * conv_h;
    TensorShape  shape_im2col   = input->info()->tensor_shape();
    shape_im2col.set(0, mat_input_cols);
    shape_im2col.set(1, mat_input_rows);
    shape_im2col.set(2, 1);
    _input_im2col_reshaped.allocator()->init(TensorInfo(shape_im2col, 1, input->info()->data_type()));

    // Create tensor (interleave) to prepare input tensor for GEMM
    if(!_is_fully_connected_convolution)
    {
        TensorShape shape_interleaved = shape_im2col;
        shape_interleaved.set(0, shape_interleaved.x() * 4);
        shape_interleaved.set(1, std::ceil(static_cast<float>(shape_interleaved.y()) / 4.f));
        _input_interleaved_reshaped.allocator()->init(TensorInfo(shape_interleaved, 1, input->info()->data_type()));
    }

    // Create GEMM output tensor
    TensorShape shape_gemm = _input_im2col_reshaped.info()->tensor_shape();
    shape_gemm.set(0, mat_weights_cols);
    shape_gemm.set(1, mat_input_rows);
    _gemm_output.allocator()->init(TensorInfo(shape_gemm, 1, input->info()->data_type()));

    // Configure kernels
    _input_im2col_kernel.configure(input, &_input_im2col_reshaped, std::make_pair(conv_w, conv_h), conv_info, _has_bias);
    _output_col2im_kernel.configure(&_gemm_output, output, std::make_pair(conv_w, conv_h));

    if(_is_fully_connected_convolution)
    {
        _mm_kernel.configure(&_input_im2col_reshaped, weights, &_gemm_output, 1.0f);
    }
    else
    {
        _input_interleave_kernel.configure(&_input_im2col_reshaped, &_input_interleaved_reshaped);
        _mm_kernel.configure(&_input_interleaved_reshaped, weights, &_gemm_output, 1.0f);
    }

    if(!_are_weights_reshaped)
    {
        if(!_is_fully_connected_convolution)
        {
            _weights_transposed.allocator()->allocate();
        }
        else
        {
            _weights_reshaped.allocator()->allocate();
        }
    }

    _input_im2col_reshaped.allocator()->allocate();
    if(!_is_fully_connected_convolution)
    {
        _input_interleaved_reshaped.allocator()->allocate();
    }
    _gemm_output.allocator()->allocate();
}

void CLConvolutionLayer::run()
{
    // Run weights reshaping (Runs once for every configure)
    if(!_are_weights_reshaped)
    {
        _are_weights_reshaped = true;
        _reshape_weights.run();
    }

    // Run input reshaping
    CLScheduler::get().enqueue(_input_im2col_kernel);
    if(!_is_fully_connected_convolution)
    {
        CLScheduler::get().enqueue(_input_interleave_kernel);
    }

    // Runs matrix multiply on reshaped matrices
    CLScheduler::get().enqueue(_mm_kernel);

    // Reshape output matrix
    CLScheduler::get().enqueue(_output_col2im_kernel, false);
}
