#include "caffe/common.hpp"
#include "caffe/greentea/greentea.hpp"
#include "caffe/syncedmem.hpp"

#include "caffe/device.hpp"
#include "caffe/util/math_functions.hpp"

#ifdef USE_GREENTEA
#include "caffe/greentea/greentea_im2col.hpp"
#include "caffe/greentea/greentea_math_functions.hpp"

#define ZEROCOPY_SUPPORTED(device, ptr, size, own_cpu_data) \
        (device->is_host_unified()) && (own_cpu_data || \
        (((std::uintptr_t)ptr%OPENCL_PAGE_ALIGN == 0) && \
        (size%OPENCL_CACHE_ALIGN == 0)))
#endif

namespace caffe {

// If CUDA is available and in GPU mode, host memory will be allocated pinned,
// using cudaMallocHost. It avoids dynamic pinning for transfers (DMA).
// The improvement in performance seems negligible in the single GPU case,
// but might be more significant for parallel training. Most importantly,
// it improved stability for large models on many GPUs.

static
void CaffeAlignedMallocHost(void **ptr, int_tp size, device *dev) {

  if (dev->backend() == BACKEND_CPU) {
    #ifdef USE_MKL
    *ptr = mkl_malloc(size ? size:1, 64);
    #else
    *ptr = malloc(size);
    #endif
  }
  #ifdef USE_GREENTEA
  else if (dev->backend() == BACKEND_OpenCL) {
    #ifdef USE_MKL
    #define __ALIGN_MASK(x,mask)    (((x)+(mask))&~(mask))
    #define ALIGN(x,a)              __ALIGN_MASK(x,(int32_t)(a)-1)
    *ptr = mkl_malloc(size ? ALIGN(size, OPENCL_CACHE_ALIGN) : 64, OPENCL_PAGE_ALIGN);
    #undef __ALIGN_MASK
    #undef ALIGN
    #else
    #ifdef _MSC_VER
    *ptr = _aligned_malloc(((size - 1)/OPENCL_CACHE_ALIGN + 1) *
                         OPENCL_CACHE_ALIGN, OPENCL_PAGE_ALIGN);
    #else
    CHECK_EQ(0, posix_memalign(ptr, OPENCL_PAGE_ALIGN,
            ((size - 1)/OPENCL_CACHE_ALIGN + 1) * OPENCL_CACHE_ALIGN))
                << "Host memory allocation error of size: "
                << size << " B";
    #endif  // USE_GREENTEA
    #endif
  }
  #endif
  CHECK(*ptr) << "host allocation of size " << size << " failed";
}

void CaffeAlignedFreeHost(void *ptr, device *dev) {
  if (dev->backend() == BACKEND_CPU) {
    #ifdef USE_MKL
    mkl_free(ptr);
    #else
    free(ptr);
    #endif
  }
  #ifdef USE_GREENTEA
  else if (dev->backend() == BACKEND_OpenCL) {
    #ifdef USE_MKL
    mkl_free(ptr);
    #else
    #ifdef _MSC_VER
    _aligned_free(ptr);
    #else
    free(ptr);
    #endif
    #endif
  }
  #endif
}

void CaffeMallocHost(void** ptr, int_tp size, device* dev) {
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    if (dev->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
      CUDA_CHECK(cudaMallocHost(ptr, size));
      return;
#endif  // USE_CUDA
    } else {
      // Make sure the memory is zero-copy usable in OpenCL
      CaffeAlignedMallocHost(ptr, size, dev);
      return;
    }
  }
#endif
  CaffeAlignedMallocHost(ptr, size, dev);
}

void CaffeFreeHost(void* ptr, device* dev) {
#ifndef CPU_ONLY
  if (Caffe::mode() == Caffe::GPU) {
    if (dev->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
      cudaFreeHost(ptr);
      return;
#endif  // USE_CUDA
    } else {
      CaffeAlignedFreeHost(ptr, dev);
      return;
    }
  }
#endif
  CaffeAlignedFreeHost(ptr, dev);
}


