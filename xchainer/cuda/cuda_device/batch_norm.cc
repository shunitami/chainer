#include "xchainer/cuda/cuda_device.h"

#include <cassert>
#include <cstdint>
#include <memory>

#include <cudnn.h>

#include "xchainer/array.h"
#include "xchainer/axes.h"
#include "xchainer/backend_util.h"
#include "xchainer/cuda/cudnn.h"
#include "xchainer/device.h"
#include "xchainer/dtype.h"
#include "xchainer/error.h"
#include "xchainer/routines/creation.h"
#include "xchainer/scalar.h"
#include "xchainer/shape.h"

namespace xchainer {
namespace cuda {
namespace {

class CudnnBNTensorDescriptor {
public:
    CudnnBNTensorDescriptor(const internal::CudnnTensorDescriptor& x_desc, cudnnBatchNormMode_t mode) : CudnnBNTensorDescriptor{} {
        CheckCudnnError(cudnnDeriveBNTensorDescriptor(desc_, *x_desc, mode));
    }

    ~CudnnBNTensorDescriptor() {
        if (desc_ != nullptr) {
            CheckCudnnError(cudnnDestroyTensorDescriptor(desc_));
        }
    }

    cudnnTensorDescriptor_t descriptor() const { return desc_; }
    cudnnTensorDescriptor_t operator*() const { return desc_; }

    Dtype GetDtype() {
        cudnnDataType_t cudnn_dtype{};
        int ndim{};

        CheckCudnnError(cudnnGetTensorNdDescriptor(desc_, 0, &cudnn_dtype, &ndim, nullptr, nullptr));

        switch (cudnn_dtype) {
            case CUDNN_DATA_DOUBLE:
                return Dtype::kFloat64;
            case CUDNN_DATA_FLOAT:
                return Dtype::kFloat32;
            // TODO(sonots): Support float16
            // case CUDNN_DATA_HALF;
            //     return Dtype::kFloat16;
            default:
                throw DtypeError{"Unsupported cudnn data type: ", cudnn_dtype};
        }
    }

private:
    CudnnBNTensorDescriptor() { CheckCudnnError(cudnnCreateTensorDescriptor(&desc_)); }
    cudnnTensorDescriptor_t desc_{};
};

class CudaBatchNormForwardBackward : public xchainer::BatchNormForwardBackward {
public:
    explicit CudaBatchNormForwardBackward(cudnnHandle_t cudnn_handle) : cudnn_handle_{cudnn_handle} {}

    Array Forward(
            const Array& x,
            const Array& gamma,
            const Array& beta,
            const Array& running_mean,
            const Array& running_var,
            Scalar eps,
            Scalar decay,
            const Axes& axis) override {
        if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
            throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
        }

#ifndef NDEBUG
        {
            Shape reduced_shape = xchainer::internal::ReduceShape(x.shape(), axis, true);
            assert(gamma.shape() == reduced_shape);
            assert(beta.shape() == reduced_shape);

            int64_t reduced_total_size = reduced_shape.GetTotalSize();
            assert(running_mean.GetTotalSize() == reduced_total_size);
            assert(running_var.GetTotalSize() == reduced_total_size);

            assert(&x.device() == &gamma.device());
            assert(&x.device() == &beta.device());
            assert(&x.device() == &running_mean.device());
            assert(&x.device() == &running_var.device());

            assert(x.dtype() == gamma.dtype());
            assert(x.dtype() == beta.dtype());
            assert(x.dtype() == running_mean.dtype());
            assert(x.dtype() == running_var.dtype());

            assert(gamma.IsContiguous());
            assert(beta.IsContiguous());
            assert(running_mean.IsContiguous());
            assert(running_var.IsContiguous());
        }
#endif  // NDEBUG

        if (!running_mean.IsContiguous()) {
            throw DeviceError{"Running mean must to be contiguous for cuDNN to update it in-place."};
        }
        if (!running_var.IsContiguous()) {
            throw DeviceError{"Running variance must to be contiguous for cuDNN to update it in-place."};
        }

        Device& device = x.device();
        Dtype dtype = x.dtype();

        Array x_cont = AsContiguousArray(x);
        internal::CudnnTensorDescriptor x_desc{x_cont};
        cudnnBatchNormMode_t mode = GetBatchNormMode(axis);

        CudnnBNTensorDescriptor gamma_beta_mean_var_desc{x_desc, mode};
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();

