#ifndef CAFFE2_OPERATORS_CONCAT_SPLIT_OP_H_
#define CAFFE2_OPERATORS_CONCAT_SPLIT_OP_H_

#include "caffe2/core/context.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/types.h"
#include "caffe2/utils/math.h"

namespace caffe2 {

namespace {
inline int GetDimFromOrderString(const string& str) {
  auto order = StringToStorageOrder(str);
  switch (order) {
    case StorageOrder::NHWC:
      return 3;
    case StorageOrder::NCHW:
      return 1;
    default:
      CAFFE_THROW("Unsupported storage order: ", str);
      return -1;
  }
}
} // namespace

template <class Context>
class SplitOp final : public Operator<Context> {
 public:
  static const int kSplitOpInputSize = 2;

  USE_OPERATOR_CONTEXT_FUNCTIONS;
  SplitOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        split_(this->template GetRepeatedArgument<int>("split")) {
    CAFFE_ENFORCE(
        !(OperatorBase::HasArgument("axis") &&
          OperatorBase::HasArgument("order")),
        "You shouldn't specify both the dim to split, and the order "
        "in the case of 4-D images.");
    if (OperatorBase::HasArgument("axis")) {
      axis_ = this->template GetSingleArgument<int>("axis", -1);
      // only exists for computing the gradient of a Concat with 'add_axis'
      add_axis_ = this->template GetSingleArgument<int>("add_axis", 0);
    } else {
      axis_ = GetDimFromOrderString(
          this->template GetSingleArgument<string>("order", "NCHW"));
      add_axis_ = 0;
    }
  }

  bool RunOnDevice() override;

 protected:
  int axis_;
  int add_axis_;
  vector<int> split_;
  // Input: X, optionally split
  // The split tensor is stored in CPU.
};

template <class Context>
class SplitByLengthsOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  SplitByLengthsOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {
    CAFFE_ENFORCE(
        !(OperatorBase::HasArgument("axis") &&
          OperatorBase::HasArgument("order")),
        "You shouldn't specify both the dim to split, and the order "
        "in the case of 4-D images.");
    if (OperatorBase::HasArgument("axis")) {
      axis_ = this->template GetSingleArgument<int>("axis", 0);
    } else {
      axis_ = GetDimFromOrderString(
          this->template GetSingleArgument<string>("order", "NCHW"));
    }
  }

  bool RunOnDevice() override;

 protected:
  int axis_;
  Tensor inclusive_scan_buffer_{Context::GetDeviceType()};
  Tensor inclusive_scan_length_buffer_{Context::GetDeviceType()};
  // Input: X, optionally split
  // The split tensor is stored in CPU.
};

template <class Context>
class ConcatOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  ConcatOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {
    CAFFE_ENFORCE(
        !(OperatorBase::HasArgument("axis") &&
          OperatorBase::HasArgument("order")),
        "You shouldn't specify both the dim to concat, and the order "
        "in the case of 4-D images.");
    if (OperatorBase::HasArgument("axis")) {
      axis_ = this->template GetSingleArgument<int>("axis", -1);
      add_axis_ = this->template GetSingleArgument<int>("add_axis", 0);
    } else {
      axis_ = GetDimFromOrderString(
          this->template GetSingleArgument<string>("order", "NCHW"));
      add_axis_ = 0;
    }
  }

  bool RunOnDevice() override;

 protected:
  int axis_;
  int add_axis_;
  // Input: a number of tensors. Output: Y, split
  // The split are stored in CPU.
};

// Implementations
template <class Context>
bool SplitOp<Context>::RunOnDevice() {
  auto& input = Input(0);
  int canonical_axis = input.canonical_axis_index(axis_);
  CAFFE_ENFORCE_LT(
      canonical_axis, input.ndim(), "Axis not in input ndim range.");
  const int input_channels = input.dim32(canonical_axis);
  const int* axis_data;
  vector<int> equal_split;
  if (InputSize() == kSplitOpInputSize) {
    // We obtain split from the input tensor.
    CAFFE_ENFORCE_EQ(
        split_.size(),
        0,
        "If you set split with an input blob, do not pass in "
        "split in the argument.");
    auto& split_tensor = this->template Input<Tensor>(1, CPU);
    CAFFE_ENFORCE_EQ(split_tensor.numel(), OutputSize());
    axis_data = split_tensor.template data<int>();
  } else if (split_.size() == 0) {
    CAFFE_ENFORCE_EQ(
        input_channels % OutputSize(),
        0,
        "If you did not specify split explicitly, the number of "
        "input channels should be divisible by the output size.");
    equal_split.resize(OutputSize(), input_channels / OutputSize());
    axis_data = equal_split.data();
  } else {
    // We obtain split from the parameters.
    CAFFE_ENFORCE_EQ(
        split_.size(),
        OutputSize(),
        "The number of splits specified should be equal to the "
        "number of outputs.");
    axis_data = split_.data();
  }

  CAFFE_ENFORCE_EQ(
      add_axis_ ? OutputSize()
                : std::accumulate(axis_data, axis_data + OutputSize(), 0),
      input_channels,
      "Sum of split dimensions do not match: should be ",
      input_channels);
  vector<int64_t> output_dims(input.sizes().vec());
  int before = 1, after = 1;
  for (int i = 0; i < canonical_axis; ++i) {
    before *= input.dim32(i);
  }
  for (int i = canonical_axis + 1; i < input.ndim(); ++i) {
    after *= input.dim32(i);
  }
  if (add_axis_) {
    output_dims.erase(output_dims.begin() + canonical_axis);
  }
  size_t input_offset = 0;
  for (int i = 0; i < OutputSize(); ++i) {
    auto* output = Output(i);
    auto axis_dim = add_axis_ ? 1 : axis_data[i];
    if (!add_axis_) {
      output_dims[canonical_axis] = axis_data[i];
    }
    output->Resize(output_dims);
    math::CopyMatrix<Context>(
        input.itemsize(),
        before,
        axis_dim * after,
        static_cast<const char*>(input.raw_data()) + input_offset,
        input.dim32(canonical_axis) * after,
        output->raw_mutable_data(input.dtype()),
        axis_dim * after,
        &context_,
        input.dtype().copy());
    input_offset += axis_dim * after * input.itemsize();
  }
  return true;
}