SyncedMemory::~SyncedMemory() {
#ifndef CPU_ONLY
  if (gpu_ptr_ && own_gpu_data_) {
    if (device_->backend() == Backend::BACKEND_CUDA) {
#ifdef USE_CUDA
      // Free device memory
      // Get current device active during call of destructor
      int initial_device;
      cudaGetDevice(&initial_device);
      // We know that this memory blob belongs to the device_
      cudaSetDevice(device_->id());
      cudaFree(gpu_ptr_);
      // Restore current device
      cudaSetDevice(initial_device);
      gpu_ptr_ = nullptr;
      device_->DecreaseMemoryUsage(size_);
#endif  // USE_CUDA
    } else {
#ifdef USE_GREENTEA
      // Free device memory
      viennacl::ocl::context &ctx = viennacl::ocl::get_context(
          device_->id());
      ctx.get_queue().finish();
      CHECK_EQ(CL_SUCCESS, clReleaseMemObject(cl_gpu_mem_))
          << "OpenCL memory corruption";
      gpu_ptr_ = nullptr;
      cl_gpu_mem_ = nullptr;
      ctx.get_queue().finish();
      if (own_zero_copy_data_ && own_cpu_data_ && cpu_ptr_) {
        CaffeFreeHost(cpu_ptr_, device_);
        cpu_ptr_ = nullptr;
      }
      device_->DecreaseMemoryUsage(size_);
#endif  // USE_GREENTEA
    }
  }
#endif  // !CPU_ONLY
  // Free host memory
  if (cpu_ptr_ && own_cpu_data_) {
    CaffeFreeHost(cpu_ptr_, device_);
    cpu_ptr_ = nullptr;
  }
}

inline void SyncedMemory::to_cpu() {
  check_device();
  switch (head_) {
    case UNINITIALIZED: {
      CaffeMallocHost(&cpu_ptr_, size_, device_);
      caffe_memset(size_, 0, cpu_ptr_);
      head_ = HEAD_AT_CPU;
      own_cpu_data_ = true;
      break;
    }
    case HEAD_AT_GPU: {
#ifndef CPU_ONLY
      if (cpu_ptr_ == nullptr) {
        CaffeMallocHost(&cpu_ptr_, size_, device_);
        own_cpu_data_ = true;
#ifdef USE_GREENTEA
        CHECK_EQ(own_zero_copy_data_, false)
           << "Allocate host memory for a zero copy buffer.";
#endif
      }

      if (device_->backend() == Backend::BACKEND_CUDA) {
#ifdef USE_CUDA
        caffe_gpu_memcpy(size_, gpu_ptr_, cpu_ptr_);
#endif  // USE_CUDA
      } else {
#ifdef USE_GREENTEA
        viennacl::ocl::context &ctx = viennacl::ocl::get_context(
            device_->id());
        if (!own_zero_copy_data_) {
          greentea_gpu_memcpy(size_, (cl_mem) gpu_ptr_, 0, cpu_ptr_, &ctx);
        } else {
          void *mapped_ptr = clEnqueueMapBuffer(ctx.get_queue().handle().get(),
                                (cl_mem) gpu_ptr_,
                                true,
                                CL_MAP_READ | CL_MAP_WRITE,
                                0, size_, 0, NULL, NULL, NULL);
          CHECK_EQ(mapped_ptr, cpu_ptr_)
            << "Device claims it support zero copy"
            << " but failed to create correct user ptr buffer";
          clEnqueueUnmapMemObject(ctx.get_queue().handle().get(),
                                  (cl_mem) gpu_ptr_,
                                  mapped_ptr, 0, NULL, NULL);
        }
        ctx.get_queue().finish();
#endif
      }
      head_ = SYNCED;
#else
      NO_GPU;
#endif  // !CPU_ONLY
      break;
    }
    case HEAD_AT_CPU:
    case SYNCED:
      break;
  }
}