        Array gamma_casted = gamma.AsType(gamma_beta_mean_var_dtype, false);
        Array beta_casted = beta.AsType(gamma_beta_mean_var_dtype, false);
        Array running_mean_casted = running_mean.AsType(gamma_beta_mean_var_dtype, false);
        Array running_var_casted = running_var.AsType(gamma_beta_mean_var_dtype, false);

        Array out = EmptyLike(x, device);

        // Initialize cache.
        result_mean_ = EmptyLike(gamma_casted, device);
        result_inv_var_ = EmptyLike(gamma_casted, device);

        CheckCudnnError(cudnnBatchNormalizationForwardTraining(
                cudnn_handle_,
                mode,
                internal::GetValuePtr<1>(dtype),
                internal::GetValuePtr<0>(dtype),
                *x_desc,
                xchainer::internal::GetRawOffsetData<void>(x_cont),
                *x_desc,
                xchainer::internal::GetRawOffsetData<void>(out),
                *gamma_beta_mean_var_desc,
                xchainer::internal::GetRawOffsetData<void>(gamma_casted),
                xchainer::internal::GetRawOffsetData<void>(beta_casted),
                1.0 - static_cast<double>(decay),
                xchainer::internal::GetRawOffsetData<void>(running_mean_casted),
                xchainer::internal::GetRawOffsetData<void>(running_var_casted),
                static_cast<double>(eps),
                xchainer::internal::GetRawOffsetData<void>(result_mean_),
                xchainer::internal::GetRawOffsetData<void>(result_inv_var_)));

        // When data type of prameters is converted, say, from fp16
        // to fp32, the values of fp32 arrays of running_mean and
        // running_var updated by batchNormalizationForwardTraining
        // must be explicitly written back to their original fp16 arrays.
        //
        // TODO(sonots): write tests after we supports fp16
        if (dtype != gamma_beta_mean_var_dtype) {
            assert(running_mean.IsContiguous());
            assert(running_mean_casted.IsContiguous());
            assert(running_var.IsContiguous());
            assert(running_var_casted.IsContiguous());
            device.MemoryCopyFrom(
                    xchainer::internal::GetRawOffsetData<void>(running_mean),
                    xchainer::internal::GetRawOffsetData<void>(running_mean_casted.AsType(dtype, false)),
                    running_mean.GetNBytes(),
                    device);
            device.MemoryCopyFrom(
                    xchainer::internal::GetRawOffsetData<void>(running_var),
                    xchainer::internal::GetRawOffsetData<void>(running_var_casted.AsType(dtype, false)),
                    running_var.GetNBytes(),
                    device);
        }

        return out;
    }

    // TODO(hvy): Implement me.
    std::array<Array, 3> Backward(
            const Array& /*x*/, const Array& /*gamma*/, const Array& /*gout*/, Scalar /*eps*/, const Axes& /*axis*/) override {
        return {Array{}, Array{}, Array{}};
    }

    // TODO(niboshi): Implement me.
    std::array<Array, 3> DoubleBackward(const Array& /*ggx*/, const Array& /*gggamma*/, const Array& /*ggbeta*/) override {
        return {Array{}, Array{}, Array{}};
    }

private:
    // TODO(sonots): Support other than 4, 5 dimensional arrays by reshaping into 4-dimensional arrays as Chainer does.
    cudnnBatchNormMode_t GetBatchNormMode(const Axes& axis) {
        if (axis.ndim() == 1 && axis[0] == 0) {  // (1, channels, (depth, )height, width)
            return CUDNN_BATCHNORM_PER_ACTIVATION;
        }
        if ((axis.ndim() == 3 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3) ||
            (axis.ndim() == 4 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3 && axis[3] == 4)) {  // (1, channels, (1, )1, 1)
            // TODO(hvy): Consider CUDNN_BATCHNORM_SPATIAL_PERSISTENT if we can afford to check for overflow, with or without blocking.
            return CUDNN_BATCHNORM_SPATIAL;
        }
        throw DimensionError{"Invalid axis for BatchNorm using cuDNN ", axis, ". Expected 1, 3 or 4 dimensions."};
    }

    cudnnHandle_t cudnn_handle_;

    // Cache intermediate results during Forward for reuse in Backward.
    Array result_mean_{};
    Array result_inv_var_{};
};

}  // namespace

std::unique_ptr<BatchNormForwardBackward> CudaDevice::GetBatchNormForwardBackward() {
    return std::make_unique<CudaBatchNormForwardBackward>(cudnn_handle());
}

}  // namespace cuda
}  // namespace xchainer