// Implementations
template <class Context>
bool SplitByLengthsOp<Context>::RunOnDevice() {
  auto& input = Input(0);
  auto& length = this->template Input<Tensor>(1, CPU);
  auto length_length = length.numel();
  CAFFE_ENFORCE_EQ(
      length_length % OutputSize(),
      0,
      "len(Lengths) should be divisible by OutputSize().");
  int canonical_axis = input.canonical_axis_index(axis_);
  CAFFE_ENFORCE_LT(
      canonical_axis, input.ndim(), "Axis not in input ndim range.");
  const int input_channels = input.dim32(canonical_axis);
  const auto* axis_data = length.template data<int>();
  CAFFE_ENFORCE_EQ(
      std::accumulate(axis_data, axis_data + length.numel(), 0),
      input_channels,
      "Sum of split dimensions do not match: should be ",
      input_channels);
  vector<int64_t> output_dims(input.sizes().vec());
  int before = input.size_to_dim(canonical_axis);
  int after = input.size_from_dim(canonical_axis + 1);
  size_t input_offset = 0;
  for (int i = 0; i < OutputSize(); ++i) {
    auto* output = Output(i);
    const auto* axis_offset = axis_data + length_length / OutputSize() * i;
    auto axis_dim = std::accumulate(
        axis_offset, axis_offset + length_length / OutputSize(), 0);
    output_dims[canonical_axis] = axis_dim;
    output->Resize(output_dims);
    math::CopyMatrix<Context>(
        input.itemsize(),
        before,
        axis_dim * after,
        static_cast<const char*>(input.raw_data()) + input_offset,
        input.dim32(canonical_axis) * after,
        output->raw_mutable_data(input.dtype()),
        axis_dim * after,
        &context_,
        input.dtype().copy());
    input_offset += axis_dim * after * input.itemsize();
  }
  return true;
}

template <class Context>
bool ConcatOp<Context>::RunOnDevice() {
  auto* output = Output(0);
  Tensor* split = this->template Output<Tensor>(1, CPU);
  split->Resize(vector<int64_t>(1, InputSize()));
  int* axis_data = split->template mutable_data<int>();
  auto& input_zero = Input(0);
  int adj_size = input_zero.ndim() + (add_axis_ ? 1 : 0);
  int canonical_axis = canonical_axis_index_(axis_, adj_size);
  CAFFE_ENFORCE_LT(canonical_axis, adj_size, "Axis not in input ndim range.");
  for (int i = 1; i < InputSize(); ++i) {
    CAFFE_ENFORCE(
        Input(i).dtype() == input_zero.dtype(),
        "All inputs must have the same type, expected: ",
        input_zero.dtype().name(),
        " but got: ",
        Input(i).dtype().name(),
        " for input: ",
        i);
  }

  int before = 1, after = 1;
  vector<int64_t> output_dims(input_zero.sizes().vec());
  for (int i = 0; i < input_zero.ndim(); ++i) {
    if (i == canonical_axis && !add_axis_) {
      continue;
    }
    int dim = input_zero.dim32(i);
    if (i < canonical_axis) {
      before *= dim;
    } else { // i > canonical_axis || i == canonical_axis && add_axis_
      after *= dim;
    }
    // check the input dims are compatible.
    for (int j = 1; j < InputSize(); ++j) {
      int dim_j = Input(j).dim32(i);
      CAFFE_ENFORCE(
          dim == dim_j,
          "Expect dimension = ",
          dim,
          " got ",
          dim_j,
          " at axis = ",
          i,
          " for input: ",
          j,
          ". The input tensors can only have different dimensions "
          "when arg 'add_axis' = 0 and along the axis = ",
          canonical_axis,
          " <",
          Input(0).sizes(),
          "> vs <",
          Input(j).sizes(),
          ">.");
    }
  }

  int output_channels = 0;
  for (int i = 0; i < InputSize(); ++i) {
    axis_data[i] = add_axis_ ? 1 : Input(i).dim32(canonical_axis);
    output_channels += axis_data[i];
  }
  if (add_axis_) {
    output_dims.insert(output_dims.begin() + canonical_axis, output_channels);
  } else {
    output_dims[canonical_axis] = output_channels;
  }
  output->Resize(output_dims);
  size_t output_offset = 0;
  for (int i = 0; i < InputSize(); ++i) {
    auto& input = Input(i);
    auto axis_dim = add_axis_ ? 1 : input.dim32(canonical_axis);
    math::CopyMatrix<Context>(
        input.itemsize(),
        before,
        axis_dim * after,
        input.raw_data(),
        axis_dim * after,
        static_cast<char*>(output->raw_mutable_data(input_zero.dtype())) +
            output_offset,
        output_channels * after,
        &context_,
        input_zero.dtype().copy());
    output_offset += axis_dim * after * input.itemsize();
  }
  return true;
}

} // namespace caffe2

#endif // CAFFE2_OPERATORS_CONCAT_SPLIT_OP_H_