inline void SyncedMemory::to_gpu() {
  check_device();
#ifndef CPU_ONLY
  switch (head_) {
    case UNINITIALIZED: {
      if (device_->backend() == Backend::BACKEND_CUDA) {
#ifdef USE_CUDA
        CUDA_CHECK(cudaMalloc(&gpu_ptr_, size_));
        device_->IncreaseMemoryUsage(size_);
        caffe_gpu_memset(size_, 0, gpu_ptr_);
        own_gpu_data_ = true;
#endif  // USE_CUDA
      } else {
#ifdef USE_GREENTEA
        viennacl::ocl::context &ctx = viennacl::ocl::get_context(
            device_->id());
        cl_int err;
        if (ctx.devices()[0].type() == CL_DEVICE_TYPE_CPU) {
          cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(),
                     CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                     size_, nullptr, &err);
        } else if (device_->is_host_unified()) {
            size_t zero_copy_size = (size_ + OPENCL_CACHE_ALIGN - 1)
                                    & ~(OPENCL_CACHE_ALIGN - 1);
            CaffeMallocHost(&cpu_ptr_, zero_copy_size, device_);
            caffe_memset(size_, 0, cpu_ptr_);
            own_cpu_data_ = true;
            cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(),
                              CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                              zero_copy_size, cpu_ptr_, &err);
            void *mapped_ptr = clEnqueueMapBuffer(
                                  ctx.get_queue().handle().get(),
                                  cl_gpu_mem_,
                                  true,
                                  CL_MAP_READ | CL_MAP_WRITE,
                                  0, size_, 0, NULL, NULL, NULL);
            CHECK_EQ(mapped_ptr, cpu_ptr_)
              << "Device claims it support zero copy"
              << " but failed to create correct user ptr buffer";
            clEnqueueUnmapMemObject(ctx.get_queue().handle().get(),
                                    cl_gpu_mem_,
                                    mapped_ptr, 0, NULL, NULL);
            own_zero_copy_data_ = true;
        }

        if (cl_gpu_mem_ == nullptr) {
          cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(),
                                         CL_MEM_READ_WRITE,
                                         size_, nullptr, &err);

          CHECK_EQ(0, err) << "OpenCL buffer allocation of size "
                          << size_ << " failed.";
        }

        device_->IncreaseMemoryUsage(size_);
        if (!own_zero_copy_data_) {
          int_tp alpha = 0;
          greentea_memset(device_->id(), size_, alpha, cl_gpu_mem_, 0);
        }
        gpu_ptr_ = reinterpret_cast<void*>(cl_gpu_mem_);
        own_gpu_data_ = true;
#endif  // USE_GREENTEA
      }
      head_ = HEAD_AT_GPU;
      break;
    }
    case HEAD_AT_CPU: {
      if (device_->backend() == Backend::BACKEND_CUDA) {
#ifdef USE_CUDA
        if (gpu_ptr_ == nullptr) {
          CUDA_CHECK(cudaMalloc(&gpu_ptr_, size_));
          device_->IncreaseMemoryUsage(size_);
        }
        caffe_gpu_memcpy(size_, cpu_ptr_, gpu_ptr_);
        own_gpu_data_ = true;
#endif  // USE_CUDA
      } else {
#ifdef USE_GREENTEA
        viennacl::ocl::context &ctx = viennacl::ocl::get_context(
            device_->id());
        if (gpu_ptr_ == nullptr) {
          cl_int err;
          if (ctx.devices()[0].type() == CL_DEVICE_TYPE_CPU) {
            cl_gpu_mem_ = clCreateBuffer(
                ctx.handle().get(), CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                size_, nullptr, &err);
            CHECK_EQ(0, err) << "Failed to create OpenCL Buffer." << std::endl;
          } else if (ZEROCOPY_SUPPORTED(device_, cpu_ptr_, size_, own_cpu_data_)) {
              size_t aligned_size = ((size_ - 1)/OPENCL_CACHE_ALIGN + 1) *
                                    OPENCL_CACHE_ALIGN;
              cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(),
                               CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                               aligned_size, cpu_ptr_, &err);
              CHECK_EQ(0, err) << "Failed to create OpenCL Buffer." << std::endl;
              void *mapped_ptr = clEnqueueMapBuffer(
                                    ctx.get_queue().handle().get(),
                                    (cl_mem) cl_gpu_mem_,
                                    true,
                                    CL_MAP_READ | CL_MAP_WRITE,
                                    0, size_, 0, NULL, NULL, NULL);
              CHECK_EQ(mapped_ptr, cpu_ptr_)
                << "Device claims it support zero copy"
                << " but failed to create correct user ptr buffer";
              clEnqueueUnmapMemObject(ctx.get_queue().handle().get(),
                                      cl_gpu_mem_,
                                      mapped_ptr, 0, NULL, NULL);
              own_zero_copy_data_ = true;
          }
          if (cl_gpu_mem_ == nullptr) {
            cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(), CL_MEM_READ_WRITE,
                                         size_, nullptr, &err);
            CHECK_EQ(0, err) << "OpenCL buffer allocation of size "
                            << size_ << " failed.";
          }
          device_->IncreaseMemoryUsage(size_);
          gpu_ptr_ = reinterpret_cast<void*>(cl_gpu_mem_);
          // ctx.get_queue().finish();
        }
        if (!own_zero_copy_data_) {
          greentea_gpu_memcpy(size_, cpu_ptr_, (cl_mem) gpu_ptr_, 0, &ctx);
          ctx.get_queue().finish();
        }
        own_gpu_data_ = true;
#endif  // USE_GREENTEA
      }
      head_ = SYNCED;
      break;
    }
    case HEAD_AT_GPU:
    case SYNCED:
      break;
  }
#else
  NO_GPU;
#endif
}

const void* SyncedMemory::cpu_data() {
  check_device();
  to_cpu();
  return (const void*) cpu_ptr_;
}

void SyncedMemory::set_cpu_data(void* data) {
  check_device();
  CHECK(data);
  if (cpu_ptr_ && own_cpu_data_) {
#ifdef USE_GREENTEA
    if (own_zero_copy_data_) {
      own_zero_copy_data_ = false;
      if (own_gpu_data_) {
        CHECK_EQ(CL_SUCCESS, clReleaseMemObject(cl_gpu_mem_))
            << "OpenCL memory corruption";
        cl_gpu_mem_ = NULL;
        gpu_ptr_ = NULL;
      }
    }
#endif
    CaffeFreeHost(cpu_ptr_, device_);
  }
  cpu_ptr_ = data;
  head_ = HEAD_AT_CPU;
  own_cpu_data_ = false;
}

const void* SyncedMemory::gpu_data() {
  check_device();
#ifndef CPU_ONLY
  to_gpu();
  return (const void*) gpu_ptr_;
#else
  NO_GPU;
  return NULL;
#endif
}

void SyncedMemory::set_gpu_data(void* data) {
  check_device();
#ifndef CPU_ONLY
  if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
  CHECK(data);
  if (own_gpu_data_) {
    CUDA_CHECK(cudaFree(gpu_ptr_));
  }
  gpu_ptr_ = data;
  head_ = HEAD_AT_GPU;
  own_gpu_data_ = false;
#endif  // USE_CUDA
  } else {
#ifdef USE_GREENTEA
    CHECK(data);
    if (own_gpu_data_) {
      CHECK_EQ(CL_SUCCESS, clReleaseMemObject(cl_gpu_mem_))
          << "OpenCL memory corruption";
      cl_gpu_mem_ = NULL;
    }
    if (own_zero_copy_data_) {
      own_zero_copy_data_ = false;
    }
    gpu_ptr_ = data;
    head_ = HEAD_AT_GPU;
    own_gpu_data_ = false;
#endif  // USE_GREENTEA
  }
#else
  NO_GPU;
#endif
}

void* SyncedMemory::mutable_cpu_data() {
  check_device();
  to_cpu();
  head_ = HEAD_AT_CPU;
  return cpu_ptr_;
}

void* SyncedMemory::mutable_gpu_data() {
  check_device();
#ifndef CPU_ONLY
  to_gpu();
  head_ = HEAD_AT_GPU;
  return gpu_ptr_;
#else
  NO_GPU;
  return NULL;
#endif
}

// TODO: Implement this function device abstracted
#ifndef CPU_ONLY
#ifdef USE_CUDA
void SyncedMemory::async_gpu_push(const cudaStream_t& stream) {
  check_device();
  CHECK(head_ == HEAD_AT_CPU);
  if (gpu_ptr_ == NULL) {
    CUDA_CHECK(cudaMalloc(&gpu_ptr_, size_));
    own_gpu_data_ = true;
  }
  const cudaMemcpyKind put = cudaMemcpyHostToDevice;
  CUDA_CHECK(cudaMemcpyAsync(gpu_ptr_, cpu_ptr_, size_, put, stream));
  // Assume caller will synchronize on the stream before use
  head_ = SYNCED;
}
#endif  // USE_CUDA

#ifdef USE_GREENTEA
void SyncedMemory::async_gpu_push() {
  check_device();
  CHECK(head_ == HEAD_AT_CPU);
  viennacl::ocl::context &ctx = viennacl::ocl::get_context(
      device_->id());
  if (cl_gpu_mem_ == nullptr) {
    cl_int err;
    cl_gpu_mem_ = clCreateBuffer(ctx.handle().get(), CL_MEM_READ_WRITE,
                                 size_, nullptr, &err);
    CHECK_EQ(0, err) << "OpenCL buffer allocation of size "
                  << size_ << " failed.";
    own_gpu_data_ = true;
    device_->IncreaseMemoryUsage(size_);
    gpu_ptr_ = reinterpret_cast<void*>(cl_gpu_mem_);
  }
  greentea_gpu_memcpy(size_, cpu_ptr_, (cl_mem) gpu_ptr_, 0, &ctx);
  ctx.get_queue().finish();
  head_ = SYNCED;

}
#endif
#endif  // !CPU_ONLY

void SyncedMemory::check_device() {
#ifndef CPU_ONLY
#ifdef DEBUG
  int device;
  cudaGetDevice(&device);
  CHECK(device == device_);
  if (gpu_ptr_ && own_gpu_data_) {
    cudaPointerAttributes attributes;
    CUDA_CHECK(cudaPointerGetAttributes(&attributes, gpu_ptr_));
    CHECK(attributes.device == device_);
  }
#endif
#endif
}
}  // namespace caffe

